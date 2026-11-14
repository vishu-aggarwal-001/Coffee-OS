#pragma once
#include <ArduinoJson.h>

// Persistent data on the LittleFS partition:
//   /recipes.json   drinks, prices, ingredients, calibration (editable from the owner app)
//   /settings.json  Wi-Fi, admin PIN, test mode, rinse time
//   /inventory.json remaining beans, powders, syrups, cups, lids
//   /stats.json     sales counters
//   /sales.csv      one line per sale
namespace store {
  extern JsonDocument recipes;
  extern JsonDocument settings;
  extern JsonDocument inventory;
  extern JsonDocument stats;

  bool begin();
  bool saveRecipes(const String& json, String& error);   // validates before saving
  void saveSettings();
  void saveInventory();
  void saveStats();
  void logSale(const char* drinkId, const char* size, int priceCents, const char* method, bool ok);
  String timestamp();

  float cal(const char* key, float fallback);
  float calArr(const char* key, int idx, float fallback);
  String adminPin();
}
