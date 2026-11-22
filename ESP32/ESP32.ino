#include "config.h"

bool awake = false;
String inS = "";
bool commandReady = false;
String command = "";
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_TIMEOUT = 3000;
unsigned long lastFlash = 0;
const unsigned long FLASH_DURATION = 100;

void setup() {
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
    // later add input sanitation and input validity check
    command.trim();
    if (command == "skibidi") {
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
    if ((millis() - lastHeartbeat) >= HEARTBEAT_TIMEOUT) {
      Serial.println("PC Dead");
      awake = false;
      digitalWrite(STATUSLIGHT, HIGH);
    }
  } else {
    // PC Sleeping: call Telegram API, boot PC when message received, LCD board activation sequence

  }
}
