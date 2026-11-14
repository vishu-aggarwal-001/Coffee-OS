# Coffee machine software (v8, firmware 1.2.0, CoffeeOS 2.0)

New in v8: CoffeeOS 2.0 adds a control page for your phone (`/control`), a PIN-protected service menu on the touchscreen (hold the top-left corner for 3 seconds), opening hours, Refilled buttons, low-stock and end-of-day phone alerts, and a `coffee` command for the terminal. Firmware 1.2.0 adds Close/Open (`paused`) and the printed cup droppers (`cupMode`). Everything is in `coffee-os/README.md`, and "What changed in 1.2.0" is at the end of this file.


Firmware 1.1.0 fixes the problems found in the v3 review: sensor inputs on the wrong I2C bus, pins used twice, motor driver wiring, temperature reading, heater relay wear, no hardware watchdog, and no way for staff to report a bottle swap. See "What changed in 1.1.0" at the end, and `../CHANGES_v4.txt`.

Three programs work together:

| Program | Runs on | What it does |
| --- | --- | --- |
| **Controller firmware** (`firmware/`) | KinCony KC868-A32 board inside the machine | Makes every drink, runs heaters, water, chiller and safety shutoffs, checks counter codes, and serves the screens below over Wi-Fi |
| **Touchscreen menu** (`firmware/CoffeeController/data/index.html`) | The RCA tablet, inside Fully Kiosk | What customers see: menu, sizes, sugar, code entry, a cup that fills while the drink is made |
| **Counter screen** (`firmware/CoffeeController/data/counter.html`) | The station's register computer or a phone | Cashier takes payment and makes a 4-digit code for the drink |
| **Owner app** (`firmware/CoffeeController/data/owner.html`) | Your phone's browser, added to the home screen | Sales, prices, recipes, stock, cleaning, calibration and settings |
| **CoffeeOS + hub** (`coffee-os/`, v6, recommended) | Raspberry Pi 5 + Touch Display 2 in the door | Boots straight to the touchscreen. The hub serves all screens, copies sales to a database, sends phone alerts, allows access from home (Tailscale), shows all machines, updates the controller's firmware over USB |
| **Kiosk app** (`android-kiosk/`, optional) | The RCA tablet, if you keep it | Same job as Fully Kiosk. v5 said this folder was missing; it is in the package (source only, build it with Android Studio; not tested). Or use Fully Kiosk Browser ($10.99) |

## How customers pay

The machine takes no money. The customer pays at the station counter. The cashier opens the **counter screen** (`http://192.168.4.1/counter`, or the machine's store-Wi-Fi address), taps the drink and size, and reads out or writes down the 4-digit code. The customer types the code on the machine, chooses sugar, and the drink is made. A code works once; if a drink fails, the code keeps working. Unused codes expire after 12 hours (changeable). Sales made with codes show in the owner app with their price.

**Free mode** (owner app, Settings) skips the code so any drink starts with a tap. It is **on when the machine is new** for testing. Turn it off before opening.

The tablet and your phone never control the machine directly. They send requests; the controller decides whether it is safe.

---

## 1. Try it on a laptop first (5 minutes)

```
cd simulator
python3 simulator.py
```
Open http://localhost:8080/ for the touchscreen, http://localhost:8080/counter for the cashier screen (PIN `1234`) and http://localhost:8080/owner for the owner app (PIN `000000`). Make a code on the counter screen, then type it on the touchscreen. Size your browser to a tall, narrow window to see the tablet layout.

## 2. Flash the controller

**Easiest: from a browser.** Plug the KC868-A32 into a computer with USB. Open https://espressif.github.io/esptool-js/ in Chrome, click *Connect*, pick the board, add `firmware/prebuilt/coffee-controller-full.bin` at address `0x0`, and click *Program*. This writes the firmware and the screens in one go.

> **v6 warning:** the full image also overwrites the saved PINs, Wi-Fi settings, recipes, stock and sales log. Use it only on a **new** board. To **update** a working machine, load `firmware/prebuilt/CoffeeController.ino.bin` at **`0x10000`** instead. With the screen computer, run `sudo bash /opt/coffee-os/tools/flash-controller.sh` on the Pi, which does exactly that and refuses while a drink is being made.

**From the command line:**
```
pip install esptool
esptool.py --chip esp32 write_flash 0x0 firmware/prebuilt/coffee-controller-full.bin
```

**To change the code:** open `firmware/CoffeeController/CoffeeController.ino` in Arduino IDE 2 with the *esp32 by Espressif* board package 2.0.x and the *ArduinoJson* 7 library. Board: *ESP32 Dev Module*, partition scheme *Default 4MB with spiffs*. Upload the `data` folder with the *LittleFS upload* plugin, or rebuild `littlefs.bin` with `mklittlefs` (offset `0x290000`, size `0x160000`).

> **Before connecting any machinery:** firmware 1.1.0 follows KinCony's published KC868-A32 pin list (relays on GPIO15/13 at 0x24, 0x25, 0x21, 0x22; inputs on GPIO4/5 at the same addresses; free pins GPIO32 and GPIO33). Still check `config.h` against your board revision. Run the board on the bench first with nothing wired to the relays, and confirm in the owner app (Today tab) that there is no "Controller can't reach its relay, sensor or add-on boards" fault.

## 2b. Add-on boards (new in 1.1.0)

Three small boards and two sensors are added. All parts are in the v4 shopping list.

| Board | Connects to | What it does |
| --- | --- | --- |
| MCP23017 expander, address 0x20 (A0, A1, A2 to GND) | A32 I2C connector (bus A: GPIO15 SDA, GPIO13 SCL), 3.3 V, GND | Port A pin 0/1: brew-group BTS7960 RPWM/LPWM. Pin 2/3: nozzle BTS7960 RPWM/LPWM. Pin 4: thermoblock SSR (through the ULN2003). |
| 4-channel level shifter (BSS138 type), 3.3 V to 5 V | Between MCP23017 pins 0-3 and the BTS7960 inputs | The BTS7960 boards need 5 V logic. Tie R_EN and L_EN on both BTS7960 boards to their 5 V pin. |
| ULN2003 driver board | Input 1 from MCP23017 pin 4; input 2 from GPIO32 | Output 1 switches the thermoblock SSR input (SSR + to 12 V, SSR - to ULN2003 out 1). Output 2 pulses the watchdog relay's trigger input. |
| ADS1115 reader, address 0x48 (ADDR to GND) | Same I2C connector, 3.3 V, GND | A0 thermoblock, A1 boiler, A2 chilled water, A3 cabinet. Each probe: reference resistor from 3.3 V to the input (1k for A0 and A1, 10k for A2 and A3), probe from the input to GND. |
| Missing-pulse (watchdog) timer relay, 24 V DC, set to about 1 s | Trigger from GPIO32 (white 4-pin socket) through ULN2003 output 2; contacts in series with the heater contactor coil after OUT 1 | Cuts heater power within about 1 to 2 s if the controller freezes or loses its relay bus. |
| Gicar flow meter | GPIO33 (white 4-pin socket), 4.7k pull-up to 3.3 V | Counts water through the thermoblock. |
| 2 stick-on level sensors (XKC-Y25, NPN) | IN 18 and IN 19, near the bottom of each bottle | Tell the machine when a bottle is really empty. Turn on "Bottle sensors fitted" in Settings. |

If the A32's I2C connector turns out to be bus B on your revision, set `EXT_ON_BUS_B = true` in `config.h`.

**Check the temperatures before heating:** put each probe in ice water (should read 0 °C) and boiling water (about 99.4 °C in West Lafayette). Enter any difference in `NTC_OFFSET_C` in `config.h` and measure your 3.3 V rail for `NTC_VREF`.

## 3. First start-up

The machine is self-contained: plug it into a normal 120 V outlet (the GFCI is built into the plug) and put 2 full 3-gallon water bottles in the base with their draw tubes. No electrician or plumber is needed at the station.

1. Power the controller. It creates a Wi-Fi network called **CoffeeMachine-xxxx**, password **change-me-123**.
2. Join it with your phone and open **http://192.168.4.1/owner**. The PIN is **000000**.
3. In **Settings**: change the PIN, change the machine Wi-Fi password, and add the store's Wi-Fi if you want to manage the machine from anywhere in the store.
4. In **Stock**: enter what you loaded (beans, powders, syrups, cups, lids). For water, press **Bottles swapped now**. After that, **cashiers press "Bottles swapped" on the counter screen** each time they put in 2 full bottles; nobody needs the owner PIN. With the bottle sensors fitted and turned on, the machine also notices full bottles by itself and stops selling when a bottle is really empty.
5. In **Machine → Calibrate**: run each canister, the grinder, the syrups and the water outlets for 3 seconds, weigh what came out, and save. Do this again whenever you change an ingredient brand.
6. **Free mode is on** when the machine is new: drinks start without a code. Make test drinks and run the 100-drink burn-in.
7. In **Settings → Counter**: set the counter PIN (default `1234`) and give it to the station's cashiers with the counter screen link. Then turn **free mode off**.

## 4. Set up the screen

**v6: Raspberry Pi screen computer (recommended).** Follow the picture manual, screen steps A to F, or `coffee-os/README.md`. In short: write Raspberry Pi OS Lite (64-bit) to the SD card, copy `coffee-os` onto its bootfs drive, run `sudo bash /boot/firmware/coffee-os/install.sh` on the Pi, connect the display and power. It starts into the menu by itself from then on.

**Or the RCA tablet (v5 way, no building needed):**

1. On the RCA tablet, install **Fully Kiosk Browser** from the Play Store and buy the PLUS license in the app (about $8, one time).
2. Join the **CoffeeMachine-xxxx** Wi-Fi. When Android says it has no internet, choose to stay connected.
3. In Fully Kiosk settings:
   - *Start URL*: `http://192.168.4.1/`
   - *Kiosk Mode*: on, with a kiosk PIN you'll remember
   - *Device Management → Launch on Boot*: on; *Keep Screen On*: on
   - *Advanced Web Settings → Enable JavaScript Interface*: on (lets the menu report battery level so the machine controls charging)
   - *Web Zoom and Scaling → Enable Zoom*: off
4. Mount the tablet in the door bezel and plug in the USB-C cable from the machine's 5 V supply.

To leave kiosk mode for service, swipe from the left edge and enter the kiosk PIN.

**Optional, instead of Fully Kiosk:** a free kiosk app was mentioned in v3 and v4 but its `android-kiosk` folder was never in the package, so it is not available here. Use Fully Kiosk Browser.

## Moving the machine to another station

1. Owner app → **Machine → Get ready to move**. Heaters, chiller and sales switch off. Wait about 20 minutes for the boiler to cool.
2. Empty the wastewater bucket and grounds bin, take out the water bottles, unplug, and release the caster locks.
3. Roll it with 2 people using the side hand-holds; use a truck with a lift gate or ramp between stations. Keep it upright.
4. At the new station: lock the casters, plug in, load full bottles, update **Stock**, then press **Back in service**. It reheats and primes itself.

## 5. Put the owner app on your phone

Join the machine's Wi-Fi (or the store Wi-Fi, if you set it up) and open `http://192.168.4.1/owner`, or the store-network address shown in *Settings*. Use *Add to Home Screen* in your browser. It then opens like an app.

The owner app works when your phone is on the machine's Wi-Fi or the store's Wi-Fi. **v6:** with the screen computer you can also check machines from home with Tailscale (free, no server to rent); see the picture manual, step F.

## 6. Changing drinks

Everything about the menu lives in `recipes.json` and is edited from the owner app's **Drinks** tab: name, description, hot or chilled, prices per size, sugar on or off, and the recipe steps. Recipes are written for a 12 oz cup; 10 oz uses 80% of each amount and 16 oz uses 130% (espresso stays a single shot except in 16 oz, which gets 1.6×).

| Step | Settings | What happens |
| --- | --- | --- |
| Espresso shot | beans (g), shot (ml) | Grinds, brews through the brew group, ejects the puck |
| Mix powder with water | mixer 1 to 3, hot or chilled, water (ml), grams from each canister, whisk seconds | Doses powders into a mixing bowl with water and whisks |
| Plain water | hot or chilled, ml | Pours from the hot spout or chilled line |
| Flavor syrup | syrup 1 or 2, ml | Runs the peristaltic pump |
| Pause | seconds | Waits |

Default canisters: 1 sugar (fixed), 2 creamer, 3 chocolate, 4 French vanilla mix. Default syrups: vanilla, caramel. Chilled water feeds mixer 1. The 14 starting drinks: americano, coffee with cream, latte, cappuccino, mocha, vanilla latte, caramel latte, French vanilla cappuccino, hot chocolate, espresso, hot water, chilled americano, chilled latte, chilled mocha. "Chilled" drinks use chilled water; the machine has no ice maker.

## 7. What still needs doing on your side

- **Prototype adjustment.** Firmware 1.1.0 compiles with no warnings (Arduino ESP32 core 2.0.17, ArduinoJson 7.2.1) and the screens were tested in the simulator, but it has not run on a real machine. Expect to adjust motor timings, heater tuning and recipe amounts during the first build.
- **Remote access** from outside the store: use the v6 screen computer with Tailscale (needs internet at the station).

## File map

```
firmware/
  CoffeeController/          Arduino sketch for the KC868-A32
    config.h                 pins, I/O map, safety limits  <- check first
    main.cpp                 main loop, daily rinse, service button
    io.*                     relays (bus A), inputs (bus B), MCP23017 motors + thermoblock SSR,
                             ADS1115 temperatures, flow meter, watchdog heartbeat
    plant.*                  heaters, boiler fill, chiller, priming, faults, tablet charging
    machine.*                order engine: payment, cup, recipe steps, lid, pick-up
    vouchers.*               pay-at-the-counter codes
    store.*                  recipes, settings, stock and sales saved on the board
    web.*                    Wi-Fi and the API used by the tablet and owner app
    data/                    index.html (touchscreen), counter.html (cashier), owner.html (owner app), recipes.json
  prebuilt/                  ready-to-flash files (1.2.0) + SHA256SUMS.txt
android-kiosk/               optional Android kiosk app source (for the RCA tablet)
coffee-os/                   screen computer (CoffeeOS 2.0): install.sh, hub, control page, kiosk, tools, firmware copy
simulator/                   laptop simulator for both screens
```

## What changed in 1.1.0

| # | Problem in 1.0.0 | Fix in 1.1.0 |
| --- | --- | --- |
| 1 | The 32 inputs were read on the relay bus at the wrong addresses, so no sensor (leak, door, boiler level) worked. | Inputs are read on bus B (GPIO4/5) at 0x24, 0x25, 0x21, 0x22. A new critical fault stops heating if any I2C chip stops answering. |
| 2 | Staff could swap bottles but only the owner app could reset the water count. | "Bottles swapped" button on the counter screen (counter PIN). Optional level sensors on IN 18/19 stop sales when a bottle is really empty and reset the count when full bottles go in. |
| 3 | The nozzle motor used GPIO4 (input bus) and GPIO2 (boot pin); the flow meter used GPIO14 (RS-485 line). | Motors moved to the MCP23017; flow meter on GPIO33; watchdog heartbeat on GPIO32. |
| 4 | The motor code sent speed + direction; the BTS7960 needs forward and reverse inputs. | Forward and reverse are driven separately from the expander. |
| 5 | Temperatures assumed a 3.3 V input with a 10k resistor; the A32's analog inputs are 0-5 V and in a different order. | ADS1115 reader with 1k reference resistors for the hot probes; offsets in `config.h`. |
| 6 | Heater power was pulsed once a second through mechanical relays (up to 86,000 clicks a day). | Thermoblock SSR switched by the expander (no moving parts); boiler pulses in 10 s windows. |
| 7 | Relays hold their last position if the controller freezes. | GPIO32 heartbeat drives a missing-pulse relay that drops the heater contactor. The heartbeat also stops if the relay bus stops answering. |
| 8 | Power budget ignored the chiller. | Combined heater time is capped at 70% while the chiller runs. |
| - | Boiler could keep filling from an empty bottle. | Filling stops as soon as the machine knows the water ran out. |

## What changed in 1.2.0

| Change | Why |
| --- | --- |
| New setting `paused`. When true the menu shows "Closed" and no code works; heaters, rinses and alerts carry on | Close and open sales from CoffeeOS, by hand or on opening hours, without the "Get ready to move" cool-down |
| New setting `cupMode`: `turret` (default) or `droppers` | v8 replaces the bought cup dispenser with three printed droppers. In `droppers` mode, barrel 1, 2 and 3 (from `sizeBarrel`) run OUT 23, 24 and 25 until the cup-dropped sensor sees a cup or `dropperMaxMs` (1500 ms) passes; no turret, no lids |
| Owner summary reports `paused` and `cupMode` | So the control page can show them |

Nothing else changed: same pins, recipes, saved settings and web API. Built with Arduino ESP32 core 2.0.17 and ArduinoJson 7.2.1, no warnings. To update a working machine, load only `CoffeeController.ino.bin` at `0x10000` (or run `coffee-os/tools/flash-controller.sh`).

