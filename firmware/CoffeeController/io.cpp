#include "io.h"
#include <Wire.h>

// Bus A = Wire  (relays, and the add-on boards unless EXT_ON_BUS_B)
// Bus B = Wire1 (opto inputs)
namespace {
  TwoWire& busA = Wire;
  TwoWire& busB = Wire1;
  TwoWire& busExt() { return EXT_ON_BUS_B ? Wire1 : Wire; }

  uint32_t outState = 0, outWritten = 0xFFFFFFFF, lastOutRefresh = 0;
  uint32_t inRaw = 0, inStable = 0, inLastChange[32];
  float temps[4] = {NAN, NAN, NAN, NAN};
  volatile uint32_t pulses = 0;

  // health
  uint32_t relayOkAt = 0, inputOkAt = 0, extOkAt = 0;

  // MCP23017
  uint8_t expState = 0, expWritten = 0xFF; uint32_t lastExpRefresh = 0; bool expInit = false;
  // ADS1115 round-robin
  uint8_t adsCh = 0; bool adsBusy = false; uint32_t adsStartedAt = 0;
  // heartbeat
  bool hb = false; uint32_t hbAt = 0;

  void IRAM_ATTR onFlowPulse() { pulses++; }

  bool writeByte(TwoWire& w, uint8_t addr, uint8_t value) {
    w.beginTransmission(addr);
    w.write(value);
    return w.endTransmission() == 0;
  }
  int readByte(TwoWire& w, uint8_t addr) {
    if (w.requestFrom((int)addr, 1) != 1) return -1;
    return w.read();
  }
  bool writeReg8(TwoWire& w, uint8_t addr, uint8_t reg, uint8_t v) {
    w.beginTransmission(addr); w.write(reg); w.write(v);
    return w.endTransmission() == 0;
  }
  bool writeReg16(TwoWire& w, uint8_t addr, uint8_t reg, uint16_t v) {
    w.beginTransmission(addr); w.write(reg); w.write((uint8_t)(v >> 8)); w.write((uint8_t)(v & 0xFF));
    return w.endTransmission() == 0;
  }
  bool readReg16(TwoWire& w, uint8_t addr, uint8_t reg, int16_t& out) {
    w.beginTransmission(addr); w.write(reg);
    if (w.endTransmission(false) != 0) return false;
    if (w.requestFrom((int)addr, 2) != 2) return false;
    out = (int16_t)((w.read() << 8) | w.read());
    return true;
  }

  // ---- MCP23017 (IOCON.BANK = 0): IODIRA 0x00, OLATA 0x14
  bool expBegin() {
    TwoWire& w = busExt();
    bool ok = writeReg8(w, MCP23017_ADDR, 0x14, 0x00) && writeReg8(w, MCP23017_ADDR, 0x00, 0x00);  // outputs low, then all port A as outputs
    expWritten = 0xFF; expInit = ok;
    return ok;
  }
  void expFlush(uint32_t now) {
    if (!expInit && !expBegin()) return;
    if (expState == expWritten && now - lastExpRefresh < 500) return;   // re-send twice a second in case the chip reset
    if (writeReg8(busExt(), MCP23017_ADDR, 0x14, expState)) { expWritten = expState; extOkAt = now; }
    else expInit = false;
    lastExpRefresh = now;
  }
  void expBit(uint8_t bit, bool on) { if (on) expState |= (1 << bit); else expState &= ~(1 << bit); }

  // ---- ADS1115: single-shot, +/-4.096 V range, 128 samples/s, comparator off
  uint16_t adsConfig(uint8_t ch) { return 0x8000 | ((uint16_t)(4 + ch) << 12) | 0x0200 | 0x0100 | 0x0080 | 0x0003; }
  float ntcFromVolts(uint8_t ch, float v) {
    if (v < 0.02f || v > NTC_VREF - 0.02f) return NAN;                   // shorted or open probe
    float r = NTC_PULLUP[ch] * v / (NTC_VREF - v);
    float t = 1.0f / (1.0f / 298.15f + log(r / NTC_R25) / NTC_BETA) - 273.15f;
    return t + NTC_OFFSET_C[ch];
  }
  void adsStep(uint32_t now) {
    TwoWire& w = busExt();
    if (!adsBusy) {
      if (writeReg16(w, ADS1115_ADDR, 0x01, adsConfig(adsCh))) { adsBusy = true; adsStartedAt = now; }
      else { temps[adsCh] = NAN; adsCh = (adsCh + 1) % 4; }
      return;
    }
    if (now - adsStartedAt < 12) return;                                  // one conversion takes about 8 ms
    int16_t raw;
    float t = NAN;
    if (readReg16(w, ADS1115_ADDR, 0x00, raw)) { extOkAt = now; t = ntcFromVolts(adsCh, raw * (4.096f / 32768.0f)); }
    temps[adsCh] = isnan(t) ? NAN : (isnan(temps[adsCh]) ? t : temps[adsCh] * 0.7f + t * 0.3f);
    adsBusy = false; adsCh = (adsCh + 1) % 4;
  }
}

namespace io {
  void begin() {
    busA.begin(PIN_I2C_A_SDA, PIN_I2C_A_SCL, 100000);
    busB.begin(PIN_I2C_B_SDA, PIN_I2C_B_SCL, 100000);
    for (int i = 0; i < 4; i++) writeByte(busA, OUT_EXPANDER_ADDR[i], RELAY_ACTIVE_LOW ? 0xFF : 0x00);   // all relays off
    for (int i = 0; i < 4; i++) writeByte(busB, IN_EXPANDER_ADDR[i], 0xFF);                               // inputs: release high
    outWritten = 0xFFFFFFFF;
    for (int i = 0; i < 32; i++) inLastChange[i] = 0;
    expState = 0; expBegin();
    pinMode(PIN_FLOWMETER, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_FLOWMETER), onFlowPulse, FALLING);
    pinMode(PIN_HEARTBEAT, OUTPUT); digitalWrite(PIN_HEARTBEAT, LOW);
  }

  void update() {
    uint32_t now = millis();
    // relays (bus A)
    if (outState != outWritten || now - lastOutRefresh > 1000) {
      bool ok = true;
      for (int e = 0; e < 4; e++) {
        uint8_t bits = (outState >> (e * 8)) & 0xFF;
        ok &= writeByte(busA, OUT_EXPANDER_ADDR[e], RELAY_ACTIVE_LOW ? (uint8_t)~bits : bits);
      }
      if (ok) { outWritten = outState; relayOkAt = now; }
      lastOutRefresh = now;
    }
    // inputs (bus B)
    bool allIn = true;
    for (int e = 0; e < 4; e++) {
      int v = readByte(busB, IN_EXPANDER_ADDR[e]);
      if (v < 0) { allIn = false; continue; }
      uint8_t bits = INPUT_ACTIVE_LOW ? (uint8_t)~v : (uint8_t)v;
      for (int b = 0; b < 8; b++) {
        int idx = e * 8 + b;
        bool raw = bits & (1 << b);
        bool prevRaw = inRaw & (1UL << idx);
        if (raw != prevRaw) { inLastChange[idx] = now; if (raw) inRaw |= (1UL << idx); else inRaw &= ~(1UL << idx); }
        if (now - inLastChange[idx] > 30) {                 // 30 ms debounce
          if (raw) inStable |= (1UL << idx); else inStable &= ~(1UL << idx);
        }
      }
    }
    if (allIn) inputOkAt = now;
    // add-on boards
    expFlush(now);
    adsStep(now);
    // heartbeat to the missing-pulse relay: stops if the loop hangs or the relay bus
    // stops answering, so the heater contactor drops even if a relay is stuck on
    if (relaysOk() && now - hbAt >= HEARTBEAT_HALF_MS) { hb = !hb; digitalWrite(PIN_HEARTBEAT, hb); hbAt = now; }
  }

  void set(uint8_t out, bool on) {
    if (out < 1 || out > 32) return;
    if (on) outState |= (1UL << (out - 1)); else outState &= ~(1UL << (out - 1));
  }
  bool get(uint8_t out) { return out >= 1 && out <= 32 && (outState & (1UL << (out - 1))); }
  bool in(uint8_t input) { return input >= 1 && input <= 32 && (inStable & (1UL << (input - 1))); }
  float tempC(uint8_t ntc) { return ntc < 4 ? temps[ntc] : NAN; }

  void allMotorsAndValvesOff() {
    const uint8_t keep[] = {OUT_HEATER_CONTACTOR, OUT_BOILER_SSR, OUT_CHILLER,
                            OUT_TABLET_CHARGE, OUT_FAN, OUT_LED_SIGN, OUT_MASTER_VALVE, OUT_BOILER_FILL};
    uint32_t mask = 0;
    for (uint8_t k : keep) mask |= (1UL << (k - 1));
    outState &= mask;
    groupMotor(0); nozzleMotor(0);
  }

  // BTS7960: RPWM high = forward, LPWM high = reverse, both low = stop (brake)
  void groupMotor(int dir)  { expBit(EXP_GROUP_FWD, dir > 0);  expBit(EXP_GROUP_REV, dir < 0); }
  void nozzleMotor(int dir) { expBit(EXP_NOZZLE_FWD, dir > 0); expBit(EXP_NOZZLE_REV, dir < 0); }
  void thermoSSR(bool on)   { expBit(EXP_THERMO_SSR, on); }
  bool thermoSSRState()     { return expState & (1 << EXP_THERMO_SSR); }

  uint32_t flowPulses() { noInterrupts(); uint32_t p = pulses; interrupts(); return p; }
  void resetFlow() { noInterrupts(); pulses = 0; interrupts(); }

  bool relaysOk() { return millis() - relayOkAt < 1500; }
  bool inputsOk() { return millis() - inputOkAt < 1500; }
  bool extOk()    { return millis() - extOkAt < 1500; }
}
