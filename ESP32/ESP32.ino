#include "config.h"
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

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

void setup() {
  Wire.begin(SDA, SCL);
  if (!i2CAddrTest(0x27)) {
    lcd = LiquidCrystal_I2C(0x3F, 16, 2);
  }
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("The monitored PC");
  lcd.setCursor(0,1);
  lcd.print("   is offline   ");
  pinMode(WORKLIGHT, OUTPUT);
  pinMode(STATUSLIGHT, OUTPUT);
  Serial.begin(115200);
  digitalWrite(STATUSLIGHT, HIGH);
  Serial.println("Initialization Complate");
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
      awake = true;
      digitalWrite(STATUSLIGHT, LOW);
    }
    commandReady = false;
  }
  if ((digitalRead(WORKLIGHT) == HIGH) && (millis() - lastFlash >= FLASH_DURATION)) {
    digitalWrite(WORKLIGHT, LOW);
  }
  if (awake) {
    // PC awake: update LCD Board and check for heartbeat
    if (millis() - lastCycle >= CYCLE_DURATION) {
      LCDCycle();
    }
    if ((millis() - lastHeartbeat) >= HEARTBEAT_TIMEOUT) {
      Serial.println("PC Dead");
      awake = false;
      digitalWrite(STATUSLIGHT, HIGH);
      lcd.setCursor(0,0);
      lcd.print("The monitored PC");
      lcd.setCursor(0,1);
      lcd.print("   is offline   ");
    }
  } else {
    // PC Sleeping: call Telegram API, boot PC when message received, LCD board activation sequence
    
  }
}
