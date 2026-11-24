#include "config.h"
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "telegram_ca.h"
#include <time.h>
#include <ArduinoJson.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include "tusb.h"

struct data{
  String name;
  String unit;
  int value;
  int slotAllocation = 1;
};
int currentData = 0;
const int dataLength = 6;
data telemetry[dataLength]{
    {"CPUU", "%", 0, 1}, // CPU Usage
    {"RAMU", "%", 0, 1}, // RAM Usage
    {"GPUU", "%", 0, 1}, // GPU Usage
    {"GPUT", "C", 0, 1}, // GPU Temp
    {"UplS", "KB/s", 0, 2}, // Upload Speed
    {"DwnS", "KB/s", 0, 2} // Download Speed
};

bool telegramConnected = false;
bool awake = false;
String inS = "";
bool commandReady = false;
String command = "";
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_TIMEOUT = 3000;
unsigned long lastFlash = 0;
const unsigned long FLASH_DURATION = 100;
unsigned long lastCycle = 0;
const unsigned long CYCLE_DURATION = 1000;
unsigned long lastTelegramUpdate = 0;
const unsigned long TELEGRAM_UPDATE_INTERVAL = 3000;
long nextUpdateOffset = 0;
bool telegramBootstrapped = false;
time_t telegramSessionStart = 0;
bool wakePending = false;
unsigned long lastWakeAttempt = 0;
const unsigned long WAKE_COOLDOWN = 30000;
USBHIDKeyboard Keyboard;
USBCDC USBSerial;
volatile bool usbSuspended = false;
volatile bool remoteWakeAllowed = false;
bool telemetryMissingShown = false;
bool wifiStatusShown = false;

// LCD display Rotater
LiquidCrystal_I2C lcd(0x27,16,2);

// One Slot is XXXXXXXX
// One Slot Allocation: XXXX:  X
// Two Slot Allocation: XXXX:       XXXX
void displayData(data d) {
  lcd.print(d.name);
  lcd.print(":");
  int leadingSpace{};
  if (d.slotAllocation == 1) {
    leadingSpace = 2;
  } else if (d.slotAllocation == 2) {
    leadingSpace = 7;
  }
  leadingSpace -= String(d.value).length();
  for (int i = 0; i < leadingSpace; i++) lcd.print(" ");
  lcd.print(d.value);
  lcd.print(d.unit);
  if (currentData < dataLength - 1) {
    currentData++;
  } else {
    currentData = 0;
  }
  
}

void LCDCycle() {
  lastCycle = millis();
  lcd.setCursor(0,0); // Slot 0
  if (telemetry[currentData].slotAllocation == 1) {
    displayData(telemetry[currentData]);
    if (telemetry[currentData].slotAllocation == 1) {
      lcd.setCursor(8,0); // Slot 1
      displayData(telemetry[currentData]);
    }
  } else {
    displayData(telemetry[currentData]);
  }
  lcd.setCursor(0,1); // Slot 2
  if (telemetry[currentData].slotAllocation == 1) {
    displayData(telemetry[currentData]);
    if (telemetry[currentData].slotAllocation == 1) {
      lcd.setCursor(8,1); // Slot 3
      displayData(telemetry[currentData]);
    }
  } else {
    displayData(telemetry[currentData]);
  }
}

void displayStatus(const char *line1, const char *line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

bool i2CAddrTest(uint8_t addr) {
  Wire.beginTransmission(addr);
  if (Wire.endTransmission() == 0) {
    return true;
  }
  return false;
}

void enterMonitorMode() {
  wakePending = false;
  telegramConnected = false;
  WiFi.mode(WIFI_OFF);
  USBSerial.println("PC Online");
  awake = true;
  digitalWrite(STATUSLIGHT, LOW);
  lastHeartbeat = 0;
  telemetryMissingShown = false;
  currentData = 0;
}

void enterRemoteMode() {
  wifiStatusShown = false;
  wakePending = false;
  nextUpdateOffset = 0;
  telegramBootstrapped = false;
  telegramConnected = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  USBSerial.println("PC Offline");
  awake = false;
  digitalWrite(STATUSLIGHT, HIGH);
  displayStatus(" PC is Sleeping ", "WiFi Connecting.");
}

bool initializeTelegram() {
  if (WiFi.status() != WL_CONNECTED) return false;

  // Synchronize time for TLS verification
  configTime(0, 0, "pool.ntp.org");

  unsigned long start = millis();
  while (time(nullptr) < 1700000000 &&
          millis() - start < 5000) {
    delay(100);
  }

  if (time(nullptr) < 1700000000) {
    USBSerial.println("NTP synchronization failed");
    return false;
  }

  USBSerial.println("Telegram initialized");
  telegramSessionStart = time(nullptr);
  return true;
}

bool getTelegramUpdates(String &response, long offset) {
  if (WiFi.status() != WL_CONNECTED) return false;
  digitalWrite(WORKLIGHT, HIGH);
  lastFlash = millis();

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_CA);
  client.setTimeout(5000);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(5000);

  String url = String("https://api.telegram.org/bot")
            + TELEGRAM_TOKEN
            + "/getUpdates?timeout=0&limit="
            + (offset == -1 ? "1" : "10")
            + "&offset=" + String(offset);

  if (!http.begin(client, url)) {
    USBSerial.println("HTTP initialization failed");
    return false;
  }

  int status = http.GET();

  if (status == 200) {
    response = http.getString();
  } else {
    USBSerial.printf("Telegram HTTP error %d: %s\n", status, http.errorToString(status).c_str());
    response = "";
  }

  http.end();
  return status == 200;
}


bool processTelegramUpdates(const String &response, bool discard) {
  JsonDocument doc;

  DeserializationError error = deserializeJson(doc, response);
  if (error) {
      USBSerial.printf("JSON error: %s\n", error.c_str());
      return false;
  }

  if (doc["ok"] != true || !doc["result"].is<JsonArray>()) {
      USBSerial.println("Invalid Telegram response");
      return false;
  }

  JsonArray updates = doc["result"].as<JsonArray>();

  for (JsonObject update : updates) {
    long updateId = update["update_id"].as<long>();

    if (updateId >= nextUpdateOffset) {
      nextUpdateOffset = updateId + 1;
    }

    // Discard queued messages on initial connection
    if (discard) continue;

    JsonObject message = update["message"];
    if (message.isNull()) continue;

    int64_t chatId = message["chat"]["id"].as<int64_t>();
    int64_t senderId = message["from"]["id"].as<int64_t>();
    const char *chatType = message["chat"]["type"] | "";
    const char *text = message["text"] | "";

    time_t sentAt = message["date"].as<time_t>();
    time_t now = time(nullptr);

    bool authorized =
      chatId == TELEGRAM_CHAT_ID &&
      senderId == TELEGRAM_CHAT_ID &&
      strcmp(chatType, "private") == 0;

    bool fresh =
      sentAt >= telegramSessionStart &&
      sentAt <= now &&
      now - sentAt <= 30;

    if (!authorized) {
      USBSerial.println("Unauthorized Telegram message");
      displayStatus("  Unauthorized  ", "message ignored.");
      continue;
    }

    if (!fresh) {
      USBSerial.println("Ignored stale Telegram message");
      displayStatus(" Stale Telegram ", "message ignored.");
      continue;
    }

    if (strcmp(text, "/wake") == 0) {
      USBSerial.println("AUTHORIZED WAKE COMMAND RECEIVED!");

      wakePending = true;
      displayStatus(" /wake Received ", " Booting System ");
    } else {
      USBSerial.printf("Unknown command: %s\n", text);
      displayStatus("Unknown Telegram", "message ignored.");
    }
  }

  return true;
}

void usbEventCallback(void *arg,
                      esp_event_base_t event_base,
                      int32_t event_id,
                      void *event_data) {

  if (event_base != ARDUINO_USB_EVENTS) return;

  switch (event_id) {
    case ARDUINO_USB_SUSPEND_EVENT: {
      auto *data = (arduino_usb_event_data_t *)event_data;

      remoteWakeAllowed = data->suspend.remote_wakeup_en;
      usbSuspended = true;
      break;
    }

    case ARDUINO_USB_RESUME_EVENT:
    case ARDUINO_USB_STOPPED_EVENT:
      usbSuspended = false;
      remoteWakeAllowed = false;
      break;
  }
}

void setup() {
  Wire.begin(SDA, SCL);
  if (!i2CAddrTest(0x27)) {
    lcd = LiquidCrystal_I2C(0x3F, 16, 2);
  }
  lcd.init();
  lcd.backlight();
  pinMode(WORKLIGHT, OUTPUT);
  pinMode(STATUSLIGHT, OUTPUT);
  USB.onEvent(usbEventCallback);
  USB.usbAttributes(TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP);
  Keyboard.begin();
  USBSerial.begin(115200);
  USB.begin();
  USBSerial.println("Initialization Complate");
}

void loop() {
  if (usbSuspended && awake) {
    enterRemoteMode();
  } else if (!usbSuspended && tud_mounted() && !awake) {
    enterMonitorMode();
  }
  // always try to read from stdin
  while (USBSerial.available()) {
    char inC = USBSerial.read();
    if (inC == '\n') {
      command = inS;
      commandReady = true;
      inS = "";
      break;
    } else {
      inS += inC;
    }
  }
  if (commandReady) {
    command.trim();
    int count = sscanf(
      command.c_str(),
      "TELEMETRY %d %d %d %d %d %d",
      &(telemetry[0].value), &(telemetry[1].value), &(telemetry[2].value), &(telemetry[3].value), &(telemetry[4].value), &(telemetry[5].value)
    );
    USBSerial.printf("Parsed %d values from: %s\n", count, command.c_str());
    if (count == 6) {
      digitalWrite(WORKLIGHT, HIGH);
      USBSerial.print("Heartbeat arrived in ");
      USBSerial.print(millis() - lastHeartbeat);
      USBSerial.println("ms");
      lastFlash = millis();
      lastHeartbeat = millis();
      if (telemetryMissingShown) {
        lcd.clear();
        currentData = 0;
        lastCycle = millis() - CYCLE_DURATION;
      }
      telemetryMissingShown = false;
    }
    commandReady = false;
  }
  if ((digitalRead(WORKLIGHT) == HIGH) && (millis() - lastFlash >= FLASH_DURATION)) {
    digitalWrite(WORKLIGHT, LOW);
  }
  if (awake) {
    // PC awake: update LCD Board and check for heartbeat
    if (millis() - lastHeartbeat >= HEARTBEAT_TIMEOUT) {
      if (!telemetryMissingShown) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("  No telemetry  ");
        lcd.setCursor(0, 1);
        lcd.print(" data received. ");
        telemetryMissingShown = true;
      }
    } else if (millis() - lastCycle >= CYCLE_DURATION) {
      LCDCycle();
    }
  } else {
    if (wakePending) {
      wakePending = false;

      if (usbSuspended && tud_suspended() && remoteWakeAllowed) {
        if (lastWakeAttempt != 0 && millis() - lastWakeAttempt < WAKE_COOLDOWN) {
          displayStatus("  /wake Failed  ", "Cooldown active.");
          return;
        }

        if (tud_remote_wakeup()) {
          lastWakeAttempt = millis();
          Serial.println("USB remote wakeup requested");
          displayStatus(" RAMMP Systems: ", " Device Booting ");
        } else {
          Serial.println("USB wake request failed");
          displayStatus("  /wake failed  ", " Request error. ");
        }
      } else {
        Serial.println("USB wake unavailable");
        displayStatus("  Wake command  ", " is unavailable ");
      }
      return;
    }
    if (usbSuspended && WiFi.status() == WL_CONNECTED) {
      if (!wifiStatusShown) {
        wifiStatusShown = true;
        displayStatus(" WiFi Connected ", "Booting Telegram");
      }
      if (telegramConnected) {
        // PC Sleeping: call Telegram API, boot PC when message received, LCD board activation sequence
        if (millis() - lastTelegramUpdate >= TELEGRAM_UPDATE_INTERVAL) {
          String response;
          lastTelegramUpdate = millis();
          long offset = telegramBootstrapped ? nextUpdateOffset : -1;
          if (getTelegramUpdates(response, offset)) {
            if (processTelegramUpdates(response, !telegramBootstrapped)) {
              if (!telegramBootstrapped) {
                telegramBootstrapped = true;
                USBSerial.println("Old Telegram messages discarded");
                displayStatus(" Telegram Ready ", "   Send /wake   ");
              }
            }
          }
        }
      } else {
        telegramConnected = initializeTelegram();
      }
    }
  }
}
