#!/bin/sh
# Build qpext.so for the Snow2 target: aarch64-linux-gnu, glibc ≥ 2.31.
# Zig handles cross-compilation + statically linked libstdc++ — the resulting
# .so depends only on libc.so.6, libpthread.so.0, libdl.so.2 (matches what the
# device already exposes).
#
# Usage:
#   ./build.sh            # build qpext.so
#   ./build.sh deploy     # build, push, restart app, wipe gst-launch zombies
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

zig c++ \
    -target aarch64-linux-gnu.2.31 \
    -shared -fPIC -O2 \
    -Wl,--version-script=qpext.ver \
    -o qpext.so \
    qpext.cpp qpext_ha.cpp qpext_mqtt.cpp \
    -lpthread -ldl

ls -l qpext.so

if [ "${1:-}" = "deploy" ]; then
    DEV="${ANDROID_SERIAL:-MSNS2D400E404501}"
    adb -s "$DEV" push qpext.so /data/qpext/qpext.so
    adb -s "$DEV" shell "pkill -9 gst-launch; pkill QingSnow2App; sleep 1; pkill -9 gst-launch"
    echo "[build] deployed; watchdog will respawn QingSnow2App in ~5s"
fi
