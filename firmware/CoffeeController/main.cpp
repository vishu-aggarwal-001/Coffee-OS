// ============================================================================
//  Coffee vending machine controller - main loop
//  Board: KinCony KC868-A32 (ESP32).  Build: Arduino IDE / arduino-cli, board
//  "ESP32 Dev Module", partition "Default 4MB with spiffs". Upload the data/
//  folder to LittleFS (see README).
// ============================================================================
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "config.h"
#include "io.h"
#include "store.h"
#include "plant.h"
#include "vouchers.h"
#include "machine.h"
#include "web.h"

static uint32_t lastRinseDay = 0, lastSave = 0, svcPressedAt = 0;

static void dailyRinse() {
  if (machine::busy()) return;
  int hour = store::settings["rinseHour"] | 3;
  time_t now = time(nullptr);
  uint32_t dayKey;
  if (now > 1700000000) {
    struct tm t; localtime_r(&now, &t);
    if (t.tm_hour != hour) return;
    dayKey = t.tm_year * 400 + t.tm_yday;
  } else {
    dayKey = millis() / 86400000UL + 1;          // no clock: once per 24 h of uptime
  }
  if (dayKey == lastRinseDay) return;
  String err;
  if (machine::startRinse(err)) lastRinseDay = dayKey;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nCoffee controller " FW_VERSION);
  io::begin();
  if (!store::begin()) Serial.println("WARNING: recipes.json missing - upload the data folder");
  plant::begin();
  vouchers::begin();
  machine::begin();
  web::begin();
  esp_task_wdt_init(5, true);                      // reboot (outputs off) if the loop ever hangs
  esp_task_wdt_add(NULL);
  Serial.println("Ready. Tablet: http://192.168.4.1/  Owner: /owner  Counter: /counter");
}

void loop() {
  esp_task_wdt_reset();
  io::update();
  plant::update();
  machine::update();
  web::handle();
  dailyRinse();

  // Service button inside the door: hold 3 s to clear latched faults and re-prime
  if (io::in(IN_SERVICE_BTN)) { if (!svcPressedAt) svcPressedAt = millis(); else if (millis() - svcPressedAt > 3000) { plant::clearLatched(); plant::requestPrime(); svcPressedAt = 0xFFFFFFFF; } }
  else svcPressedAt = 0;

  if (millis() - lastSave > 300000) { lastSave = millis(); if (!machine::busy()) store::saveInventory(); }
  delay(2);
}
