#!/bin/sh
# Restore the device's production mqtt.json / widgets.json — undoes
# whatever dev/switch-device.sh did. No-op if the .prod backups are absent.
set -eu

DEV="${ANDROID_SERIAL:-MSNS2D400E404501}"

adb -s "$DEV" shell '
set -e
cd /data/qpext
restored=0
for f in mqtt.json ha.json widgets.json; do
    if [ -f "$f.prod" ]; then
        mv -f "$f.prod" "$f"
        echo "[restore-remote] restored $f from $f.prod"
        restored=$((restored + 1))
    fi
done
if [ $restored -eq 0 ]; then
    echo "[restore-remote] no .prod backups found — already on production?"
fi
'

# Restart so the shim re-reads the restored configs.
adb -s "$DEV" shell "pkill -9 gst-launch; pkill QingSnow2App || true; sleep 1; pidof QingSnow2App.real | head -1 | xargs -I{} kill -9 {} 2>/dev/null || true" >/dev/null
echo "[restore] done."
