#!/bin/sh
# Deploy qpext shim + QML to the Snow2 device.
#
# Layout on device:
#   /qingping/bin/QingSnow2App           -> shell wrapper (sets LD_PRELOAD, exec)
#   /qingping/bin/QingSnow2App.real      -> renamed original binary
#   /qingping/bin/QingSnow2App.bak       -> factory backup (already exists)
#   /data/qpext/qpext.so                 -> preload shim
#   /data/qpext/{main,MainPage}.qml      -> patched UI entry
#   /data/qpext/Plugins/Hello.qml        -> our custom tab
#
# Idempotent. `deploy.sh undo` restores the original binary.
set -eu

DEV="${ANDROID_SERIAL:-MSNS2D400E404501}"
REMOTE_ROOT=/data/qpext
APP=/qingping/bin/QingSnow2App
APP_REAL=/qingping/bin/QingSnow2App.real
HERE="$(cd "$(dirname "$0")" && pwd)"

run()  { adb -s "$DEV" shell "$@"; }
push() { adb -s "$DEV" push "$1" "$2" >/dev/null; }

# Refresh qml/version.txt from the git-describe-derived version so HelloImpl
# can show it in the corner of the HA tab and the user can tell at a glance
# what's deployed. Called before every install / push-qml.
write_version() {
    v=$(sh "${HERE}/../version.sh" 2>/dev/null || echo dev)
    printf '%s\n' "$v" > "${HERE}/qml/version.txt"
    echo "[deploy] version: $v"
}

cmd_install() {
    write_version
    echo "[deploy] push files to ${DEV}:${REMOTE_ROOT}"
    run "mkdir -p ${REMOTE_ROOT}"
    # Sync the entire qml/ tree (Header/, Notification/, Plugins/, ...).
    adb -s "$DEV" push --sync "${HERE}/qml/." "${REMOTE_ROOT}/" >/dev/null
    push "${HERE}/qpext.so" "${REMOTE_ROOT}/qpext.so"

    echo "[deploy] install wrapper at ${APP}"
    run "
        set -e
        if [ ! -f ${APP_REAL} ]; then
            # First-time install: stash the real binary.
            mv ${APP} ${APP_REAL}
        fi
        cat > ${APP} <<'WRAPPER'
#!/bin/sh
# qpext wrapper — sets LD_PRELOAD then execs the real binary.
export LD_PRELOAD=/data/qpext/qpext.so
exec /qingping/bin/QingSnow2App.real \"\$@\"
WRAPPER
        chmod 755 ${APP}
        echo '[deploy-remote] wrapper installed:'
        ls -la ${APP} ${APP_REAL}
    "

    echo "[deploy] kill QingSnow2App (watchdog respawns in ~5s)"
    run "pkill QingSnow2App || true"
    echo
    echo "[deploy] done. Tail logs:"
    echo "  $0 logs"
}

cmd_undo() {
    echo "[deploy] restore original binary"
    run "
        if [ -f ${APP_REAL} ]; then
            mv -f ${APP_REAL} ${APP}
            echo '[deploy-remote] restored'
        else
            echo '[deploy-remote] nothing to restore (no .real)'
        fi
        pkill QingSnow2App || true
        ls -la ${APP}
    "
}

cmd_logs() {
    run "
        echo '--- last app log ---'
        tail -n 100 /data/etc/log/\$(date +%Y-%m-%d).log 2>/dev/null || true
        echo
        echo '--- watchdog log ---'
        tail -n 50 /data/etc/debug/watchdog.log 2>/dev/null || true
        echo
        echo '--- process ---'
        ps -ef | grep -E 'QingSnow|qpext' | grep -v grep || true
    "
}

cmd_status() {
    run "
        echo '--- files on device ---'
        ls -la ${REMOTE_ROOT}/ ${REMOTE_ROOT}/Plugins/ 2>/dev/null
        echo
        echo '--- app & wrapper ---'
        ls -la ${APP} ${APP_REAL} 2>/dev/null
        echo
        echo '--- file head ---'
        head -5 ${APP} 2>/dev/null
        echo
        echo '--- process ---'
        ps -ef | grep -E 'QingSnow' | grep -v grep || echo '(no QingSnow process)'
    "
}

cmd_push_qml() {
    # Hot-reload friendly path: sync QML/fonts/version.txt only, don't restart
    # the app. Hello.qml polls HelloImpl.qml every ~1.5s and reloads its Loader.
    #
    # *.json files are explicitly excluded — they're per-device config
    # managed by install.sh (initial seed) and dev/switch-device.sh
    # (debug override). Letting `adb push --sync` carry them would stomp
    # on whatever the current debug session set the device to.
    write_version
    STAGE=$(mktemp -d)
    cp -R "${HERE}/qml/." "$STAGE/"
    find "$STAGE" -name '*.json' -delete
    adb -s "$DEV" push --sync "${STAGE}/." "${REMOTE_ROOT}/" >/dev/null
    rm -rf "$STAGE"
    echo "[deploy] qml/fonts/version synced; reload happens within ~1.5s"
}

case "${1:-install}" in
    install)   cmd_install ;;
    push-qml)  cmd_push_qml ;;
    undo)      cmd_undo ;;
    logs)      cmd_logs ;;
    status)    cmd_status ;;
    *) echo "usage: $0 {install|push-qml|undo|logs|status}"; exit 2 ;;
esac
