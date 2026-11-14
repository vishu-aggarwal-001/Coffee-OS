#!/bin/bash
# Update the controller's program from the Pi, over the USB cable between the Pi and the KC868-A32.
#
#   sudo bash flash-controller.sh                 update the program only (keeps PINs, Wi-Fi, recipes, sales log)
#   sudo bash flash-controller.sh --full          first-time load: program + screens, ERASES the controller's saved data
#   sudo bash flash-controller.sh --app my.bin    load a program file you built yourself
#
# Only do this with the machine idle (no drink being made). The controller restarts afterwards;
# heaters and pumps are off while it restarts.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/firmware"
MODE=app APP="$DIR/CoffeeController.ino.bin" PORT="" YES=0
while [ $# -gt 0 ]; do
  case "$1" in
    --full) MODE=full; shift;;
    --app) APP="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    --yes) YES=1; shift;;
    *) sed -n '2,10p' "$0"; exit 1;;
  esac
done
[ "$(id -u)" = 0 ] || { echo "Run with sudo"; exit 1; }

ESPTOOL=$(command -v esptool || command -v esptool.py || true)
if [ -z "$ESPTOOL" ]; then
  apt-get install -y esptool && ESPTOOL=$(command -v esptool || command -v esptool.py)
fi

if [ -z "$PORT" ]; then
  PORT=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n1 || true)
  [ -n "$PORT" ] || { echo "No USB serial port found. Is the USB cable from the Pi plugged into the KC868-A32?"; exit 1; }
fi

if [ "$MODE" = full ]; then FILE="$DIR/coffee-controller-full.bin"; ADDR=0x0; else FILE="$APP"; ADDR=0x10000; fi
[ -f "$FILE" ] || { echo "Missing $FILE"; exit 1; }

# Check the file against the published checksums when it's one of ours.
if [ -f "$DIR/SHA256SUMS.txt" ] && grep -q " $(basename "$FILE")\$" "$DIR/SHA256SUMS.txt"; then
  (cd "$(dirname "$FILE")" && grep " $(basename "$FILE")\$" "$DIR/SHA256SUMS.txt" | sha256sum -c -) || { echo "Checksum mismatch - not flashing."; exit 1; }
fi

# Busy? Ask the hub what the controller is doing.
PHASE=$(curl -fsS --max-time 3 http://127.0.0.1:8000/api/status 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('phase','idle'))" 2>/dev/null || echo unknown)
if [ "$PHASE" != idle ] && [ "$PHASE" != unknown ]; then echo "The machine is busy ($PHASE). Try again when it's idle."; exit 1; fi

echo "Port:  $PORT"
echo "File:  $FILE at $ADDR"
if [ "$MODE" = full ]; then
  echo "FULL load: the controller's PINs, Wi-Fi settings, recipes, stock and sales log will be reset to factory values."
  echo "The hub keeps its own copy of sales it has already copied."
fi
if [ "$YES" = 0 ]; then read -r -p "Flash now? (y/n) " a; [ "$a" = y ] || exit 0; fi

# Copy the latest sales off the controller first.
PIN=$(python3 -c "import json;print(json.load(open('/etc/coffee-hub/config.json')).get('hub_pin') or json.load(open('/etc/coffee-hub/config.json')).get('owner_pin',''))" 2>/dev/null || true)
[ -n "$PIN" ] && curl -fsS -X POST -H "X-Hub-Pin: $PIN" --max-time 40 http://127.0.0.1:8000/hub/api/sync >/dev/null 2>&1 || true

"$ESPTOOL" --chip esp32 --port "$PORT" --baud 460800 write_flash "$ADDR" "$FILE"
echo "Done. The controller is restarting. Check the owner app's Today tab for faults."
