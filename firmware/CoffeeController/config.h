#pragma once
#include <Arduino.h>

// ============================================================================
//  HARDWARE CONFIGURATION - KinCony KC868-A32 (ESP32)
//  v1.1.0: pins and I2C addresses now follow KinCony's published pin list for
//  the KC868-A32 (kincony.com/forum/showthread.php?tid=1771). Still check them
//  against your board revision, and test on the bench with nothing connected
//  to the relays before connecting any machinery.
// ============================================================================
#define FW_VERSION "1.2.0"

// ---- I2C bus A (GPIO15 SDA / GPIO13 SCL): the 4 PCF8574 chips that drive the 32 relays
static const int PIN_I2C_A_SDA = 15;
static const int PIN_I2C_A_SCL = 13;
static const uint8_t OUT_EXPANDER_ADDR[4] = {0x24, 0x25, 0x21, 0x22};   // relays 1-8, 9-16, 17-24, 25-32

// ---- I2C bus B (GPIO4 SDA / GPIO5 SCL): the 4 PCF8574 chips that read the 32 opto inputs
//      (v1.0.0 read these on bus A at the wrong addresses, so no sensor worked)
static const int PIN_I2C_B_SDA = 4;
static const int PIN_I2C_B_SCL = 5;
static const uint8_t IN_EXPANDER_ADDR[4]  = {0x24, 0x25, 0x21, 0x22};   // inputs 1-8, 9-16, 17-24, 25-32

static const bool RELAY_ACTIVE_LOW = true;   // PCF8574 pulls low to energise relay
static const bool INPUT_ACTIVE_LOW = true;   // opto input reads low when closed

// ---- Add-on boards on the A32's I2C connector (new in v1.1.0)
//      Wire them to the same bus as the relays (bus A). If your A32's I2C connector
//      turns out to be bus B, set EXT_ON_BUS_B to true.
static const bool    EXT_ON_BUS_B   = false;
static const uint8_t MCP23017_ADDR  = 0x20;   // 16 extra outputs (A0, A1, A2 tied to GND)
static const uint8_t ADS1115_ADDR   = 0x48;   // 4-channel temperature reader (ADDR tied to GND)

// MCP23017 port A pins
static const uint8_t EXP_GROUP_FWD   = 0;    // BTS7960 #1 RPWM  (brew group close)
static const uint8_t EXP_GROUP_REV   = 1;    // BTS7960 #1 LPWM  (brew group open)
static const uint8_t EXP_NOZZLE_FWD  = 2;    // BTS7960 #2 RPWM  (nozzle out)
static const uint8_t EXP_NOZZLE_REV  = 3;    // BTS7960 #2 LPWM  (nozzle back)
static const uint8_t EXP_THERMO_SSR  = 4;    // ULN2003 input 1 -> thermoblock SSR (no mechanical relay)
// Pins 0-3 go through a 3.3 V to 5 V level shifter to the BTS7960 boards.
// Tie R_EN and L_EN on both BTS7960 boards to their 5 V pin (always enabled).

// ---- Temperature probes: 10k NTC B3950, read by the ADS1115 (channel 0-3).
//      Each probe has a reference resistor from 3.3 V to the probe; the probe goes to GND.
//      Hot probes use 1k (best resolution near 95 C); cold and cabinet probes use 10k.
static const float NTC_BETA = 3950.0f, NTC_R25 = 10000.0f;
static const float NTC_VREF = 3.30f;                                   // measure your 3.3 V rail and enter it here
static const float NTC_PULLUP[4]   = {1000.0f, 1000.0f, 10000.0f, 10000.0f};
static const float NTC_OFFSET_C[4] = {0.0f, 0.0f, 0.0f, 0.0f};          // set after the ice-water / boiling-water check

// ---- Free ESP32 pins: the A32's white 4-pin "serial extender" socket has GPIO32 and GPIO33
static const int PIN_FLOWMETER = 33;          // Gicar flow meter pulse (add a 4.7k pull-up to 3.3 V)
static const int PIN_HEARTBEAT = 32;          // ULN2003 input 2 -> missing-pulse watchdog relay trigger
static const float FLOW_PULSES_PER_ML = 1.95f; // calibrate on site
static const uint32_t HEARTBEAT_HALF_MS = 100; // 5 Hz square wave while the controller is healthy


// ============================================================================
//  OUTPUT MAP (matches Section 3 of the build spec, v4)
// ============================================================================
enum Out : uint8_t {
  OUT_HEATER_CONTACTOR = 1, OUT_SPARE_2 = 2, OUT_BOILER_SSR = 3, OUT_PUMP = 4,
  OUT_MASTER_VALVE = 5, OUT_BOILER_FILL = 6, OUT_BREW_VALVE = 7,
  OUT_MIXER1_VALVE = 8, OUT_MIXER2_VALVE = 9, OUT_MIXER3_VALVE = 10,
  OUT_SPOUT_VALVE = 11, OUT_COLD_VALVE = 12, OUT_GRINDER = 13,
  OUT_AUGER1 = 14, OUT_AUGER2 = 15, OUT_AUGER3 = 16, OUT_AUGER4 = 17,
  OUT_WHIP1 = 18, OUT_WHIP2 = 19, OUT_WHIP3 = 20,
  OUT_SYRUP1 = 21, OUT_SYRUP2 = 22,
  OUT_CUP_TURRET = 23, OUT_CUP_DROP = 24, OUT_LID_DROP = 25,
  OUT_DOOR_LOCK = 26, OUT_PICKUP_LIGHT = 27, OUT_BOTTLE_PUMP = 28,
  OUT_CHILLER = 29, OUT_TABLET_CHARGE = 30, OUT_FAN = 31, OUT_LED_SIGN = 32
};

enum In : uint8_t {
  IN_DOOR = 1, IN_GROUP_HOME = 2, IN_GROUP_PRESSED = 3, IN_GROUNDS_BIN = 4,
  IN_WASTE_FULL = 5, IN_BOILER_LEVEL = 6, IN_CUP_PRESENT = 7, IN_BEANS_LOW = 8,
  /* IN 9 spare (the flow meter is on GPIO33) */
  IN_INLET_PRESSURE = 10, IN_LEAK = 11, IN_SERVICE_BTN = 12,
  IN_CUP_DROPPED = 13, IN_LID_DROPPED = 14, IN_TURRET_HOME = 15,
  IN_NOZZLE_FRONT = 16, IN_NOZZLE_BACK = 17,
  IN_BOTTLE1_OK = 18, IN_BOTTLE2_OK = 19     // stick-on level sensors: active = water present
};

enum Ntc : uint8_t { NTC_THERMOBLOCK = 0, NTC_BOILER = 1, NTC_COLD = 2, NTC_CABINET = 3 };

// ============================================================================
//  POWER: plugs into a normal 120 V 15 A outlet (NEMA 5-15, GFCI in the plug).
//  Thermoblock 1,000 W and boiler 1,000 W are never on together; both pause
//  while the grinder runs; total heater time is capped while the chiller runs.
// ============================================================================
static const uint32_t THERMO_WINDOW_MS = 1000;    // thermoblock SSR: switched by the expander, no wear
static const uint32_t BOILER_WINDOW_MS = 10000;   // boiler SSR: relay clicks at most twice per 10 s
static const float    CHILLER_HEATER_CAP = 0.70f; // max combined heater duty while the compressor runs

// ============================================================================
//  WATER BOTTLES
// ============================================================================
static const float    BOTTLE_PAIR_ML = 22700.0f;  // two full 3-gallon bottles
static const uint32_t BOTTLE_EMPTY_CONFIRM_MS = 3000;  // sensor must read empty this long (sloshing)

// ============================================================================
//  SAFETY LIMITS (software; hardware cut-outs are the final backstop)
// ============================================================================
static const float THERMO_MAX_C = 110.0f;
static const float BOILER_MAX_C = 99.5f;
static const float CABINET_MAX_C = 50.0f;
static const uint32_t PUMP_MAX_RUN_MS  = 60000;    // Ulka EAX5: 1 min on
static const uint32_t PUMP_MIN_REST_MS = 120000;   //            2 min off
static const uint32_t BOILER_FILL_TIMEOUT_MS = 180000;
static const uint32_t MOTOR_TIMEOUT_MS = 4000;     // brew group / nozzle / cup drop
static const uint32_t CHILLER_MIN_OFF_MS = 180000; // compressor restart delay
