#!/bin/bash
# One-screen health check for the touchscreen Pi.
echo "== Services"
for s in coffee-hub coffee-kiosk tailscaled; do
  printf '  %-14s %s\n' "$s" "$(systemctl is-active "$s" 2>/dev/null || true)"
done
echo "== Network"
nmcli -t -f DEVICE,STATE,CONNECTION device 2>/dev/null | sed 's/^/  /'
ip -4 -br addr | grep -v '^lo' | sed 's/^/  /'
echo "== Hub"
curl -fsS --max-time 3 http://127.0.0.1:8000/hub/api/health 2>/dev/null | python3 -m json.tool 2>/dev/null | sed 's/^/  /' || echo "  hub not answering"
echo "== Pi"
echo "  temp: $(vcgencmd measure_temp 2>/dev/null | cut -d= -f2)   throttled: $(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)  (0x0 = power OK)"
echo "  disk: $(df -h / | awk 'NR==2{print $3" used of "$2}')   uptime: $(uptime -p)"
echo "== Last hub messages"
journalctl -u coffee-hub -n 8 --no-pager 2>/dev/null | sed 's/^/  /'
