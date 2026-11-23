#include "config.h"
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "telegram_ca.h"
#include <time.h>
#include <ArduinoJson.h>

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
bool relayActive = false;
unsigned long relayStarted = 0;
unsigned long lastWakeAttempt = 0;
const unsigned long RELAY_DURATION = 250;
const unsigned long WAKE_COOLDOWN = 30000;

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

bool i2CAddrTest(uint8_t addr) {
  Wire.beginTransmission(addr);
  if (Wire.endTransmission() == 0) {
    return true;
  }
  return false;
}

void enterMonitorMode() {
  wakePending = false;
  digitalWrite(RELAY, LOW);
  relayActive = false;
  telegramConnected = false;
  WiFi.mode(WIFI_OFF);
  Serial.println("PC Online");
  awake = true;
  digitalWrite(STATUSLIGHT, LOW);
}

void enterRemoteMode() {
  wakePending = false;
  digitalWrite(RELAY, LOW);
  relayActive = false;
  nextUpdateOffset = 0;
  telegramBootstrapped = false;
  telegramConnected = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("PC Offline");
  awake = false;
  digitalWrite(STATUSLIGHT, HIGH);
  lcd.setCursor(0,0);
  lcd.print("The monitored PC");
  lcd.setCursor(0,1);
  lcd.print("   is offline   ");
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
    Serial.println("NTP synchronization failed");
    return false;
  }

  Serial.println("Telegram initialized");
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
    Serial.println("HTTP initialization failed");
    return false;
  }

  int status = http.GET();

  if (status == 200) {
    response = http.getString();
  } else {
    Serial.printf("Telegram HTTP error %d: %s\n", status, http.errorToString(status).c_str());
    response = "";
  }

  http.end();
  return status == 200;
}


bool processTelegramUpdates(const String &response, bool discard) {
  JsonDocument doc;

  DeserializationError error = deserializeJson(doc, response);
  if (error) {
      Serial.printf("JSON error: %s\n", error.c_str());
      return false;
  }

  if (doc["ok"] != true || !doc["result"].is<JsonArray>()) {
      Serial.println("Invalid Telegram response");
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
      Serial.println("Unauthorized Telegram message");
      continue;
    }

    if (!fresh) {
      Serial.println("Ignored stale Telegram message");
      continue;
    }

    if (strcmp(text, "/wake") == 0) {
      Serial.println("AUTHORIZED WAKE COMMAND RECEIVED!");

      wakePending = true;
    } else {
      Serial.printf("Unknown command: %s\n", text);
    }
  }

  return true;
}

void setup() {
  Wire.begin(SDA, SCL);
  if (!i2CAddrTest(0x27)) {
    lcd = LiquidCrystal_I2C(0x3F, 16, 2);
  }
  digitalWrite(RELAY, LOW);
  lcd.init();
  lcd.backlight();
  pinMode(WORKLIGHT, OUTPUT);
  pinMode(STATUSLIGHT, OUTPUT);
  pinMode(RELAY, OUTPUT);
  Serial.begin(115200);
  Serial.println("Initialization Complate");
  enterRemoteMode();
}

void loop() {
  // always try to read from stdin
  while (Serial.available()) {
    char inC = Serial.read();
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
    Serial.printf("Parsed %d values from: %s\n", count, command.c_str());
    if (count == 6) {
      digitalWrite(WORKLIGHT, HIGH);
      Serial.print("Heartbeat arrived in ");
      Serial.print(millis() - lastHeartbeat);
      Serial.println("ms");
      lastFlash = millis();
      lastHeartbeat = millis();
      if (!awake) {
        enterMonitorMode();
      }
    }
    commandReady = false;
  }
  if ((digitalRead(WORKLIGHT) == HIGH) && (millis() - lastFlash >= FLASH_DURATION)) {
    digitalWrite(WORKLIGHT, LOW);
  }
  if (awake) {
    if (relayActive) {
      digitalWrite(RELAY, LOW);
      relayActive = false;
      lastWakeAttempt = millis();
    }
    // PC awake: update LCD Board and check for heartbeat
    if (millis() - lastCycle >= CYCLE_DURATION) {
      LCDCycle();
    }
    if ((millis() - lastHeartbeat) >= HEARTBEAT_TIMEOUT) {
      enterRemoteMode();
    }
  } else {
    if (relayActive && (millis() - relayStarted >= RELAY_DURATION)) {
      digitalWrite(RELAY, LOW);
      relayActive = false;
      lastWakeAttempt = millis();
      wakePending = false;
      return;
    }
    if (wakePending && (millis() - lastWakeAttempt >= WAKE_COOLDOWN)) {
      digitalWrite(RELAY, HIGH);
      relayActive = true;
      relayStarted = millis();
      wakePending = false;
      return;
    }
    if (!relayActive && WiFi.status() == WL_CONNECTED) {
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
                Serial.println("Old Telegram messages discarded");
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
