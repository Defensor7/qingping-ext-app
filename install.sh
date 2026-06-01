#!/bin/sh
# One-shot installer for the qpext shim on a rooted Qingping Air Monitor 2.
#
# Modes:
#   ./install.sh                  # from a clone, full install
#   curl -fsSL …/install.sh | sh  # bootstrap: clones repo, then installs
#   ./install.sh --uninstall      # restore the original QingSnow2App binary
#   ./install.sh --update         # git pull + rebuild + redeploy
#
# Env vars / flags:
#   ANDROID_SERIAL          target device (default: first device adb sees)
#   --device <serial>       same as ANDROID_SERIAL
#   --no-build              skip zig build (use existing qpext/qpext.so)
#   --no-seed-config        skip the local cameras.json / widgets.json seeding
#
# Prerequisites: adb, zig (>= 0.13). The script will surface clear errors if
# either is missing. Root access on the device must already be set up — see
# https://github.com/ea/cgs2_decloud for that part.

set -eu

REPO_URL="https://github.com/Defensor7/qingping-ext-app.git"
REPO_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/qingping-ext-app"

# ---- ANSI helpers ----------------------------------------------------------
if [ -t 1 ]; then
    cBOLD=$(printf '\033[1m')   ; cDIM=$(printf '\033[2m')
    cGRN=$(printf '\033[32m')   ; cYEL=$(printf '\033[33m')
    cRED=$(printf '\033[31m')   ; cRST=$(printf '\033[0m')
else
    cBOLD= ; cDIM= ; cGRN= ; cYEL= ; cRED= ; cRST=
fi
say()  { printf "%s[install]%s %s\n" "$cBOLD" "$cRST" "$*"; }
warn() { printf "%s[install]%s %s%s%s\n" "$cBOLD" "$cRST" "$cYEL" "$*" "$cRST" >&2; }
die()  { printf "%s[install]%s %s%s%s\n" "$cBOLD" "$cRST" "$cRED" "$*" "$cRST" >&2; exit 1; }

# ---- flag parsing ----------------------------------------------------------
ACTION="install"
DEVICE="${ANDROID_SERIAL:-}"
DO_BUILD=1
DO_SEED=1
while [ $# -gt 0 ]; do
    case "$1" in
        --uninstall|--undo)  ACTION="uninstall" ;;
        --update)            ACTION="update" ;;
        --device)            shift; DEVICE="$1" ;;
        --no-build)          DO_BUILD=0 ;;
        --no-seed-config)    DO_SEED=0 ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)  die "unknown argument: $1" ;;
    esac
    shift
done

# ---- dependency checks -----------------------------------------------------
need() { command -v "$1" >/dev/null 2>&1 || die "missing dependency: $1 — $2"; }
need adb "install Android platform-tools (brew install --cask android-platform-tools, or apt install adb)"
[ $DO_BUILD -eq 1 ] && need zig "install zig (brew install zig, or https://ziglang.org/download/)"
[ "$ACTION" = "update" ] && need git "git is required for --update"

# ---- locate (or fetch) the repo --------------------------------------------
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/qpext/build.sh" ] && [ -d "$SCRIPT_DIR/qpext/qml" ]; then
    REPO="$SCRIPT_DIR"
    say "running from repo at ${cDIM}$REPO${cRST}"
elif [ "$ACTION" = "install" ] || [ "$ACTION" = "update" ]; then
    need git "git is required to clone the repo when running curl|sh-style"
    if [ ! -d "$REPO_CACHE/.git" ]; then
        say "cloning $REPO_URL into $REPO_CACHE"
        mkdir -p "$(dirname "$REPO_CACHE")"
        git clone --depth 1 "$REPO_URL" "$REPO_CACHE"
    elif [ "$ACTION" = "update" ]; then
        say "updating $REPO_CACHE"
        git -C "$REPO_CACHE" pull --ff-only
    fi
    REPO="$REPO_CACHE"
else
    die "couldn't find a qpext/ tree next to this script"
fi
cd "$REPO"

# ---- device selection ------------------------------------------------------
if [ -z "$DEVICE" ]; then
    DEVICE=$(adb devices 2>/dev/null | awk 'NR>1 && $2=="device" {print $1; exit}')
    [ -z "$DEVICE" ] && die "no adb device found; connect the Snow2 and run 'adb devices'"
fi
adb -s "$DEVICE" wait-for-device
PRODUCT=$(adb -s "$DEVICE" shell 'cat /proc/sys/kernel/hostname 2>/dev/null || echo unknown' | tr -d '\r')
say "target device: ${cBOLD}$DEVICE${cRST} (${cDIM}$PRODUCT${cRST})"

if ! adb -s "$DEVICE" shell '[ -e /qingping/bin/QingSnow2App ] && echo OK' 2>/dev/null | grep -q OK; then
    die "/qingping/bin/QingSnow2App not found — is this the right device?"
fi

# ---- uninstall path --------------------------------------------------------
if [ "$ACTION" = "uninstall" ]; then
    say "uninstalling: restoring original QingSnow2App"
    ANDROID_SERIAL="$DEVICE" "$REPO/qpext/deploy.sh" undo
    say "uninstall complete; /data/qpext/ kept in case you re-install"
    exit 0
fi

# ---- build .so -------------------------------------------------------------
if [ $DO_BUILD -eq 1 ]; then
    say "building qpext.so (zig c++ → aarch64-linux-gnu.2.31)"
    "$REPO/qpext/build.sh"
else
    [ -f "$REPO/qpext/qpext.so" ] || die "--no-build given, but qpext/qpext.so does not exist"
fi

# ---- prepare local config files --------------------------------------------
# Editable-locally → deploy.sh sync sends the whole qml/ tree to the device.
# For each user-managed file (mqtt.json, ha.json, cameras.json):
#   (a) if missing locally but the device has one → pull it down
#   (b) otherwise → seed from the .example template, ask the user to edit
# widgets.json is NOT user-managed any more — the qpext_airmonitor HA
# integration owns it and the shim writes it on every dashboard/set MQTT
# message. We migrate any legacy widgets.json that still has an embedded
# `ha` block: ha.* moves to ha.json, the rest stays as the cache.
NEEDS_EDIT=0
if [ $DO_SEED -eq 1 ]; then
    # Migration: extract ha.* out of a legacy widgets.json into ha.json.
    if [ ! -f "$REPO/qpext/qml/ha.json" ] && \
       [ -f "$REPO/qpext/qml/widgets.json" ] && \
       grep -q '"ha"' "$REPO/qpext/qml/widgets.json" 2>/dev/null; then
        say "migrating legacy ha.* from widgets.json → ha.json"
        python3 - "$REPO/qpext/qml/widgets.json" "$REPO/qpext/qml/ha.json" <<'PYEOF'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
with open(src) as f: data = json.load(f)
ha = data.pop("ha", None)
if ha:
    with open(dst, "w") as f: json.dump(ha, f, indent=2, ensure_ascii=False)
with open(src, "w") as f: json.dump(data, f, indent=2, ensure_ascii=False)
PYEOF
    fi

    for f in mqtt.json ha.json cameras.json; do
        local_f="$REPO/qpext/qml/$f"
        if [ ! -f "$local_f" ]; then
            if adb -s "$DEVICE" shell "[ -s /data/qpext/$f ] && echo OK" 2>/dev/null | grep -q OK; then
                say "pulling existing /data/qpext/$f → qpext/qml/$f"
                adb -s "$DEVICE" pull "/data/qpext/$f" "$local_f" >/dev/null
            else
                say "seeding qpext/qml/$f from .example"
                cp "$REPO/qpext/qml/$f.example" "$local_f"
                NEEDS_EDIT=1
            fi
        fi
    done

    # Placeholder detection. The strings below appear ONLY in the .example
    # templates; any real config will have replaced them.
    if grep -q 'PUT_LONG_LIVED_ACCESS_TOKEN_HERE' "$REPO/qpext/qml/ha.json" 2>/dev/null; then
        warn "qpext/qml/ha.json still has a placeholder \`token\`"
        NEEDS_EDIT=1
    fi
    if grep -q 'USER:PASS' "$REPO/qpext/qml/cameras.json" 2>/dev/null; then
        warn "qpext/qml/cameras.json still has a placeholder RTSP URL"
        NEEDS_EDIT=1
    fi
    if grep -qE 'PUT_MQTT_(USERNAME|PASSWORD)_HERE' "$REPO/qpext/qml/mqtt.json" 2>/dev/null; then
        warn "qpext/qml/mqtt.json still has placeholder broker credentials"
        NEEDS_EDIT=1
    fi

    if [ $NEEDS_EDIT -eq 1 ]; then
        cat <<EOF

${cBOLD}Edit these files locally before continuing:${cRST}
  $REPO/qpext/qml/mqtt.json      → MQTT broker host/port/credentials
  $REPO/qpext/qml/ha.json        → HA base_url + long-lived access token
  $REPO/qpext/qml/cameras.json   → RTSP URL with camera credentials

All three are .gitignored, so accidental commits aren't possible. When done:
  ${cBOLD}$0${cRST}
EOF
        exit 0
    fi
fi

# ---- deploy ----------------------------------------------------------------
say "deploying QML + .so + wrapper (this runs qpext/deploy.sh install)"
ANDROID_SERIAL="$DEVICE" "$REPO/qpext/deploy.sh" install

# ---- summary ---------------------------------------------------------------
cat <<EOF

${cGRN}[install] done.${cRST}

Useful commands:
  ${cBOLD}$REPO/qpext/deploy.sh logs${cRST}          tail the qpext log
  ${cBOLD}$REPO/qpext/deploy.sh status${cRST}        show deployed files + running process
  ${cBOLD}$REPO/qpext/deploy.sh push-qml${cRST}      hot-push QML changes (no restart)
  ${cBOLD}$0 --update${cRST}            git pull + rebuild + redeploy
  ${cBOLD}$0 --uninstall${cRST}         restore the original QingSnow2App

Home Assistant integration (optional):
  cp -r $REPO/ha_integration/qpext_airmonitor \\
        \$HA_CONFIG/custom_components/qpext_airmonitor
  Restart HA. The device will appear in Settings → Devices & Services →
  Discovered (the shim publishes a retained presence message to
  qpext/<mac>/info that HA picks up automatically).
EOF
