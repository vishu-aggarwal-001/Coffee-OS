#include "store.h"
#include <LittleFS.h>
#include <time.h>

namespace store {
  JsonDocument recipes, settings, inventory, stats;

  static bool loadJson(const char* path, JsonDocument& doc) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    return !e;
  }
  static void saveJson(const char* path, JsonDocument& doc) {
    String tmp = String(path) + ".tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f) return;
    serializeJson(doc, f);
    f.close();
    LittleFS.remove(path);
    LittleFS.rename(tmp, path);         // atomic-ish: never leaves a half-written file
  }

  static void defaultsSettings() {
    if (settings["apSsid"].isNull())   settings["apSsid"] = "CoffeeMachine-" + String((uint32_t)ESP.getEfuseMac(), HEX).substring(0, 4);
    if (settings["apPass"].isNull())   settings["apPass"] = "change-me-123";
    if (settings["staSsid"].isNull())  settings["staSsid"] = "";
    if (settings["staPass"].isNull())  settings["staPass"] = "";
    if (settings["adminPin"].isNull()) settings["adminPin"] = "000000";
    if (settings["freeVend"].isNull()) settings["freeVend"] = true;    // test mode: no counter code needed
    if (settings["counterPin"].isNull()) settings["counterPin"] = "1234";
    if (settings["codeHours"].isNull()) settings["codeHours"] = 12;   // counter codes expire after this
    if (settings["rinseHour"].isNull()) settings["rinseHour"] = 3;
    if (settings["tz"].isNull())       settings["tz"] = "EST5EDT,M3.2.0,M11.1.0";
    if (settings["lids"].isNull())     settings["lids"] = true;
    if (settings["bottled"].isNull())  settings["bottled"] = true;     // portable: water from 2 bottles in the base
    if (settings["moving"].isNull())   settings["moving"] = false;
    if (settings["chiller"].isNull())  settings["chiller"] = false;    // optional add-on: off = no iced drinks
    if (settings["bottleSensors"].isNull()) settings["bottleSensors"] = false;  // turn on once the 2 level sensors are wired
    if (settings["paused"].isNull())   settings["paused"] = false;     // true = closed: no sales, heaters stay on
    if (settings["cupMode"].isNull())  settings["cupMode"] = "turret"; // "droppers" = 3 printed droppers on OUT 23/24/25
  }
  static void defaultsInventory() {
    if (inventory["beans_g"].isNull()) inventory["beans_g"] = 0;
    if (inventory["canisters_g"].isNull()) { JsonArray a = inventory["canisters_g"].to<JsonArray>(); for (int i = 0; i < 4; i++) a.add(0); }
    if (inventory["syrups_ml"].isNull())   { JsonArray a = inventory["syrups_ml"].to<JsonArray>(); a.add(0); a.add(0); }
    if (inventory["cups"].isNull()) { JsonObject c = inventory["cups"].to<JsonObject>(); c["10"] = 0; c["12"] = 0; c["16"] = 0; }
    if (inventory["lids"].isNull()) inventory["lids"] = 0;
    if (inventory["water_ml"].isNull()) inventory["water_ml"] = 0;
  }
  static void defaultsStats() {
    if (stats["count"].isNull()) stats["count"] = 0;
    if (stats["revenueCents"].isNull()) stats["revenueCents"] = 0;
    if (stats["perDrink"].isNull()) stats["perDrink"].to<JsonObject>();
    if (stats["days"].isNull()) stats["days"].to<JsonObject>();
  }

  bool begin() {
    if (!LittleFS.begin(true)) return false;
    loadJson("/recipes.json", recipes);
    loadJson("/settings.json", settings);   defaultsSettings();
    loadJson("/inventory.json", inventory); defaultsInventory();
    loadJson("/stats.json", stats);         defaultsStats();
    return !recipes["drinks"].isNull();
  }

  bool saveRecipes(const String& json, String& error) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, json);
    if (e) { error = String("Not valid JSON: ") + e.c_str(); return false; }
    JsonArray drinks = doc["drinks"];
    if (drinks.isNull() || drinks.size() == 0) { error = "Add at least one drink."; return false; }
    for (JsonObject d : drinks) {
      if (d["id"].isNull() || d["name"].isNull()) { error = "Every drink needs an id and a name."; return false; }
      if (d["steps"].isNull() || d["steps"].size() == 0) { error = String("Drink ") + (const char*)d["name"] + " has no steps."; return false; }
      if (d["steps"].size() > 12) { error = String("Drink ") + (const char*)d["name"] + " has more than 12 steps."; return false; }
      if (d["prices"].isNull()) { error = String("Drink ") + (const char*)d["name"] + " has no prices."; return false; }
    }
    if (doc["calibration"].isNull()) doc["calibration"] = recipes["calibration"];   // keep calibration if omitted
    recipes = doc;
    saveJson("/recipes.json", recipes);
    return true;
  }
  void saveSettings()  { saveJson("/settings.json", settings); }
  void saveInventory() { saveJson("/inventory.json", inventory); }
  void saveStats()     { saveJson("/stats.json", stats); }

  String timestamp() {
    time_t now = time(nullptr);
    if (now < 1700000000) return String("uptime+") + String(millis() / 1000) + "s";
    struct tm t; localtime_r(&now, &t);
    char buf[24]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    return String(buf);
  }

  void logSale(const char* drinkId, const char* size, int priceCents, const char* method, bool ok) {
    File f = LittleFS.open("/sales.csv", "a");
    if (f) {
      if (f.size() == 0) f.println("time,drink,size,price_cents,payment,result");
      f.printf("%s,%s,%s,%d,%s,%s\n", timestamp().c_str(), drinkId, size, priceCents, method, ok ? "ok" : "failed");
      f.close();
    }
    if (!ok) return;
    stats["count"] = (long)stats["count"] + 1;
    stats["revenueCents"] = (long)stats["revenueCents"] + priceCents;
    stats["perDrink"][drinkId] = (long)(stats["perDrink"][drinkId] | 0) + 1;
    String day = timestamp().substring(0, 10);
    JsonObject days0 = stats["days"];
    JsonObject d = days0[day];
    if (d.isNull()) d = days0[day].to<JsonObject>();
    d["count"] = (long)(d["count"] | 0) + 1;
    d["revenueCents"] = (long)(d["revenueCents"] | 0) + priceCents;
    // keep 60 days of history
    JsonObject days = stats["days"];
    while (days.size() > 60) { auto it = days.begin(); days.remove(it->key()); }
    saveStats();
  }

  float cal(const char* key, float fallback) {
    JsonVariant v = recipes["calibration"][key];
    return v.isNull() ? fallback : v.as<float>();
  }
  float calArr(const char* key, int idx, float fallback) {
    JsonVariant v = recipes["calibration"][key][idx];
    return v.isNull() ? fallback : v.as<float>();
  }
  String adminPin() { return settings["adminPin"].as<String>(); }
}
