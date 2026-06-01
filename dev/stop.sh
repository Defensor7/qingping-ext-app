#!/bin/sh
# Tear down the local HA debug stack. Keeps dev/ha-config/ so subsequent
# `dev/run.sh` calls reuse onboarding state. Pass --wipe to clear it.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

docker compose --project-name qpext-dev down

if [ "${1:-}" = "--wipe" ]; then
    echo "[stop] wiping dev/ha-config/"
    rm -rf ha-config
fi
