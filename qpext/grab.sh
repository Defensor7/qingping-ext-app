#!/bin/sh
# Trigger a screenshot via the QML-side polling loop and pull it back.
DEV="${ANDROID_SERIAL:-MSNS2D400E404501}"
OUT="${1:-/tmp/qpext_grab.png}"
adb -s "$DEV" shell "mkdir -p /tmp/qpext; date +%s > /tmp/qpext/grab.req"
sleep 1
adb -s "$DEV" pull /tmp/qpext/grab.png "$OUT" 2>&1 | tail -1
ls -la "$OUT"
