#!/bin/bash
# Runs inside Cage. Waits for the hub, then opens the touchscreen full screen.
# Chromium is restarted by systemd if it ever exits.

URL="${COFFEE_URL:-http://127.0.0.1:8000/}"
PROFILE="/tmp/coffee-kiosk-profile"     # fresh profile every start: no "restore pages?" bubbles, no stale cache

for _ in $(seq 1 60); do                  # up to 30 s for the hub to come up
  curl -fsS -o /dev/null --max-time 1 "${URL%/}/hub/api/health" && break
  sleep 0.5
done

BROWSER=$(command -v chromium || command -v chromium-browser)
rm -rf "$PROFILE"

exec "$BROWSER" \
  --kiosk "$URL" \
  --user-data-dir="$PROFILE" \
  --ozone-platform=wayland \
  --noerrdialogs \
  --disable-infobars \
  --no-first-run \
  --no-default-browser-check \
  --disable-session-crashed-bubble \
  --disable-translate \
  --disable-features=Translate,TranslateUI,TouchpadOverscrollHistoryNavigation,MediaRouter \
  --overscroll-history-navigation=0 \
  --disable-pinch \
  --password-store=basic \
  --check-for-update-interval=31536000 \
  --autoplay-policy=no-user-gesture-required \
  --enable-features=OverlayScrollbar
