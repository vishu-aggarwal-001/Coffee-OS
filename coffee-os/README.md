# CoffeeOS 2.0: the touchscreen computer for the Bean-to-Cup machine

CoffeeOS turns a Raspberry Pi 5 with the official 7" Touch Display 2 into the machine's screen. It boots straight into the customer menu, with no desktop for anyone to escape to, and it runs a small server on the Pi called the hub. Step-by-step pictures for the first install are in the picture manual, screen steps A to F.

```
  Customer ──touch──►  Raspberry Pi 5 + Touch Display 2   (CoffeeOS)
                        Chromium full screen + Coffee Hub on port 8000
                                     │
                                     │ Wi-Fi (the controller's own network)
                                     ▼
  You, on your phone ──► /control   KinCony KC868-A32 (ESP32), firmware 1.2.0
  Cashier ──► /counter              makes every drink and owns every heater,
  Sales and alerts ──► /hub         valve and safety cut-off
```

The controller stays in charge. Every button in CoffeeOS turns into an ordinary request to the controller, which checks it against its own safety rules first. The Pi can freeze, reboot or be unplugged mid-drink and the drink still finishes safely.

## What's new in 2.0: easier control

| You want to | Do this |
|---|---|
| Run the machine from your phone | Open `http://coffee-01.local:8000/control`, sign in with the owner PIN, then Add to Home Screen |
| Stop or start sales | The big Close sales / Open for sales button. Heaters stay on, so opening again is instant |
| Open and close at set hours | Opening hours on the control page: one row per day, then Save hours |
| Do a job at the machine | Hold the top-left corner of the touchscreen for 3 seconds, type the PIN on the keypad |
| Mark something as refilled | Stock, then Refilled next to it. The bar fills and the low-stock alert resets |
| Know when stock runs low | It's automatic: a phone alert when beans, a powder, a syrup or a cup size runs low |
| See the day's sales without looking | A phone message every evening (default 8:30 pm) with drinks, money and anything low |
| Work from a terminal | `sudo coffee status`, `coffee open`, `coffee rinse`, `coffee refill beans` and so on |

### The control page

One page, one PIN, and the hub sends the owner PIN to the controller for you. From top to bottom:

- **Status:** selling, closed, heating up or stopped (and why), the drink being made, temperatures, water and today's sales.
- **Open / Close:** one big button. Closing asks first; opening doesn't.
- **Quick jobs:** Bottles swapped, Rinse now, Prime water, Clear faults.
- **Stock:** a bar for beans, each canister, each syrup and each cup size, with a Refilled button. "Full" comes from `capacity` in the settings file.
- **Test a drink:** pick the drink and size, and it's made free.
- **Opening hours:** turn it on, set each day's open and close times (or switch a day off), and save. The hours only act when a time arrives, so pressing Open at 6:45 before a 7:00 opening keeps it open, and the 7:00 opening simply agrees. For hours past midnight, give that day a close time earlier than its open time.
- **More:** unlock the door, stop a stuck drink, free mode, cup system (turret or the v8 printed droppers), move the machine, restart the controller. Anything that could surprise someone asks first.
- **What happened:** the last 15 actions, refills, faults and stock alerts, with who did them (the touchscreen or a phone's address).

The page refreshes every 5 seconds. In service mode on the touchscreen it goes back to the customer menu after 2 minutes without a touch, and it never stores the PIN.

### Phone alerts

With `ntfy_topic` set, the hub sends:

- a fault that stops sales, and the all-clear when it's fixed
- water low or empty
- the controller not answering for 2 minutes, and back again
- new in 2.0: running low on stock, once per item until you tap Refilled (limits are `low_stock` in the settings)
- new in 2.0: the end-of-day message at `daily_report`

### The `coffee` command

Installed with CoffeeOS for SSH and keyboard use. It reads the PIN from the settings file, so run it with sudo.

```
sudo coffee status                 what it's doing, temperatures, stock, today's sales
sudo coffee open | close
sudo coffee rinse | prime | swap | clear | unlock | stop | restart
sudo coffee test latte 16          free test drink (drink ids are in the owner app)
sudo coffee refill beans           also can1..can4, syrup1, syrup2, cups10, cups12, cups16
sudo coffee log                    the last things that happened
```

## Firmware 1.2.0

CoffeeOS 2.0 needs controller firmware 1.2.0, included in `firmware/`. It adds two settings and changes nothing else:

- `paused`: true means closed. The menu shows "Closed", no counter code works, and heaters, rinse and alerts carry on. The Open/Close button and the opening hours use it.
- `cupMode`: `turret` (the old cup dispenser) or `droppers` (the three printed droppers, motors on outputs 23, 24 and 25, no lids). `sizeBarrel` in the recipes still says which size uses which dropper.

To load it on a working machine: `sudo bash /opt/coffee-os/tools/flash-controller.sh` (keeps PINs, Wi-Fi, recipes and sales). It builds with Arduino ESP32 core 2.0.17 and ArduinoJson 7.2.1 with no warnings.

## Parts (per machine)

The links are in Buy_List_v8.pdf.

| Part | Notes |
|---|---|
| Raspberry Pi 5, 2 GB | Plenty for one browser page |
| Raspberry Pi Touch Display 2, 7" (720 × 1280, portrait) | Outside 189.3 × 120.2 mm; mounts in printed holder H01 behind bezel P14 |
| Active Cooler for Pi 5 | The cabinet runs warm |
| 32 GB high-endurance microSD | Copes with daily unplugging better than a normal card |
| USB cable, Pi to the KC868-A32 | For firmware updates from the Pi |
| USB-C power cable with bare ends (20 AWG or thicker) | From the 5 V supply's terminals into the Pi |
| Mean Well MDR-40-5 (5 V, 6 A) | The Pi 5 and display need more than 3 A |

Wiring: run the USB-C power cable from the 5 V supply's +V and −V terminals straight into the Pi, not through relay OUT 30, and measure 5.0 to 5.2 V at the plug before connecting it. The display takes its power from the Pi through its own small cable.

## Install (about 30 minutes)

You need internet for the install. After that the Pi works without it.

1. Install Raspberry Pi Imager 2.0 or newer. Choose Raspberry Pi 5, then Raspberry Pi OS (other), then Raspberry Pi OS Lite (64-bit). In its settings set a user name and password, turn on SSH and enter your home Wi-Fi. Write the card.
2. Copy this whole `coffee-os` folder onto the card's bootfs drive.
3. Put the card in the Pi, connect the display and power on. Wait 2 minutes.
4. SSH in (`ssh yourname@raspberrypi.local`) or plug in a keyboard, then run:
   ```
   sudo bash /boot/firmware/coffee-os/install.sh
   ```
   It asks for the machine's name (such as `coffee-01`), the controller's Wi-Fi name and password, the owner PIN and an optional ntfy topic, then reboots.
5. The Pi comes back showing the customer menu, on the controller's Wi-Fi.

With no keyboard and no SSH, use `cloud-init/user-data-addon.yaml`: paste a few lines into a file on the card and the Pi installs itself on first boot.

To update CoffeeOS later, copy the new folder to the Pi and run `install.sh` again. Your settings in `/etc/coffee-hub/config.json` are kept, and new settings take their defaults until you add them.

## Addresses

| Page | Address | PIN |
|---|---|---|
| Customer menu | `http://coffee-01.local:8000/` | none |
| Control page (new) | `http://coffee-01.local:8000/control` | hub PIN |
| Service menu (new) | hold the touchscreen's top-left corner 3 s, or `/service` | hub PIN |
| Cashier screen | `http://coffee-01.local:8000/counter` | counter PIN |
| Owner app (recipes, prices, Wi-Fi) | `http://coffee-01.local:8000/owner` | owner PIN |
| Sales and history | `http://coffee-01.local:8000/hub` | hub PIN |

The old `http://192.168.4.1/...` addresses still work for the cashier screen and owner app.

Remote access: give the Pi internet (a network cable to the store router, or a USB Wi-Fi adapter on the store Wi-Fi; the built-in Wi-Fi stays on the controller's network), run `sudo bash /opt/coffee-os/tools/enable-remote.sh` and sign in to Tailscale. With Tailscale on your phone on the same account, `http://coffee-01:8000/control` works from anywhere.

## Settings file: `/etc/coffee-hub/config.json`

After editing: `sudo systemctl reload coffee-hub`. `hub/config.example.json` lists every setting.

| Setting | What it does |
|---|---|
| `machine_name` | Name on the pages and in alerts |
| `machine_url` | The controller: `http://192.168.4.1` on its own Wi-Fi, or `http://coffee.local` on the store Wi-Fi |
| `owner_pin` | The machine's owner PIN. When you change it in the owner app, change it here too. If it's wrong, the hub stops after one try (so it can't lock you out) and says so on the pages |
| `hub_pin` | PIN for /control, /service and /hub. Empty means the same as `owner_pin` |
| `ntfy_topic` | Phone alerts topic. Empty means no alerts |
| `capacity` | What Refilled sets each count to: `beans_g`, `canister_g`, `syrup_ml`, `cups` |
| `low_stock` | Alert below these: `beans_g`, `canister_g`, `syrup_ml`, `cups` |
| `canister_names`, `syrup_names` | Names on the control page and in alerts |
| `daily_report` | Time of the end-of-day message, such as `"20:30"`. `""` turns it off |
| `service_corner` | `true`: holding the top-left corner opens the service menu. `false` turns that off |
| `peers` | Other machines' hubs for the all-machines view on /hub |
| `sync_minutes`, `watch_seconds` | How often sales are copied (5 min) and the machine is checked (60 s) |

Opening hours are saved by the control page in `/var/lib/coffee-hub/schedule.json`.

## Service

- Health check: `sudo bash /opt/coffee-os/tools/status.sh`.
- To get out of the touchscreen, plug in a USB keyboard and press Ctrl+Alt+F2 (Ctrl+Alt+F1 goes back), or SSH in. At a station, join the `CoffeeMachine-xxxx` Wi-Fi with a laptop and `ssh yourname@coffee-01.local`.
- Logs: `journalctl -u coffee-hub -f` and `journalctl -u coffee-kiosk -f`.
- Restart the screen: `sudo systemctl restart coffee-kiosk`. It also restarts itself every night at 3:40 am.
- If Linux hangs, the Pi's hardware watchdog reboots it within 15 seconds.
- Pulling the plug is safe: the hub writes every sale to disk at once and flushes everything on Get ready to move.

## Try it on your laptop first

```
cd software/simulator && python3 simulator.py        # the fake machine, port 8080
cd ../coffee-os && python3 hub/coffee_hub.py --config none --machine http://127.0.0.1:8080 --port 8000 --data ./hubdata --owner-pin 000000
```

Then open `http://localhost:8000/control` (PIN 000000), `/`, `/counter`, `/owner` and `/hub`. The simulator speaks firmware 1.2.0, including Close and the cup modes.

## What's in this folder

```
install.sh                 sets up a fresh Raspberry Pi OS Lite
hub/coffee_hub.py          the hub server (Python standard library only)
hub/control.html           control page and touchscreen service menu (new in 2.0)
hub/hub.html               sales, history and all machines
hub/config.example.json    every setting
ui/                        customer, counter and owner screens; the customer menu
                           has the service corner
kiosk/start-browser.sh     starts Chromium full screen inside Cage
systemd/                   services: hub, touchscreen, nightly refresh
tools/coffee               the command-line control (new in 2.0)
tools/                     status check, remote access, firmware update from the Pi
firmware/                  firmware 1.2.0 prebuilt files and checksums
cloud-init/                optional no-keyboard install
```

## Tested and not tested

Tested here against the simulator: every control-page button, refills, the opening hours closing the machine on schedule, the hours-past-midnight case, low-stock alerts firing once, the end-of-day message, a wrong PIN on the control page, unknown buttons being refused, the `coffee` command, and the 3-second corner hold opening the service menu. The control page was rendered at phone width and at the display's 720 × 1280 with no browser errors. Firmware 1.2.0 compiles with no warnings.

Not tested: anything on a real Raspberry Pi or a real controller, including `install.sh`, the printed cup droppers and firmware 1.2.0's dropper timing. Expect small adjustments on the first machine.
