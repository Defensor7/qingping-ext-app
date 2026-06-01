#!/bin/sh
# Redirect the Snow2 device at the local debug HA stack started by dev/run.sh.
#
# Backs up `/data/qpext/mqtt.json` and `/data/qpext/widgets.json` on the
# device (renamed with the `.prod` suffix), then writes new versions
# pointing at:
#   - the Mac's local broker (anonymous mosquitto on host:1883)
#   - the local HA WebSocket (http://<mac-ip>:8123) with the long-lived
#     token minted by bootstrap_ha.py
#
# Run dev/restore-device.sh to revert.
#
# Usage:   dev/switch-device.sh [mac-ip]
#          (defaults to ipconfig getifaddr en0/en1)
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
DEV="${ANDROID_SERIAL:-MSNS2D400E404501}"
TOKEN_FILE="$HERE/ha-config/.qpext-token"

LAN_IP="${1:-}"
if [ -z "$LAN_IP" ]; then
    LAN_IP=$(ipconfig getifaddr en0 2>/dev/null || \
             ipconfig getifaddr en1 2>/dev/null || true)
fi
[ -z "$LAN_IP" ] && {
    echo "couldn't detect Mac LAN IP — pass it explicitly: $0 <ip>"
    exit 2
}
[ -f "$TOKEN_FILE" ] || {
    echo "$TOKEN_FILE not found — run dev/run.sh first"
    exit 2
}
TOKEN=$(cat "$TOKEN_FILE" | tr -d '\r\n')

echo "[switch] device → debug stack at $LAN_IP"

# 1. Snapshot the production configs. widgets.json is included so
#    `restore-device.sh` can put back any in-place state from before the
#    debug HA integration started overwriting it.
adb -s "$DEV" shell '
set -e
cd /data/qpext
for f in mqtt.json ha.json widgets.json; do
    if [ -f "$f" ] && [ ! -f "$f.prod" ]; then
        cp "$f" "$f.prod"
        echo "[switch-remote] backed up $f → $f.prod"
    fi
done
'

# 2. Write debug mqtt.json (anonymous broker on the Mac).
TMP=$(mktemp)
cat > "$TMP" <<EOF
{
  "host":     "$LAN_IP",
  "port":     1883,
  "username": "",
  "password": ""
}
EOF
adb -s "$DEV" push "$TMP" /data/qpext/mqtt.json >/dev/null
rm "$TMP"

# 3. Write debug ha.json pointing at the local HA + the long-lived token
#    minted by bootstrap_ha.py. widgets.json is left alone — once the user
#    completes the discovery flow in the debug HA, the integration will
#    publish its (initially empty) widget composition over MQTT and the
#    shim will overwrite widgets.json accordingly.
TMP=$(mktemp)
cat > "$TMP" <<EOF
{
  "base_url": "http://$LAN_IP:8123",
  "token":    "$TOKEN"
}
EOF
adb -s "$DEV" push "$TMP" /data/qpext/ha.json >/dev/null
rm "$TMP"
echo "[switch] /data/qpext/ha.json rewritten for debug stack"

# 4. Force a clean restart so MQTT reconnects to the new broker.
adb -s "$DEV" shell "pkill -9 gst-launch; pkill QingSnow2App || true; sleep 1; pidof QingSnow2App.real | head -1 | xargs -I{} kill -9 {} 2>/dev/null || true" >/dev/null
echo "[switch] done. The device should appear under Settings → Devices & Services → Discovered within ~10 s."
