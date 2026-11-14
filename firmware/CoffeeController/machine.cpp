#include "machine.h"
#include "io.h"
#include "plant.h"
#include "vouchers.h"
#include "store.h"

namespace {
  enum Op : uint8_t { OP_GRIND, OP_BREW, OP_MIX, OP_WATER, OP_SYRUP, OP_WAIT };
  struct Step {
    Op op; uint8_t idx = 0;      // mixer (1-3) or syrup pump (1-2)
    float ml = 0, g = 0, whipS = 0, sec = 0;
    float powder[4] = {0, 0, 0, 0};   // grams from canisters 1-4 (mix step)
    bool cold = false;
  };
  enum Phase { PH_IDLE, PH_PAYING, PH_CUP, PH_NOZZLE_OUT, PH_STEPS, PH_LID, PH_NOZZLE_BACK, PH_READY, PH_FAILED };

  Phase phase = PH_IDLE;
  Step steps[16]; int nSteps = 0, cur = 0, sub = 0;
  uint32_t phaseAt = 0, subAt = 0, orderStartedAt = 0, estimateMs = 0;
  uint32_t endValve = 0, endWhip = 0, endAuger[4] = {0, 0, 0, 0};
  uint32_t flowTarget = 0;
  String orderDrink, orderSize, orderName, failMsg, label, orderCode;
  int orderPrice = 0, cupTries = 0;
  bool orderNoCup = false, orderNoPayment = false, orderIsRinse = false;

  float rate(const char* key, int i, float fb) { return store::calArr(key, i, fb); }
  float sizeFactor(const String& size) { return store::recipes["sizeFactors"][size] | 1.0f; }

  JsonObject findDrink(const char* id) {
    for (JsonObject d : store::recipes["drinks"].as<JsonArray>()) if (strcmp(d["id"] | "", id) == 0) return d;
    return JsonObject();
  }

  // Build the step list for a drink at a size, with sugar level 0-3.
  int build(JsonObject d, const String& size, int sugar, Step* out) {
    int n = 0; float f = sizeFactor(size);
    for (JsonObject s : d["steps"].as<JsonArray>()) {
      if (n >= 14) break;
      const char* op = s["op"] | "";
      bool scale = s["scale"] | (strcmp(op, "espresso") != 0);
      float k = scale ? f : 1.0f;
      if (!strcmp(op, "espresso")) {
        if (!scale && size == "16" && (s["double16"] | true)) k = 1.6f;   // stronger shot in the big cup
        Step a; a.op = OP_GRIND; a.g = (s["g"] | 9.0f) * k; out[n++] = a;
        Step b; b.op = OP_BREW; b.ml = (s["ml"] | 40.0f) * k; out[n++] = b;
      } else if (!strcmp(op, "mix")) {
        Step m; m.op = OP_MIX; m.idx = s["mixer"] | 1; m.ml = (s["ml"] | 150.0f) * k;
        m.whipS = s["whipS"] | 5.0f; m.cold = !strcmp(s["water"] | "hot", "cold");
        for (JsonPair p : s["powders"].as<JsonObject>()) { int c = atoi(p.key().c_str()); if (c >= 1 && c <= 4) m.powder[c - 1] = p.value().as<float>() * k; }
        out[n++] = m;
      } else if (!strcmp(op, "water")) {
        Step w; w.op = OP_WATER; w.ml = (s["ml"] | 200.0f) * k; w.cold = !strcmp(s["water"] | "hot", "cold"); out[n++] = w;
      } else if (!strcmp(op, "syrup")) {
        Step y; y.op = OP_SYRUP; y.idx = s["pump"] | 1; y.ml = (s["ml"] | 15.0f) * k; out[n++] = y;
      } else if (!strcmp(op, "wait")) {
        Step w; w.op = OP_WAIT; w.sec = s["s"] | 1.0f; out[n++] = w;
      }
    }
    if (sugar > 0) {
      float g = sugar * store::cal("sugarPerLevelG", 3.5f);
      bool added = false;
      if (!added) for (int i = 0; i < n; i++) if (out[i].op == OP_MIX) { out[i].powder[0] += g; added = true; break; }
      if (!added && n < 16) { Step m; m.op = OP_MIX; m.idx = 1; m.ml = 25; m.whipS = 3; m.powder[0] = g; out[n++] = m; }
    }
    return n;
  }

  uint32_t stepMs(const Step& s) {
    switch (s.op) {
      case OP_GRIND: return 1000 * s.g / rate("grinderGps", 0, 1.6f) + 500;
      case OP_BREW:  return 3000 + 1000 * s.ml / 2.0f + 4000;
      case OP_MIX: { float t = s.ml / rate(s.cold ? "coldMlps" : "mixerMlps", s.cold ? 0 : s.idx - 1, 12); return 1000 * (t + s.whipS); }
      case OP_WATER: return 1000 * s.ml / rate(s.cold ? "coldMlps" : "spoutMlps", 0, 15);
      case OP_SYRUP: return 1000 * s.ml / rate("syrupMlps", s.idx - 1, 1.5f);
      case OP_WAIT:  return 1000 * s.sec;
    }
    return 0;
  }

  // What a drink needs, used for availability checks.
  bool canMake(JsonObject d, const String& size, String& why) {
    Step tmp[16]; int n = build(d, size, 0, tmp);
    float need[4] = {0, 0, 0, 0}, beans = 0, syr[2] = {0, 0};
    bool espresso = false, hot = false, cold = false;
    for (int i = 0; i < n; i++) {
      const Step& s = tmp[i];
      if (s.op == OP_GRIND) beans += s.g;
      if (s.op == OP_BREW) espresso = true;
      if (s.op == OP_MIX) { for (int c = 0; c < 4; c++) need[c] += s.powder[c]; if (s.cold) cold = true; else hot = true; }
      if (s.op == OP_WATER) { if (s.cold) cold = true; else hot = true; }
      if (s.op == OP_SYRUP && s.idx >= 1 && s.idx <= 2) syr[s.idx - 1] += s.ml;
    }
    JsonObject inv = store::inventory.as<JsonObject>();
    float water = 0;
    for (int i = 0; i < n; i++) if (tmp[i].op == OP_BREW || tmp[i].op == OP_MIX || tmp[i].op == OP_WATER) water += tmp[i].ml;
    if (plant::waterEstimateUsed() && (float)(inv["water_ml"] | 0) < water + 300) { why = "Out of water"; return false; }
    if (beans > 0 && (float)inv["beans_g"] < beans) { why = "Out of coffee beans"; return false; }
    for (int c = 0; c < 4; c++) if (need[c] > 0 && (float)inv["canisters_g"][c] < need[c]) { why = "Out of an ingredient"; return false; }
    for (int p = 0; p < 2; p++) if (syr[p] > 0 && (float)inv["syrups_ml"][p] < syr[p]) { why = "Out of flavor syrup"; return false; }
    if ((int)(inv["cups"][size] | 0) <= 0) { why = "Out of this cup size"; return false; }
    if (espresso && (plant::faults() & plant::F_BREW_GROUP)) { why = "Coffee unit needs service"; return false; }
    if (espresso && !plant::brewReady()) { why = "Heating up"; return false; }
    if (hot && !plant::hotReady()) { why = "Heating up"; return false; }
    if (cold && !plant::coldReady()) { why = "Chilling"; return false; }
    return true;
  }

  void deduct(const Step& s) {
    JsonObject inv = store::inventory.as<JsonObject>();
    if (s.op == OP_BREW || s.op == OP_MIX || s.op == OP_WATER) inv["water_ml"] = max(0.0f, (float)(inv["water_ml"] | 0) - s.ml);
    if (s.op == OP_GRIND) inv["beans_g"] = max(0.0f, (float)inv["beans_g"] - s.g);
    if (s.op == OP_MIX) for (int c = 0; c < 4; c++) if (s.powder[c] > 0) inv["canisters_g"][c] = max(0.0f, (float)inv["canisters_g"][c] - s.powder[c]);
    if (s.op == OP_SYRUP && s.idx >= 1 && s.idx <= 2) inv["syrups_ml"][s.idx - 1] = max(0.0f, (float)inv["syrups_ml"][s.idx - 1] - s.ml);
  }

  void go(Phase p) { phase = p; phaseAt = millis(); sub = 0; subAt = millis(); }

  void fail(const String& why) {
    io::allMotorsAndValvesOff(); plant::pumpOn(false); plant::setBrewActive(false);
    failMsg = why;
    if (!orderIsRinse)   // the counter code stays valid so the customer can try again
      store::logSale(orderDrink.c_str(), orderSize.c_str(), orderPrice, orderCode.length() ? "counter" : "test", false);
    go(PH_FAILED);
  }

  const char* labelFor(const Step& s) {
    switch (s.op) {
      case OP_GRIND: return "Grinding fresh beans";
      case OP_BREW:  return "Brewing espresso";
      case OP_MIX:   return s.cold ? "Mixing it cold" : "Steaming it together";
      case OP_WATER: return s.cold ? "Pouring chilled water" : "Pouring hot water";
      case OP_SYRUP: return "Adding flavor";
      case OP_WAIT:  return "Settling";
    }
    return "";
  }

  // Runs the current step. Returns true when the step has finished.
  bool runStep(Step& s) {
    uint32_t now = millis();
    switch (s.op) {
      case OP_WAIT:
        return now - subAt >= s.sec * 1000;

      case OP_GRIND:
        if (sub == 0) {
          if (!io::in(IN_GROUP_HOME)) { io::groupMotor(-1); if (now - subAt > MOTOR_TIMEOUT_MS) { plant::raise(plant::F_BREW_GROUP); fail("Coffee unit did not open"); } return false; }
          io::groupMotor(0); io::set(OUT_GRINDER, true); sub = 1; subAt = now; return false;
        }
        if (now - subAt >= 1000 * s.g / rate("grinderGps", 0, 1.6f)) { io::set(OUT_GRINDER, false); return true; }
        return false;

      case OP_BREW:
        if (sub == 0) {                                   // wait for heat and pump rest
          if (plant::brewReady() && plant::pumpAllowed()) { io::groupMotor(+1); sub = 1; subAt = now; }
          else if (now - subAt > 90000) fail("Espresso heater not ready");
          return false;
        }
        if (sub == 1) {                                   // close the brew chamber
          if (io::in(IN_GROUP_PRESSED)) {
            io::groupMotor(0); plant::setBrewActive(true);
            io::resetFlow(); flowTarget = s.ml * store::cal("flowPulsesPerMl", FLOW_PULSES_PER_ML);
            io::set(OUT_BREW_VALVE, true); plant::pumpOn(true); sub = 2; subAt = now;
          } else if (now - subAt > MOTOR_TIMEOUT_MS) { io::groupMotor(0); plant::raise(plant::F_BREW_GROUP); fail("Coffee unit did not close"); }
          return false;
        }
        if (sub == 2) {                                   // pump until the flow meter reaches the shot volume
          if (io::flowPulses() >= flowTarget || now - subAt > 45000) {
            plant::pumpOn(false); io::set(OUT_BREW_VALVE, false); plant::setBrewActive(false); sub = 3; subAt = now;
          }
          return false;
        }
        if (sub == 3) { if (now - subAt > 1500) { io::groupMotor(-1); sub = 4; subAt = now; } return false; }
        if (sub == 4) {                                   // open and eject the puck
          if (io::in(IN_GROUP_HOME)) { io::groupMotor(0); return true; }
          if (now - subAt > MOTOR_TIMEOUT_MS) { io::groupMotor(0); plant::raise(plant::F_BREW_GROUP); return true; }  // drink is done; flag for service
          return false;
        }
        return false;

      case OP_MIX: {
        if (sub == 0) {
          uint8_t valve = s.cold ? (uint8_t)OUT_COLD_VALVE : (uint8_t)(OUT_MIXER1_VALVE + s.idx - 1);
          float waterS = s.ml / rate(s.cold ? "coldMlps" : "mixerMlps", s.cold ? 0 : s.idx - 1, 12);
          io::set(valve, true); endValve = now + waterS * 1000;
          for (int c = 0; c < 4; c++) {
            endAuger[c] = 0;
            if (s.powder[c] > 0) { io::set(OUT_AUGER1 + c, true); endAuger[c] = now + 1000 * s.powder[c] / rate("canisterGps", c, 1.6f); }
          }
          io::set(OUT_WHIP1 + s.idx - 1, true); endWhip = endValve + s.whipS * 1000;
          sub = 1; return false;
        }
        bool done = true;
        if (now >= endValve) { io::set(OUT_COLD_VALVE, false); io::set(OUT_MIXER1_VALVE + s.idx - 1, false); } else done = false;
        for (int c = 0; c < 4; c++) { if (endAuger[c] && now >= endAuger[c]) { io::set(OUT_AUGER1 + c, false); endAuger[c] = 0; } if (endAuger[c]) done = false; }
        if (now >= endWhip) io::set(OUT_WHIP1 + s.idx - 1, false); else done = false;
        return done;
      }

      case OP_WATER: {
        uint8_t valve = s.cold ? (uint8_t)OUT_COLD_VALVE : (uint8_t)OUT_SPOUT_VALVE;
        if (sub == 0) { io::set(valve, true); endValve = now + 1000 * s.ml / rate(s.cold ? "coldMlps" : "spoutMlps", 0, 15); sub = 1; return false; }
        if (now >= endValve) { io::set(valve, false); return true; }
        return false;
      }

      case OP_SYRUP: {
        uint8_t pump = OUT_SYRUP1 + s.idx - 1;
        if (sub == 0) { io::set(pump, true); endValve = now + 1000 * s.ml / rate("syrupMlps", s.idx - 1, 1.5f); sub = 1; return false; }
        if (now >= endValve) { io::set(pump, false); return true; }
        return false;
      }
    }
    return true;
  }

  int barrelFor(const String& size) { return store::recipes["calibration"]["sizeBarrel"][size] | 1; }
  bool droppers() { return String(store::settings["cupMode"] | "turret") == "droppers"; }
  uint8_t dropperOut(const String& size) {
    int b = constrain(barrelFor(size), 1, 3);
    return b == 1 ? OUT_CUP_TURRET : b == 2 ? OUT_CUP_DROP : OUT_LID_DROP;
  }

  void startSteps(bool noCup) {
    cur = 0; orderStartedAt = millis();
    estimateMs = noCup ? 0 : 9000;
    for (int i = 0; i < nSteps; i++) estimateMs += stepMs(steps[i]);
    go(noCup ? PH_STEPS : PH_CUP);
  }
}

namespace machine {
  void begin() { go(PH_IDLE); }
  bool busy() { return phase != PH_IDLE; }

  bool placeOrder(const char* drinkId, const char* size, int sugar, const char* code, bool bypassCode, String& error) {
    if (phase != PH_IDLE) { error = "The machine is busy"; return false; }
    String why;
    if (plant::salesBlocked(why)) { error = why; return false; }
    JsonObject d = findDrink(drinkId);
    if (d.isNull() || !(d["enabled"] | true)) { error = "That drink is not available"; return false; }
    if (d["prices"][size].isNull()) { error = "That size is not available"; return false; }
    if (!canMake(d, size, why)) { error = why; return false; }
    bool free = store::settings["freeVend"] | false;
    orderCode = "";
    if (!free && !bypassCode) {
      String vd, vs;
      if (!vouchers::check(code ? code : "", vd, vs, error)) return false;
      if (vd != drinkId || vs != size) { error = "That code is for a different drink or size"; return false; }
      orderCode = code;
    }
    if (sugar > 0 && !(d["sugar"] | true)) sugar = 0;
    if (sugar > 3) sugar = 3;
    orderDrink = drinkId; orderSize = size; orderName = (const char*)(d["name"] | "Drink");
    orderPrice = (int)round((float)d["prices"][size] * 100);
    nSteps = build(d, size, sugar, steps);
    orderNoCup = false; orderIsRinse = false; failMsg = "";
    orderNoPayment = orderCode.length() == 0;     // test, free or owner test drink: no sale value
    startSteps(false);
    return true;
  }

  bool testVend(const char* drinkId, const char* size, String& error) {
    return placeOrder(drinkId, size, 0, "", true, error);
  }

  void cancelOrder() { }

  bool startRinse(String& error) {
    if (phase != PH_IDLE) { error = "The machine is busy"; return false; }
    if (plant::faults() & plant::CRITICAL) { error = plant::faultText(plant::faults() & plant::CRITICAL); return false; }
    nSteps = 0;
    for (int m = 1; m <= 3; m++) { Step s; s.op = OP_MIX; s.idx = m; s.ml = 60; s.whipS = 6; steps[nSteps++] = s; }
    if (plant::brewReady()) { Step b; b.op = OP_BREW; b.ml = 30; steps[nSteps++] = b; }
    orderIsRinse = true; orderNoPayment = true; orderDrink = "rinse"; orderName = "Rinse";
    startSteps(true);
    return true;
  }

  bool dispense(const char* target, int index, float seconds, String& error) {
    if (phase != PH_IDLE) { error = "The machine is busy"; return false; }
    if (seconds <= 0 || seconds > 10) { error = "Use 1 to 10 seconds"; return false; }
    Step s; s.op = OP_WAIT; s.sec = seconds; nSteps = 0;
    uint8_t out = 0;
    if (!strcmp(target, "canister") && index >= 1 && index <= 4) out = OUT_AUGER1 + index - 1;
    else if (!strcmp(target, "grinder")) out = OUT_GRINDER;
    else if (!strcmp(target, "syrup") && index >= 1 && index <= 2) out = OUT_SYRUP1 + index - 1;
    else if (!strcmp(target, "mixer") && index >= 1 && index <= 3) out = OUT_MIXER1_VALVE + index - 1;
    else if (!strcmp(target, "spout")) out = OUT_SPOUT_VALVE;
    else if (!strcmp(target, "cold")) out = OUT_COLD_VALVE;
    if (!out) { error = "Unknown target"; return false; }
    io::set(out, true);
    steps[nSteps++] = s;
    orderIsRinse = true; orderNoPayment = true; orderDrink = "calibrate"; orderName = "Calibration";
    startSteps(true);
    return true;
  }

  void update() {
    uint32_t now = millis();
    switch (phase) {
      case PH_IDLE: io::set(OUT_PICKUP_LIGHT, false); break;

      case PH_PAYING: go(PH_IDLE); break;

      case PH_CUP: {                                     // rotate turret to the right barrel, drop a cup
        uint32_t idxMs = store::cal("cupTurretIndexMs", 900);
        if (droppers()) {                                // v8 printed droppers: barrel 1/2/3 = OUT 23/24/25, no turret
          uint8_t out = dropperOut(orderSize);
          if (sub == 0) { io::set(out, true); sub = 3; subAt = now; }
          else if (sub == 3) {
            if (io::in(IN_CUP_DROPPED) || now - subAt > (uint32_t)store::cal("dropperMaxMs", 1500)) { io::set(out, false); sub = 4; subAt = now; }
          } else if (sub == 4) {
            if (io::in(IN_CUP_PRESENT)) {
              JsonObject inv = store::inventory.as<JsonObject>();
              inv["cups"][orderSize] = max(0, (int)inv["cups"][orderSize] - 1);
              cupTries = 0; go(PH_NOZZLE_OUT);
            } else if (now - subAt > 2500) {
              if (++cupTries < 2) { sub = 0; }
              else { cupTries = 0; plant::raise(plant::F_CUP_JAM); fail("No cup dropped. Your counter code still works."); }
            }
          }
          break;
        }
        if (sub == 0) { io::set(OUT_CUP_TURRET, true); sub = 1; subAt = now; }
        else if (sub == 1) { if (io::in(IN_TURRET_HOME) || now - subAt > 6000) { sub = 2; subAt = now; } }
        else if (sub == 2) { if (now - subAt >= (uint32_t)(barrelFor(orderSize) - 1) * idxMs) { io::set(OUT_CUP_TURRET, false); io::set(OUT_CUP_DROP, true); sub = 3; subAt = now; } }
        else if (sub == 3) {
          if (io::in(IN_CUP_DROPPED) || now - subAt > 3000) { io::set(OUT_CUP_DROP, false); sub = 4; subAt = now; }
        } else if (sub == 4) {
          if (io::in(IN_CUP_PRESENT)) {
            JsonObject inv = store::inventory.as<JsonObject>();
            inv["cups"][orderSize] = max(0, (int)inv["cups"][orderSize] - 1);
            cupTries = 0; go(PH_NOZZLE_OUT);
          } else if (now - subAt > 2500) {
            if (++cupTries < 2) { sub = 2; subAt = now; io::set(OUT_CUP_TURRET, false); }
            else { cupTries = 0; plant::raise(plant::F_CUP_JAM); fail("No cup dropped. Your counter code still works."); }
          }
        }
        break;
      }

      case PH_NOZZLE_OUT:
        if (sub == 0) { io::nozzleMotor(+1); sub = 1; }
        if (io::in(IN_NOZZLE_FRONT) || now - phaseAt > MOTOR_TIMEOUT_MS) { io::nozzleMotor(0); go(PH_STEPS); }
        break;

      case PH_STEPS:
        if (cur >= nSteps) { go(orderNoCup ? PH_NOZZLE_BACK : (orderIsRinse ? PH_NOZZLE_BACK : PH_LID)); break; }
        label = labelFor(steps[cur]);
        if (orderIsRinse && orderDrink == "rinse") label = "Rinsing";
        if (!(plant::faults() & plant::CRITICAL) || orderIsRinse) {
          if (runStep(steps[cur])) {
            if (!orderIsRinse) deduct(steps[cur]);
            else if (steps[cur].op == OP_MIX || steps[cur].op == OP_BREW) store::inventory["water_ml"] = max(0.0f, (float)(store::inventory["water_ml"] | 0) - steps[cur].ml);
            cur++; sub = 0; subAt = now;
          }
        } else fail(plant::faultText(plant::faults() & plant::CRITICAL));
        if (orderIsRinse && orderDrink == "calibrate" && cur >= nSteps) io::allMotorsAndValvesOff();
        break;

      case PH_LID:
        if (droppers() || !(store::settings["lids"] | true) || (int)(store::inventory["lids"] | 0) <= 0) { go(PH_NOZZLE_BACK); break; }
        if (sub == 0) { io::set(OUT_LID_DROP, true); sub = 1; }
        if (io::in(IN_LID_DROPPED) || now - phaseAt > 3000) {
          io::set(OUT_LID_DROP, false);
          store::inventory["lids"] = max(0, (int)store::inventory["lids"] - 1);
          go(PH_NOZZLE_BACK);
        }
        break;

      case PH_NOZZLE_BACK:
        if (sub == 0) { io::nozzleMotor(-1); sub = 1; }
        if (io::in(IN_NOZZLE_BACK) || now - phaseAt > MOTOR_TIMEOUT_MS) {
          io::nozzleMotor(0);
          if (orderIsRinse) { io::allMotorsAndValvesOff(); go(PH_IDLE); break; }
          if (orderCode.length()) vouchers::markUsed(orderCode.c_str());
          store::logSale(orderDrink.c_str(), orderSize.c_str(), orderNoPayment ? 0 : orderPrice,
                         orderCode.length() ? "counter" : "test", true);
          store::saveInventory();
          go(PH_READY);
        }
        break;

      case PH_READY:                                     // light the pick-up door until the cup is taken
        io::set(OUT_PICKUP_LIGHT, (now / 500) % 2);
        if (sub == 0 && !io::in(IN_CUP_PRESENT)) { sub = 1; subAt = now; }
        if (sub == 1 && io::in(IN_CUP_PRESENT)) sub = 0;
        if ((sub == 1 && now - subAt > 1500) || now - phaseAt > 120000) go(PH_IDLE);
        break;

      case PH_FAILED:
        if (now - phaseAt > 8000) go(PH_IDLE);
        break;
    }
  }

  void menu(JsonObject out) {
    out["brand"] = store::recipes["brand"];
    out["currency"] = store::recipes["currency"] | "USD";
    out["sugarLevels"] = 3;
    out["payMode"] = (store::settings["freeVend"] | false) ? "free" : "counter";
    String why; bool blocked = plant::salesBlocked(why);
    out["open"] = !blocked; if (blocked) out["reason"] = why;
    JsonArray arr = out["drinks"].to<JsonArray>();
    bool hasChiller = store::settings["chiller"] | false;
    for (JsonObject d : store::recipes["drinks"].as<JsonArray>()) {
      if (!(d["enabled"] | true)) continue;
      if (!hasChiller && strcmp(d["temp"] | "hot", "cold") == 0) continue;
      JsonObject o = arr.add<JsonObject>();
      o["id"] = d["id"]; o["name"] = d["name"]; o["desc"] = d["desc"]; o["temp"] = d["temp"] | "hot";
      o["sugar"] = d["sugar"] | true; o["layers"] = d["layers"]; if (!d["fill"].isNull()) o["fill"] = d["fill"];
      JsonObject pr = o["prices"].to<JsonObject>();
      JsonObject av = o["available"].to<JsonObject>();
      for (JsonPair p : d["prices"].as<JsonObject>()) {
        pr[p.key()] = p.value();
        String w; bool ok = canMake(d, p.key().c_str(), w);
        if (ok) av[p.key()] = true; else av[p.key()] = w;
      }
    }
  }

  void status(JsonObject out) {
    static const char* names[] = {"idle", "paying", "cup", "starting", "making", "lid", "finishing", "ready", "failed"};
    out["phase"] = names[phase];
    out["drink"] = orderName; out["size"] = orderSize; out["price"] = orderPrice / 100.0f;
    if (phase == PH_CUP) out["label"] = "Dropping your cup";
    if (phase == PH_STEPS || phase == PH_NOZZLE_OUT) out["label"] = label.length() ? label : String("Getting ready");
    if (phase == PH_LID || phase == PH_NOZZLE_BACK) out["label"] = "Almost done";
    if (phase == PH_FAILED) out["message"] = failMsg;
    if (phase >= PH_CUP && phase <= PH_READY) {
      uint32_t el = millis() - orderStartedAt;
      float p = estimateMs ? (float)el / estimateMs : 0;
      out["progress"] = phase == PH_READY ? 1.0f : min(0.97f, p);
    }
    out["step"] = cur; out["steps"] = nSteps;
  }
}
