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
#   --no-seed-config        don't auto-create cameras.json / widgets.json on device
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

# ---- seed config files on device if absent ---------------------------------
if [ $DO_SEED -eq 1 ]; then
    for f in cameras.json widgets.json; do
        if ! adb -s "$DEVICE" shell "[ -s /data/qpext/$f ] && echo OK" 2>/dev/null | grep -q OK; then
            warn "/data/qpext/$f missing — seeding from qpext/qml/$f.example"
            adb -s "$DEVICE" shell "mkdir -p /data/qpext" >/dev/null
            adb -s "$DEVICE" push "$REPO/qpext/qml/$f.example" "/data/qpext/$f" >/dev/null
            warn "  → edit /data/qpext/$f on the device to put real credentials in"
        fi
    done
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
