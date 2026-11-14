#include "web.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"
#include "io.h"
#include "plant.h"
#include "vouchers.h"
#include "machine.h"
#include "store.h"

namespace {
  WebServer server(80);
  int badPins = 0; uint32_t lockoutUntil = 0;
  uint32_t doorUnlockUntil = 0;
  bool restartPending = false; uint32_t restartAt = 0;

  void cors() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-Admin-Pin, X-Counter-Pin");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
    server.sendHeader("Cache-Control", "no-store");
  }
  void sendJson(JsonDocument& doc, int code = 200) {
    String body; serializeJson(doc, body);
    cors(); server.send(code, "application/json", body);
  }
  void sendError(const String& msg, int code = 400) {
    JsonDocument d; d["ok"] = false; d["error"] = msg; sendJson(d, code);
  }
  void sendOk() { JsonDocument d; d["ok"] = true; sendJson(d); }
  bool body(JsonDocument& doc) {
    if (deserializeJson(doc, server.arg("plain"))) { sendError("Request body is not valid JSON"); return false; }
    return true;
  }
  bool admin() {
    if (millis() < lockoutUntil) { sendError("Too many wrong PINs. Wait a minute and try again.", 429); return false; }
    if (server.header("X-Admin-Pin") == store::adminPin()) { badPins = 0; return true; }
    if (++badPins >= 5) { lockoutUntil = millis() + 60000; badPins = 0; }
    sendError("Wrong PIN", 401); return false;
  }

  // ---------------- customer endpoints (tablet) ----------------
  void hMenu()   { JsonDocument d; machine::menu(d.to<JsonObject>()); sendJson(d); }
  void hStatus() { JsonDocument d; machine::status(d.to<JsonObject>()); sendJson(d); }
  void hOrder() {
    JsonDocument b; if (!body(b)) return;
    String err;
    if (!machine::placeOrder(b["drink"] | "", b["size"] | "12", b["sugar"] | 0, b["code"] | "", false, err)) { sendError(err); return; }
    sendOk();
  }
  void hRedeem() {                       // customer typed a counter code: tell the screen which drink it is
    JsonDocument b; if (!body(b)) return;
    String drink, size, err;
    if (!vouchers::check(b["code"] | "", drink, size, err)) { sendError(err); return; }
    JsonDocument d; d["ok"] = true; d["drink"] = drink; d["size"] = size; sendJson(d);
  }

  // ---------------- counter screen (cashier) ----------------
  bool counter() {
    if (millis() < lockoutUntil) { sendError("Too many wrong PINs. Wait a minute and try again.", 429); return false; }
    String p = server.header("X-Counter-Pin");
    if (p.length() && (p == store::settings["counterPin"].as<String>() || p == store::adminPin())) { badPins = 0; return true; }
    if (++badPins >= 5) { lockoutUntil = millis() + 60000; badPins = 0; }
    sendError("Wrong PIN", 401); return false;
  }
  void hCounterList() {
    if (!counter()) return;
    JsonDocument d; machine::menu(d["menu"].to<JsonObject>()); vouchers::list(d["codes"].to<JsonArray>());
    d["freeMode"] = store::settings["freeVend"] | false;
    plant::bottleStatus(d["water"].to<JsonObject>());
    sendJson(d);
  }
  void hCounterBottles() {               // cashier: "Bottles swapped" after putting in 2 full bottles
    if (!counter()) return;
    plant::bottlesSwapped();
    JsonDocument d; d["ok"] = true; plant::bottleStatus(d["water"].to<JsonObject>()); sendJson(d);
  }
  void hCounterCreate() {
    if (!counter()) return;
    JsonDocument b; if (!body(b)) return;
    String code, err;
    if (!vouchers::create(b["drink"] | "", b["size"] | "12", code, err)) { sendError(err); return; }
    JsonDocument d; d["ok"] = true; d["code"] = code; sendJson(d);
  }
  void hCounterCancel() {
    if (!counter()) return;
    JsonDocument b; if (!body(b)) return;
    vouchers::cancel(b["code"] | ""); sendOk();
  }
  void hCancel() { machine::cancelOrder(); sendOk(); }
  void hTablet() {
    JsonDocument b; if (!body(b)) return;
    plant::tabletBattery(b["battery"] | -1, b["charging"] | false);
    sendOk();
  }

  // ---------------- owner endpoints (phone app) ----------------
  void hSummary() {
    if (!admin()) return;
    JsonDocument d;
    machine::status(d["status"].to<JsonObject>());
    JsonObject t = d["temps"].to<JsonObject>();
    auto temp = [&](const char* k, uint8_t n) { float v = io::tempC(n); if (isnan(v)) t[k] = nullptr; else t[k] = round(v * 10) / 10; };
    temp("espresso", NTC_THERMOBLOCK); temp("boiler", NTC_BOILER); temp("cold", NTC_COLD); temp("cabinet", NTC_CABINET);
    uint32_t f = plant::faults();
    d["faults"] = plant::faultText(f);
    d["faultBits"] = f;
    String why; d["selling"] = !plant::salesBlocked(why); if (why.length()) d["blockedReason"] = why;
    d["inventory"] = store::inventory;
    d["stats"] = store::stats;
    d["testMode"] = store::settings["freeVend"] | false;
    d["firmware"] = FW_VERSION;
    d["uptimeMin"] = millis() / 60000;
    d["wifi"]["ap"] = store::settings["apSsid"];
    d["wifi"]["store"] = store::settings["staSsid"];
    d["wifi"]["storeConnected"] = WiFi.status() == WL_CONNECTED;
    if (WiFi.status() == WL_CONNECTED) d["wifi"]["storeIp"] = WiFi.localIP().toString();
    d["settings"]["rinseHour"] = store::settings["rinseHour"];
    d["settings"]["lids"] = store::settings["lids"];
    d["settings"]["bottled"] = store::settings["bottled"] | false;
    d["settings"]["codeHours"] = store::settings["codeHours"] | 12;
    d["settings"]["moving"] = store::settings["moving"] | false;
    d["settings"]["chiller"] = store::settings["chiller"] | false;
    d["settings"]["bottleSensors"] = store::settings["bottleSensors"] | false;
    d["settings"]["paused"] = store::settings["paused"] | false;
    d["settings"]["cupMode"] = store::settings["cupMode"] | "turret";
    plant::bottleStatus(d["water"].to<JsonObject>());
    d["io"]["relays"] = io::relaysOk(); d["io"]["inputs"] = io::inputsOk(); d["io"]["addons"] = io::extOk();
    vouchers::list(d["openCodes"].to<JsonArray>());
    d["time"] = store::timestamp();
    sendJson(d);
  }
  void hGetRecipes() { if (!admin()) return; sendJson(store::recipes); }
  void hPutRecipes() {
    if (!admin()) return;
    String err;
    if (!store::saveRecipes(server.arg("plain"), err)) { sendError(err); return; }
    sendOk();
  }
  void hInventory() {
    if (!admin()) return;
    JsonDocument b; if (!body(b)) return;
    for (JsonPair p : b.as<JsonObject>()) {
      const char* k = p.key().c_str();
      if (!strcmp(k, "beans_g") || !strcmp(k, "lids") || !strcmp(k, "water_ml") || !strcmp(k, "canisters_g") || !strcmp(k, "syrups_ml") || !strcmp(k, "cups"))
        store::inventory[k] = p.value();
    }
    store::saveInventory();
    sendOk();
  }
  void hSettings() {
    if (!admin()) return;
    JsonDocument b; if (!body(b)) return;
    bool wifiChanged = false;
    const char* allowed[] = {"apSsid", "apPass", "staSsid", "staPass", "adminPin", "counterPin", "freeVend", "rinseHour", "tz", "lids", "bottled", "codeHours", "chiller", "bottleSensors", "paused", "cupMode"};
    for (const char* k : allowed) {
      if (b[k].isNull()) continue;
      if ((!strcmp(k, "adminPin") || !strcmp(k, "counterPin")) && strlen(b[k] | "") < 4) { sendError("PIN must be at least 4 digits"); return; }
      if (!strcmp(k, "apPass") && strlen(b[k] | "") < 8) { sendError("Wi-Fi password must be at least 8 characters"); return; }
      if (!strncmp(k, "ap", 2) || !strncmp(k, "sta", 3)) wifiChanged = true;
      if (!strcmp(k, "cupMode") && strcmp(b[k] | "", "turret") && strcmp(b[k] | "", "droppers")) { sendError("cupMode must be turret or droppers"); return; }
      store::settings[k] = b[k];
    }
    store::saveSettings();
    if (wifiChanged) { restartPending = true; restartAt = millis() + 1500; }
    JsonDocument d; d["ok"] = true; d["restarting"] = wifiChanged; sendJson(d);
  }
  void hAction() {
    if (!admin()) return;
    JsonDocument b; if (!body(b)) return;
    String a = b["action"] | "", err;
    bool ok = true;
    if (a == "rinse") ok = machine::startRinse(err);
    else if (a == "testVend") ok = machine::testVend(b["drink"] | "", b["size"] | "12", err);
    else if (a == "unlockDoor") web::unlockDoor();
    else if (a == "clearFaults") plant::clearLatched();
    else if (a == "bottlesSwapped") plant::bottlesSwapped();
    else if (a == "prime") plant::requestPrime();
    else if (a == "dispense") ok = machine::dispense(b["target"] | "", b["index"] | 0, b["seconds"] | 0.0f, err);
    else if (a == "cancel") machine::cancelOrder();
    else if (a == "moveMode") {                 // get ready to move / back in service
      bool on = b["on"] | true;
      if (on && machine::busy()) { ok = false; err = "Wait until the current drink is finished"; }
      else { store::settings["moving"] = on; store::saveSettings(); if (!on) { plant::clearLatched(); plant::requestPrime(); } }
    }
    else if (a == "resetStats") { store::stats.clear(); store::stats["count"] = 0; store::stats["revenueCents"] = 0; store::stats["perDrink"].to<JsonObject>(); store::stats["days"].to<JsonObject>(); store::saveStats(); }
    else if (a == "restart") { restartPending = true; restartAt = millis() + 1000; }
    else { ok = false; err = "Unknown action"; }
    if (ok) sendOk(); else sendError(err);
  }
  void hSales() {
    if (!admin()) return;
    File f = LittleFS.open("/sales.csv", "r");
    cors();
    if (!f) { server.send(200, "text/csv", "time,drink,size,price_cents,payment,result\n"); return; }
    server.streamFile(f, "text/csv"); f.close();
  }

  void hNotFound() {
    if (server.method() == HTTP_OPTIONS) { cors(); server.send(204); return; }
    String path = server.uri();
    if (path == "/owner" || path == "/owner/") path = "/owner.html";
    if (path == "/counter" || path == "/counter/") path = "/counter.html";
    if (path.endsWith("/")) path += "index.html";
    if (LittleFS.exists(path)) {
      File f = LittleFS.open(path, "r");
      String type = path.endsWith(".html") ? "text/html" : path.endsWith(".json") || path.endsWith(".webmanifest") ? "application/json"
                  : path.endsWith(".svg") ? "image/svg+xml" : path.endsWith(".png") ? "image/png" : "text/plain";
      server.sendHeader("Cache-Control", "no-cache");
      server.streamFile(f, type); f.close(); return;
    }
    sendError("Not found", 404);
  }
}

namespace web {
  void begin() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(store::settings["apSsid"].as<const char*>(), store::settings["apPass"].as<const char*>());
    String sta = store::settings["staSsid"] | "";
    if (sta.length()) WiFi.begin(sta.c_str(), store::settings["staPass"] | "");
    MDNS.begin("coffee");
    configTzTime(store::settings["tz"] | "EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");

    const char* hdrs[] = {"X-Admin-Pin", "X-Counter-Pin"};
    server.collectHeaders(hdrs, 2);
    server.on("/api/menu", HTTP_GET, hMenu);
    server.on("/api/status", HTTP_GET, hStatus);
    server.on("/api/order", HTTP_POST, hOrder);
    server.on("/api/cancel", HTTP_POST, hCancel);
    server.on("/api/tablet", HTTP_POST, hTablet);
    server.on("/api/redeem", HTTP_POST, hRedeem);
    server.on("/api/counter/list", HTTP_GET, hCounterList);
    server.on("/api/counter/create", HTTP_POST, hCounterCreate);
    server.on("/api/counter/cancel", HTTP_POST, hCounterCancel);
    server.on("/api/counter/bottles", HTTP_POST, hCounterBottles);
    server.on("/api/admin/summary", HTTP_GET, hSummary);
    server.on("/api/admin/recipes", HTTP_GET, hGetRecipes);
    server.on("/api/admin/recipes", HTTP_PUT, hPutRecipes);
    server.on("/api/admin/recipes", HTTP_POST, hPutRecipes);
    server.on("/api/admin/inventory", HTTP_POST, hInventory);
    server.on("/api/admin/settings", HTTP_POST, hSettings);
    server.on("/api/admin/action", HTTP_POST, hAction);
    server.on("/api/admin/sales.csv", HTTP_GET, hSales);
    server.onNotFound(hNotFound);
    server.begin();
  }

  void handle() {
    server.handleClient();
    if (doorUnlockUntil && millis() > doorUnlockUntil) { io::set(OUT_DOOR_LOCK, false); doorUnlockUntil = 0; }
    if (restartPending && millis() > restartAt) ESP.restart();
  }

  void unlockDoor() { io::set(OUT_DOOR_LOCK, true); doorUnlockUntil = millis() + 3000; }
}
