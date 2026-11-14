#!/bin/bash
# Lets you open the hub page, the owner app and SSH from anywhere, using Tailscale (free for personal use).
# Needs internet on the Pi: a network cable to the store's router, or a USB Wi-Fi adapter on the store Wi-Fi
# (the Pi's built-in Wi-Fi stays on the controller's network).
#
#   sudo bash enable-remote.sh
#
# Then install Tailscale on your phone and sign in with the same account. Open http://<machine-name>:8000/hub
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "Run with sudo"; exit 1; }
if ! curl -fsS --max-time 5 https://pkgs.tailscale.com >/dev/null; then
  echo "No internet on this Pi. Plug in a network cable or a USB Wi-Fi adapter first."
  echo "For a USB Wi-Fi adapter: sudo nmcli device wifi connect 'Store WiFi' password 'xxxx' ifname wlan1"
  exit 1
fi
command -v tailscale >/dev/null || curl -fsSL https://tailscale.com/install.sh | sh
tailscale up --ssh --hostname "$(hostname)"
echo
echo "Remote access is on. From your phone (with Tailscale on):"
echo "  Hub page:   http://$(hostname):8000/hub"
echo "  Owner app:  http://$(hostname):8000/owner"
echo "To see all machines on one page, list the others in \"peers\" in /etc/coffee-hub/config.json, e.g."
echo '  "peers": ["http://coffee-02:8000", "http://coffee-03:8000"]'
echo "then: sudo systemctl reload coffee-hub"
