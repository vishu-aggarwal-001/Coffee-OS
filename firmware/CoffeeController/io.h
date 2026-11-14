#pragma once
#include "config.h"

namespace io {
  void begin();
  void update();                       // call every loop: flush relays, read inputs and probes, heartbeat
  void set(uint8_t out, bool on);      // OUT 1..32 (relays)
  bool get(uint8_t out);
  bool in(uint8_t input);              // IN 1..32, debounced, true = active
  float tempC(uint8_t ntc);            // NAN when probe open or shorted, or the ADS1115 is not answering
  void allMotorsAndValvesOff();        // everything except heaters/contactor/chiller/lights
  // H-bridge: dir +1 forward, -1 reverse, 0 stop (driven through the MCP23017)
  void groupMotor(int dir);
  void nozzleMotor(int dir);
  // Espresso thermoblock SSR (driven through the MCP23017, no mechanical relay)
  void thermoSSR(bool on);
  bool thermoSSRState();
  // Flow meter
  uint32_t flowPulses();
  void resetFlow();
  // Health of the I2C buses (relays, inputs, add-on boards)
  bool relaysOk();
  bool inputsOk();
  bool extOk();
}
