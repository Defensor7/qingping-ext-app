#!/bin/sh
# Boot the local HA debug stack:
#   - Mosquitto on host:1883 (anonymous)
#   - Home Assistant on host:8123, with ha_integration/qpext_airmonitor
#     mounted as a custom_component
#   - Onboarding completed automatically (admin/admin)
#   - MQTT integration pre-configured to point at the Mosquitto container
#   - A long-lived access token saved to dev/ha-config/.qpext-token for
#     dev/switch-device.sh to use
#
# When done:   dev/stop.sh                   (keep config dir)
# Hard reset:  dev/stop.sh; rm -rf dev/ha-config
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# Make sure ha-config exists so the volume mount creates a normal directory
# rather than docker-root-owned.
mkdir -p ha-config

DC() { docker compose --project-name qpext-dev "$@"; }

case "${1:-up}" in
    up)
        echo "[run] starting docker stack"
        DC up -d
        echo "[run] running onboarding bootstrap"
        python3 bootstrap_ha.py

        # Discover the Mac's LAN IP so the user can point the device at us.
        LAN_IP=$(ipconfig getifaddr en0 2>/dev/null || \
                 ipconfig getifaddr en1 2>/dev/null || \
                 hostname -I 2>/dev/null | awk '{print $1}')
        cat <<EOF

[run] Debug HA stack is ready.

  Open: http://localhost:8123
  User: admin
  Pass: admin

  Mosquitto:           localhost:1883     (anonymous)
  Long-lived token:    $HERE/ha-config/.qpext-token

To redirect the Snow2 device at this stack:
  $HERE/switch-device.sh ${LAN_IP:-<your-mac-LAN-ip>}

To revert the device back to production config:
  $HERE/restore-device.sh

To stop the stack (keeping the HA config dir for next time):
  $HERE/stop.sh
EOF
        ;;
    logs)
        DC logs -f
        ;;
    ps)
        DC ps
        ;;
    *)
        echo "usage: $0 {up|logs|ps}"
        exit 2
        ;;
esac
