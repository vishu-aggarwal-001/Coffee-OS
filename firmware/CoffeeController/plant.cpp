#include "plant.h"
#include "io.h"
#include "store.h"

namespace {
  uint32_t faultBits = 0;
  bool brewActive = false;
  // heaters: time-proportioning with a small PI loop
  struct Heater { float integ = 0; float duty = 0; } hThermo, hBoiler;
  uint32_t thermoWindowStart = 0, boilerWindowStart = 0, boilerOnMs = 0;
  // water bottles (v1.1.0): per-bottle empty state with 3 s confirmation both ways
  bool bottleEmpty[2] = {false, false}; uint32_t bottleChangeSince[2] = {0, 0}; bool wasEmpty = false;
  // boiler fill
  bool filling = false; uint32_t fillStart = 0, fillCoveredAt = 0;
  // pump duty
  bool pumpIsOn = false; uint32_t pumpOnAt = 0, pumpOffAt = 0, pumpRunAccum = 0;
  // chiller
  bool chillerOn = false; uint32_t chillerOffAt = 0;
  // tablet
  int tabletPct = -1; uint32_t tabletAt = 0;
  // priming
  enum PrimeState { PRIME_NEEDED, PRIME_RUNNING, PRIME_DONE } prime = PRIME_NEEDED;
  uint32_t primeStart = 0; uint32_t primePulses0 = 0;

  float pi(Heater& h, float target, float actual) {
    if (isnan(actual)) { h.integ = 0; return 0; }
    float err = target - actual;
    h.integ += err * 0.02f;
    if (h.integ > 0.4f) h.integ = 0.4f;
    if (h.integ < -0.2f) h.integ = -0.2f;
    float out = err * 0.12f + h.integ;
    if (out < 0) out = 0; if (out > 1) out = 1;
    return out;
  }
}

namespace plant {
  void begin() { thermoWindowStart = boilerWindowStart = millis(); pumpOffAt = millis() - PUMP_MIN_REST_MS; chillerOffAt = millis() - CHILLER_MIN_OFF_MS; }
  uint32_t faults() { return faultBits; }
  void raise(uint32_t f) { faultBits |= f; }
  void clearLatched() { faultBits &= ~LATCHED; if (prime != PRIME_DONE) prime = PRIME_NEEDED; }

  String faultText(uint32_t f) {
    String s;
    auto add = [&](uint32_t bit, const char* t) { if (f & bit) { if (s.length()) s += "; "; s += t; } };
    add(F_LEAK, "Water leak detected - water shut off");
    add(F_OVERTEMP, "Heater over temperature");
    add(F_PROBE_THERMO, "Espresso heater probe failed");
    add(F_PROBE_BOILER, "Boiler probe failed");
    add(F_FILL_TIMEOUT, "Boiler did not fill - check the water bottles");
    add(F_PRIME_FAILED, "No water flow to espresso heater");
    add(F_NO_WATER, "Out of water - change the bottles");
    add(F_WASTE_FULL, "Wastewater bucket is full");
    add(F_NO_GROUNDS_BIN, "Grounds bin is missing");
    add(F_DOOR_OPEN, "Door is open");
    add(F_BREW_GROUP, "Brew group did not move");
    add(F_CUP_JAM, "Cup did not drop");
    add(F_PROBE_COLD, "Chiller probe failed");
    add(F_CABINET_HOT, "Cabinet is hot - check fans");
    add(F_MOVING, "Out of service: getting ready to move");
    add(F_IO_BUS, "Controller can't reach its relay, sensor or add-on boards - check the I2C wiring");
    return s;
  }

  void setBrewActive(bool on) { brewActive = on; }
  bool pumpAllowed() {
    if (pumpIsOn) return millis() - pumpOnAt < PUMP_MAX_RUN_MS;
    return millis() - pumpOffAt >= PUMP_MIN_REST_MS || pumpRunAccum < 20000;
  }
  void pumpOn(bool on) {
    if (on && !pumpIsOn) { pumpIsOn = true; pumpOnAt = millis(); }
    if (!on && pumpIsOn) { pumpIsOn = false; pumpOffAt = millis(); pumpRunAccum = millis() - pumpOnAt; }
    io::set(OUT_PUMP, on);
    if (store::settings["bottled"] | false) io::set(OUT_BOTTLE_PUMP, on);
  }
  void tabletBattery(int pct, bool) { tabletPct = pct; tabletAt = millis(); }
  void requestPrime() { prime = PRIME_NEEDED; }
  bool primed() { return prime == PRIME_DONE; }

  bool hotReady() {
    float t = io::tempC(NTC_BOILER);
    return io::in(IN_BOILER_LEVEL) && !isnan(t) && t >= (float)(store::recipes["temps"]["boilerC"] | 95) - 4;
  }
  bool brewReady() {
    float t = io::tempC(NTC_THERMOBLOCK);
    return prime == PRIME_DONE && !isnan(t) && t >= (float)(store::recipes["temps"]["thermoblockC"] | 100) - 6;
  }
  bool coldReady() {
    if (!(store::settings["chiller"] | false)) return false;   // no chiller fitted
    float t = io::tempC(NTC_COLD);
    return !(faultBits & F_PROBE_COLD) && !isnan(t) && t <= (float)(store::recipes["temps"]["coldC"] | 6) + 4;
  }

  bool salesBlocked(String& why) {
    uint32_t blocking = CRITICAL | F_NO_WATER | F_WASTE_FULL | F_NO_GROUNDS_BIN | F_DOOR_OPEN | F_CUP_JAM | F_MOVING;
    if (faultBits & blocking) { why = faultText(faultBits & blocking); return true; }
    if (store::settings["paused"] | false) { why = "Closed"; return true; }   // owner pressed Close (or the CoffeeOS schedule did)
    if (!hotReady()) { why = "Heating up"; return true; }
    return false;
  }

  bool waterEstimateUsed() {
    return (store::settings["bottled"] | false) && !(store::settings["bottleSensors"] | false);
  }

  void bottlesSwapped() {
    store::inventory["water_ml"] = BOTTLE_PAIR_ML;
    store::saveInventory();
    faultBits &= ~F_NO_WATER;          // re-checked on the next loop
  }

  void bottleStatus(JsonObject out) {
    bool bottled = store::settings["bottled"] | false;
    bool sensors = bottled && (store::settings["bottleSensors"] | false);
    float ml = store::inventory["water_ml"] | 0.0f;
    out["bottled"] = bottled;
    out["sensors"] = sensors;
    out["ml"] = (int)ml;
    out["fullMl"] = (int)BOTTLE_PAIR_ML;
    out["empty"] = bottled && (faultBits & F_NO_WATER);
    out["low"] = bottled && !(faultBits & F_NO_WATER) && !sensors && ml < 3000;
    if (sensors) { out["bottle1"] = !bottleEmpty[0]; out["bottle2"] = !bottleEmpty[1]; }
  }

  void update() {
    uint32_t now = millis();
    float tT = io::tempC(NTC_THERMOBLOCK), tB = io::tempC(NTC_BOILER), tC = io::tempC(NTC_COLD), tCab = io::tempC(NTC_CABINET);
    bool bottled = store::settings["bottled"] | false;

    // ---- live (non-latched) conditions
    auto setIf = [&](uint32_t bit, bool cond) { if (cond) faultBits |= bit; else faultBits &= ~bit; };
    setIf(F_PROBE_THERMO, isnan(tT));
    setIf(F_PROBE_BOILER, isnan(tB));
    bool hasChiller = store::settings["chiller"] | false;
    setIf(F_PROBE_COLD, hasChiller && isnan(tC));
    bool moving = store::settings["moving"] | false;
    setIf(F_MOVING, moving);
    bool sensors = bottled && (store::settings["bottleSensors"] | false);
    for (int i = 0; i < 2; i++) {                      // stick-on level sensors, 3 s confirmation both ways
      bool reading = !io::in(i == 0 ? IN_BOTTLE1_OK : IN_BOTTLE2_OK);   // true = reads empty
      if (reading == bottleEmpty[i]) bottleChangeSince[i] = 0;
      else if (!bottleChangeSince[i]) bottleChangeSince[i] = now ? now : 1;
      else if (now - bottleChangeSince[i] > BOTTLE_EMPTY_CONFIRM_MS) { bottleEmpty[i] = reading; bottleChangeSince[i] = 0; }
    }
    if (sensors) {
      bool anyEmpty = bottleEmpty[0] || bottleEmpty[1];  // bottles are teed together: one empty lets air in
      if (wasEmpty && !anyEmpty) bottlesSwapped();      // full bottles detected: reset the count automatically
      wasEmpty = anyEmpty;
      setIf(F_NO_WATER, anyEmpty);
    }
    else if (bottled) setIf(F_NO_WATER, (float)(store::inventory["water_ml"] | 0) < 300);
    else              setIf(F_NO_WATER, !io::in(IN_INLET_PRESSURE));
    if (now > 3000) setIf(F_IO_BUS, !io::relaysOk() || !io::inputsOk() || !io::extOk());   // 3 s grace after boot
    setIf(F_WASTE_FULL, io::in(IN_WASTE_FULL));
    setIf(F_NO_GROUNDS_BIN, !io::in(IN_GROUNDS_BIN));
    setIf(F_DOOR_OPEN, io::in(IN_DOOR));
    setIf(F_CABINET_HOT, !isnan(tCab) && tCab > CABINET_MAX_C);
    if (io::in(IN_LEAK)) faultBits |= F_LEAK;
    if ((!isnan(tT) && tT > THERMO_MAX_C) || (!isnan(tB) && tB > BOILER_MAX_C)) faultBits |= F_OVERTEMP;

    bool critical = (faultBits & CRITICAL) || moving;

    // ---- water: master valve open unless a leak was detected
    io::set(OUT_MASTER_VALVE, !(faultBits & F_LEAK));

    // ---- boiler fill
    bool covered = io::in(IN_BOILER_LEVEL);
    if (!filling && !covered && !moving && !(faultBits & (F_LEAK | F_FILL_TIMEOUT | F_NO_WATER))) { filling = true; fillStart = now; fillCoveredAt = 0; }
    if (filling) {
      if (covered && fillCoveredAt == 0) fillCoveredAt = now;
      bool done = fillCoveredAt && now - fillCoveredAt > 1500;
      bool timeout = now - fillStart > BOILER_FILL_TIMEOUT_MS;
      if (timeout) faultBits |= F_FILL_TIMEOUT;
      if (done || timeout || (faultBits & (F_LEAK | F_NO_WATER))) filling = false;   // never pump from empty bottles
    }
    io::set(OUT_BOILER_FILL, filling);
    if (bottled && filling) io::set(OUT_BOTTLE_PUMP, true);
    else if (bottled && !pumpIsOn) io::set(OUT_BOTTLE_PUMP, false);

    // ---- prime the thermoblock once after boot (pump water through to the drip tray)
    if (prime == PRIME_NEEDED && !critical && !(faultBits & F_NO_WATER) && io::in(IN_GROUP_HOME) && pumpAllowed()) {
      prime = PRIME_RUNNING; primeStart = now; primePulses0 = io::flowPulses();
      io::set(OUT_BREW_VALVE, true);      // open path through the empty group to the drip tray
      pumpOn(true);
    }
    if (prime == PRIME_RUNNING) {
      if (now - primeStart > 8000) {
        pumpOn(false);
        io::set(OUT_BREW_VALVE, false);
        bool flowed = io::flowPulses() - primePulses0 > 40;
        if (flowed) prime = PRIME_DONE; else { prime = PRIME_NEEDED; faultBits |= F_PRIME_FAILED; }
      }
    }

    // ---- heater contactor and SSRs
    //  Thermoblock: 1 s time-proportioning through the MCP23017 (no mechanical wear).
    //  Boiler: 10 s windows through relay OUT 3 (at most 2 clicks per window).
    //  120 V power budget: the two heaters never overlap, both pause while the
    //  grinder runs, and their combined share is capped while the chiller runs.
    bool contactor = !critical;
    io::set(OUT_HEATER_CONTACTOR, contactor);
    float thermoTarget = store::recipes["temps"]["thermoblockC"] | 100;
    float boilerTarget = store::recipes["temps"]["boilerC"] | 95;
    float dT = (contactor && prime == PRIME_DONE) ? pi(hThermo, thermoTarget, tT) : 0;
    float dB = (contactor && covered && !brewActive && !filling) ? pi(hBoiler, boilerTarget, tB) : 0;
    float cap = chillerOn ? CHILLER_HEATER_CAP : 1.0f;
    if (dT > cap) dT = cap;
    if (now - boilerWindowStart >= BOILER_WINDOW_MS) {
      boilerWindowStart = now;
      float b = min(dB, max(0.0f, cap - dT));
      boilerOnMs = (uint32_t)(b * BOILER_WINDOW_MS);
      if (boilerOnMs < 300) boilerOnMs = 0;                                         // skip tiny pulses
      if (boilerOnMs > BOILER_WINDOW_MS - 300) boilerOnMs = BOILER_WINDOW_MS;       // and tiny gaps
    }
    bool grinding = io::get(OUT_GRINDER);
    bool boilerOn = boilerOnMs && dB > 0 && !grinding && (now - boilerWindowStart) < boilerOnMs;
    if (now - thermoWindowStart >= THERMO_WINDOW_MS) thermoWindowStart = now;
    float share = 1.0f - (float)boilerOnMs / BOILER_WINDOW_MS;                     // thermoblock runs in the boiler's off time
    float dTeff = share > 0.05f ? min(1.0f, dT / share) : 0.0f;
    bool thermoOn = !boilerOn && !grinding && (now - thermoWindowStart) < (uint32_t)(dTeff * THERMO_WINDOW_MS);
    hThermo.duty = dT; hBoiler.duty = dB;
    io::thermoSSR(thermoOn);
    io::set(OUT_BOILER_SSR, boilerOn);

    // ---- chiller with compressor restart delay; pauses while boiler heats from cold
    float coldTarget = store::recipes["temps"]["coldC"] | 6;
    bool wantChill = hasChiller && !isnan(tC) && !(faultBits & F_LEAK) && !moving;
    if (wantChill) {
      if (!chillerOn && tC > coldTarget + 1.5f && now - chillerOffAt > CHILLER_MIN_OFF_MS && !(hBoiler.duty > 0.9f)) chillerOn = true;
      if (chillerOn && tC < coldTarget - 1.0f) { chillerOn = false; chillerOffAt = now; }
    } else if (chillerOn) { chillerOn = false; chillerOffAt = now; }
    io::set(OUT_CHILLER, chillerOn);

    // ---- pump duty-cycle guard (hard stop)
    if (pumpIsOn && now - pumpOnAt > PUMP_MAX_RUN_MS) pumpOn(false);

    // ---- fan, sign, tablet charging (40% on / 80% off; on if tablet silent > 10 min)
    io::set(OUT_FAN, isnan(tCab) || tCab > 32.0f || chillerOn);
    io::set(OUT_LED_SIGN, true);
    bool charge = io::get(OUT_TABLET_CHARGE);
    if (tabletPct < 0 || now - tabletAt > 600000) charge = true;
    else if (tabletPct <= 40) charge = true;
    else if (tabletPct >= 80) charge = false;
    io::set(OUT_TABLET_CHARGE, charge);
  }
}
