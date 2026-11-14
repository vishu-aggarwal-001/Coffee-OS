#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// "Plant" = the always-running parts of the machine: water, heaters, chiller,
// safety interlocks, fans and tablet charging. Runs every loop, order or not.
namespace plant {
  enum Fault : uint32_t {
    F_LEAK          = 1u << 0,   // critical: water on the floor
    F_OVERTEMP      = 1u << 1,   // critical
    F_PROBE_THERMO  = 1u << 2,   // critical
    F_PROBE_BOILER  = 1u << 3,   // critical
    F_FILL_TIMEOUT  = 1u << 4,   // critical: boiler would not fill
    F_PRIME_FAILED  = 1u << 5,   // critical: no flow through thermoblock
    F_NO_WATER      = 1u << 6,   // blocks sales
    F_WASTE_FULL    = 1u << 7,   // blocks sales
    F_NO_GROUNDS_BIN= 1u << 8,   // blocks sales
    F_DOOR_OPEN     = 1u << 9,   // blocks sales
    F_BREW_GROUP    = 1u << 10,  // critical for coffee drinks
    F_CUP_JAM       = 1u << 11,  // blocks sales until cleared
    F_PROBE_COLD    = 1u << 12,  // disables chilled drinks only
    F_CABINET_HOT   = 1u << 13,  // warning
    F_MOVING        = 1u << 14,  // owner set "get ready to move": heaters, chiller and sales off
    F_IO_BUS        = 1u << 15,  // critical: controller can't reach its relay, input or add-on chips
  };
  const uint32_t CRITICAL = F_LEAK | F_OVERTEMP | F_PROBE_THERMO | F_PROBE_BOILER | F_FILL_TIMEOUT | F_PRIME_FAILED | F_IO_BUS;
  const uint32_t LATCHED  = F_FILL_TIMEOUT | F_PRIME_FAILED | F_BREW_GROUP | F_CUP_JAM | F_OVERTEMP | F_LEAK;

  void begin();
  void update();
  uint32_t faults();
  void raise(uint32_t f);
  void clearLatched();
  String faultText(uint32_t f);

  bool hotReady();            // boiler full and at temperature
  bool brewReady();           // thermoblock primed and at temperature
  bool coldReady();           // chiller at temperature
  bool salesBlocked(String& why);

  void setBrewActive(bool on);          // load management: boiler pauses during espresso
  bool pumpAllowed();                   // Ulka duty cycle
  void pumpOn(bool on);                 // all pump use must go through here
  void tabletBattery(int pct, bool charging);
  void requestPrime();
  bool primed();

  // Water bottles (v1.1.0)
  void bottlesSwapped();               // staff put in 2 full bottles: reset the water count, re-prime
  bool waterEstimateUsed();            // true when sales stop on the estimated count (no bottle sensors)
  void bottleStatus(JsonObject out);   // for the counter screen and owner app
}
