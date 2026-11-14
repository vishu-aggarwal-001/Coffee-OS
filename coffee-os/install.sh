#!/bin/bash
# CoffeeOS installer: turns a fresh Raspberry Pi OS Lite (64-bit) into the coffee machine's screen.
#
#   sudo bash install.sh                       (asks a few questions)
#   sudo bash install.sh --name coffee-02 --machine-ssid CoffeeMachine-7F3A --machine-pass 'your-pass' \
#        --owner-pin 482913 --yes               (no questions)
#
# After a reboot the Pi starts straight into the touchscreen. Safe to run again to update.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST=/opt/coffee-os
CONF_DIR=/etc/coffee-hub
CONF=$CONF_DIR/config.json
BOOT=/boot/firmware
[ -d "$BOOT" ] || BOOT=/boot

NAME="" SSID="" PASS="" PIN="" NTFY="" MODE="ap" DISPLAY_KIND="touch2" DSI_PORT="" YES=0

usage() {
  sed -n '2,9p' "$0"
  cat <<EOF
Options:
  --name NAME            machine / Pi name, e.g. coffee-01 (also the network name of the Pi)
  --machine-ssid SSID    the controller's Wi-Fi name (CoffeeMachine-xxxx)
  --machine-pass PASS    the controller's Wi-Fi password
  --owner-pin PIN        the machine's owner PIN (lets the hub copy sales and send alerts)
  --ntfy TOPIC           phone alerts through the ntfy app (optional)
  --store-wifi           controller and Pi are both on the store Wi-Fi (controller at http://coffee.local)
  --display touch2|hdmi  Raspberry Pi Touch Display 2 (default) or an HDMI touch monitor
  --dsi0                 Touch Display 2 is plugged into the DSI/CAM port 0 instead of 1
  --yes                  don't ask questions; don't ask to reboot
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --name) NAME="$2"; shift 2;;
    --machine-ssid) SSID="$2"; shift 2;;
    --machine-pass) PASS="$2"; shift 2;;
    --owner-pin) PIN="$2"; shift 2;;
    --ntfy) NTFY="$2"; shift 2;;
    --store-wifi) MODE="store"; shift;;
    --display) DISPLAY_KIND="$2"; shift 2;;
    --dsi0) DSI_PORT=",dsi0"; shift;;
    --yes|-y) YES=1; shift;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1"; usage; exit 1;;
  esac
done

say()  { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!! %s\033[0m\n' "$*"; }
ask()  { local q="$1" def="${2:-}" a; if [ "$YES" = 1 ]; then echo "$def"; return; fi
         read -r -p "$q${def:+ [$def]}: " a </dev/tty; echo "${a:-$def}"; }

[ "$(id -u)" = 0 ] || { echo "Run with sudo: sudo bash $0"; exit 1; }
[ -f "$SRC/hub/coffee_hub.py" ] || { echo "Can't find hub/coffee_hub.py next to install.sh"; exit 1; }
. /etc/os-release
case "${VERSION_CODENAME:-}" in bookworm|trixie) ;; *) warn "Tested on Raspberry Pi OS bookworm and trixie; this is ${VERSION_CODENAME:-unknown}. Continuing.";; esac

# ---------------------------------------------------------------- questions
EXISTING=0; [ -f "$CONF" ] && EXISTING=1
if [ "$EXISTING" = 1 ]; then
  say "Updating an existing install (keeping $CONF)"
else
  [ -n "$NAME" ] || NAME=$(ask "Name for this machine" "coffee-01")
  if [ "$MODE" = ap ]; then
    [ -n "$SSID" ] || SSID=$(ask "Controller Wi-Fi name (shown as CoffeeMachine-xxxx)" "")
    [ -n "$PASS" ] || PASS=$(ask "Controller Wi-Fi password" "change-me-123")
  fi
  [ -n "$PIN" ] || PIN=$(ask "Owner PIN (leave empty to skip sales copy and alerts)" "")
  [ -n "$NTFY" ] || NTFY=$(ask "ntfy topic for phone alerts (optional)" "")
fi
NAME=$(echo "${NAME:-coffee-01}" | tr -cd 'a-zA-Z0-9-' | tr 'A-Z' 'a-z')

# ---------------------------------------------------------------- packages
say "Installing packages (needs internet, takes 5-15 minutes)"
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
BROWSER_PKG=chromium
apt-cache show chromium >/dev/null 2>&1 || BROWSER_PKG=chromium-browser
apt-get install -y --no-install-recommends cage "$BROWSER_PKG" python3 curl ca-certificates fonts-roboto \
  network-manager avahi-daemon
apt-get install -y --no-install-recommends esptool || warn "esptool not installed; tools/flash-controller.sh will offer to install it later"

# ---------------------------------------------------------------- users and files
say "Copying CoffeeOS to $DEST"
id coffeehub >/dev/null 2>&1 || useradd --system --home-dir /var/lib/coffee-hub --shell /usr/sbin/nologin coffeehub
id kiosk >/dev/null 2>&1 || useradd --create-home --shell /bin/bash kiosk
for g in video render input tty; do getent group "$g" >/dev/null && usermod -aG "$g" kiosk; done
mkdir -p "$DEST"
for d in hub ui kiosk tools; do rm -rf "${DEST:?}/$d"; cp -r "$SRC/$d" "$DEST/"; done
[ -d "$SRC/firmware" ] && { rm -rf "$DEST/firmware"; cp -r "$SRC/firmware" "$DEST/"; }
cp "$SRC/README.md" "$DEST/" 2>/dev/null || true
chmod 755 "$DEST"/kiosk/*.sh "$DEST"/tools/*.sh "$DEST/tools/coffee" "$DEST/hub/coffee_hub.py"
ln -sf "$DEST/tools/coffee" /usr/local/bin/coffee
chown -R root:root "$DEST"

if [ "$EXISTING" = 0 ]; then
  say "Writing $CONF"
  mkdir -p "$CONF_DIR"
  URL="http://192.168.4.1"; [ "$MODE" = store ] && URL="http://coffee.local"
  python3 - "$CONF" "$NAME" "$URL" "$PIN" "$NTFY" <<'PY'
import json, sys
path, name, url, pin, ntfy = sys.argv[1:]
cfg = {"machine_name": name, "machine_url": url, "listen_port": 8000, "data_dir": "/var/lib/coffee-hub",
       "ui_dir": "/opt/coffee-os/ui", "owner_pin": pin, "hub_pin": "", "sync_minutes": 5, "watch_seconds": 60,
       "ntfy_topic": ntfy, "ntfy_server": "https://ntfy.sh", "peers": []}
json.dump(cfg, open(path, "w"), indent=2)
PY
fi
chown root:coffeehub "$CONF"; chmod 640 "$CONF"

# ---------------------------------------------------------------- services
say "Setting up services"
cp "$SRC"/systemd/*.service "$SRC"/systemd/*.timer /etc/systemd/system/
systemctl daemon-reload
systemctl enable coffee-hub.service coffee-kiosk.service coffee-kiosk-refresh.timer
systemctl disable getty@tty1.service 2>/dev/null || true
systemctl set-default graphical.target

# Hardware watchdog: if Linux ever hangs, the Pi reboots itself in 15 s.
mkdir -p /etc/systemd/system.conf.d
printf '[Manager]\nRuntimeWatchdogSec=15\nRebootWatchdogSec=2min\n' > /etc/systemd/system.conf.d/coffee-watchdog.conf
# Keep logs small on the SD card.
mkdir -p /etc/systemd/journald.conf.d
printf '[Journal]\nSystemMaxUse=50M\n' > /etc/systemd/journald.conf.d/coffee.conf

# ---------------------------------------------------------------- name and network
if [ "$EXISTING" = 0 ]; then
  say "Naming this Pi '$NAME'"
  hostnamectl set-hostname "$NAME"
  sed -i "s/^127\.0\.1\.1.*/127.0.1.1\t$NAME/" /etc/hosts
  grep -q "^127.0.1.1" /etc/hosts || echo -e "127.0.1.1\t$NAME" >> /etc/hosts

  command -v raspi-config >/dev/null && raspi-config nonint do_wifi_country US || true
  if [ "$MODE" = ap ] && [ -n "$SSID" ]; then
    say "Adding the controller's Wi-Fi ($SSID). The Pi switches to it after the reboot."
    nmcli connection delete coffee-machine >/dev/null 2>&1 || true
    # never-default: if a store network cable or USB Wi-Fi is added later, internet still goes that way.
    nmcli connection add type wifi ifname wlan0 con-name coffee-machine ssid "$SSID" \
      wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$PASS" \
      connection.autoconnect yes connection.autoconnect-priority 100 connection.autoconnect-retries 0 \
      ipv4.never-default yes ipv6.method disabled >/dev/null
  fi
fi

# ---------------------------------------------------------------- display and boot
say "Setting up the screen and a quiet boot"
CFG="$BOOT/config.txt"; CMD="$BOOT/cmdline.txt"
[ -f "$CFG.coffee-backup" ] || cp "$CFG" "$CFG.coffee-backup"
[ -f "$CMD.coffee-backup" ] || cp "$CMD" "$CMD.coffee-backup"
sed -i '/# >>> coffee-os/,/# <<< coffee-os/d' "$CFG"
{
  echo "# >>> coffee-os"
  echo "[all]"
  echo "disable_splash=1"
  echo "# Pi 5 powered from the machine's 5 V supply (no USB-PD), so allow full USB current"
  echo "usb_max_current_enable=1"
  if [ "$DISPLAY_KIND" = touch2 ]; then
    grep -q "^dtoverlay=vc4-kms-v3d" "$CFG" || echo "dtoverlay=vc4-kms-v3d"
    echo "dtoverlay=vc4-kms-dsi-ili9881-7inch$DSI_PORT"
  fi
  echo "# <<< coffee-os"
} >> "$CFG"
for opt in quiet loglevel=3 logo.nologo vt.global_cursor_default=0 consoleblank=0; do
  grep -qw -- "$opt" "$CMD" || sed -i "1 s/\$/ $opt/" "$CMD"
done

systemctl start coffee-hub.service || true

say "Done"
cat <<EOF

  Machine name:     $NAME
  Touchscreen:      starts by itself after the reboot
  Control page:     http://$NAME.local:8000/control   (phone: open it, then Add to Home Screen)
  Sales page:       http://$NAME.local:8000/hub
  Service menu:     hold the touchscreen's top-left corner for 3 seconds
  From a terminal:  sudo coffee status   (also: open, close, rinse, swap, refill, test)
  Settings file:    $CONF   (after editing: sudo systemctl reload coffee-hub)
  Remote access:    sudo bash $DEST/tools/enable-remote.sh
  Status check:     sudo bash $DEST/tools/status.sh

  After the reboot this Pi joins the controller's Wi-Fi${SSID:+ ($SSID)}.
  To reach it with SSH, join that Wi-Fi with your laptop and run: ssh $(logname 2>/dev/null || echo pi)@$NAME.local
EOF

if [ "$YES" = 0 ]; then
  a=$(ask "Reboot now? (y/n)" "y")
  [ "$a" = y ] && reboot
fi
