#include "vouchers.h"
#include <LittleFS.h>
#include <time.h>
#include "store.h"

namespace {
  JsonDocument doc;            // {"codes":[{"code","drink","size","price","name","at"}]}
  int wrong = 0; uint32_t lockUntil = 0;

  void save() {
    File f = LittleFS.open("/vouchers.json", "w");
    if (f) { serializeJson(doc, f); f.close(); }
  }
  void expire() {
    time_t now = time(nullptr);
    if (now < 1700000000) return;                       // no clock yet: keep codes
    long maxAge = (long)(store::settings["codeHours"] | 12) * 3600;
    JsonArray a = doc["codes"];
    for (int i = (int)a.size() - 1; i >= 0; i--) {
      long at = a[i]["at"] | 0L;
      if (at > 1700000000 && now - at > maxAge) a.remove(i);
    }
  }
  JsonObject find(const char* code) {
    for (JsonObject v : doc["codes"].as<JsonArray>()) if (strcmp(v["code"] | "", code) == 0) return v;
    return JsonObject();
  }
}

namespace vouchers {
  void begin() {
    File f = LittleFS.open("/vouchers.json", "r");
    if (f) { deserializeJson(doc, f); f.close(); }
    if (doc["codes"].isNull()) doc["codes"].to<JsonArray>();
  }

  bool create(const char* drink, const char* size, String& code, String& error) {
    JsonObject d;
    for (JsonObject x : store::recipes["drinks"].as<JsonArray>()) if (strcmp(x["id"] | "", drink) == 0) d = x;
    if (d.isNull() || !(d["enabled"] | true)) { error = "That drink is not on the menu"; return false; }
    if (d["prices"][size].isNull()) { error = "That size is not sold"; return false; }
    expire();
    JsonArray a = doc["codes"];
    while (a.size() >= 40) a.remove(0);                 // oldest unused code drops off
    do { code = String(1000 + esp_random() % 9000); } while (!find(code.c_str()).isNull());
    JsonObject v = a.add<JsonObject>();
    v["code"] = code; v["drink"] = drink; v["size"] = size; v["name"] = d["name"];
    v["price"] = d["prices"][size]; v["at"] = (long)time(nullptr);
    save();
    return true;
  }

  bool cancel(const char* code) {
    JsonArray a = doc["codes"];
    for (size_t i = 0; i < a.size(); i++) if (strcmp(a[i]["code"] | "", code) == 0) { a.remove(i); save(); return true; }
    return false;
  }

  bool lockedOut() { return millis() < lockUntil; }

  bool check(const char* code, String& drink, String& size, String& error) {
    if (lockedOut()) { error = "Too many wrong codes. Ask at the counter."; return false; }
    expire();
    JsonObject v = find(code);
    if (v.isNull()) {
      if (++wrong >= 5) { wrong = 0; lockUntil = millis() + 60000; }
      error = "That code didn't work. Check it with the cashier.";
      return false;
    }
    wrong = 0;
    drink = (const char*)v["drink"]; size = (const char*)v["size"];
    return true;
  }

  void markUsed(const char* code) { cancel(code); }

  void list(JsonArray out) {
    expire();
    for (JsonObject v : doc["codes"].as<JsonArray>()) out.add(v);
  }
}
