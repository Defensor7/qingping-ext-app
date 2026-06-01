// Bump revision + save to trigger hot-reload via Cameras.qml.
//
// Renders a SINGLE camera tile — multi-camera was deprecated when each
// camera became its own user-defined tab. The impl picks its camera out
// of /data/qpext/cameras.json by matching the entry's `tab_id` against
// our `tabId` property (forwarded by Cameras.qml's shell).
import QtQuick 2.9
import QtQuick.Layouts 1.3
import Qing.Controls 1.0
import "."   // for MdiIcon (one dir up via ../, but we're already in Plugins/)

Item {
    id: impl
    anchors.fill: parent
    readonly property int revision: 6

    // Set by Cameras.qml shell.  active=false → shim SIGSTOPs gst-launch.
    property bool active: false
    // Loaded inside a PathView delegate (currently always true; we keep
    // the flag for the legacy push-onto-stack code path).
    property bool inPathView: false
    readonly property int topGap: impl.inPathView ? 72 : 0
    // Per-tab identity, forwarded by Cameras.qml from MainPage's delegate.
    property string tabId: ""
    property string tabName: ""
    function restore() {}

    // --- config (cameras.json, hot-reloaded) -----------------------------
    // We hold the FULL cameras list and pick our own entry reactively from
    // (cameras, tabId). Keeps the impl trivially robust to tabId arriving
    // after the first cameras.json read.
    property var cameras: []
    property string camerasSig: ""

    function _camForTab() {
        if (!impl.tabId) return null
        for (var i = 0; i < impl.cameras.length; ++i) {
            var c = impl.cameras[i]
            if (c && c.tab_id === impl.tabId) return c
        }
        return null
    }
    property var cam: _camForTab()
    onTabIdChanged: impl.cam = _camForTab()
    onCamerasChanged: impl.cam = _camForTab()

    function loadCameras() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "file:///data/qpext/cameras.json")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status !== 0 && xhr.status !== 200) return
            var t = xhr.responseText
            if (!t) return
            var sig = t.length + ":" + t.substring(0, 32)
            if (sig === impl.camerasSig) return
            impl.camerasSig = sig
            try {
                var parsed = JSON.parse(t)
                impl.cameras = parsed.cameras || []
                console.log("[qpext-cam] cameras.json: " + impl.cameras.length +
                            " cameras (tabId=" + impl.tabId + " match=" +
                            (impl._camForTab() ? "yes" : "no") + ")")
            } catch (e) {
                console.log("[qpext-cam] parse error:", e)
            }
        }
        xhr.send()
    }
    Timer { interval: 1500; running: true; repeat: true; triggeredOnStart: true
            onTriggered: loadCameras() }

    // --- CPU load: poll /proc/stat once per second, compute %busy ----------
    property real cpuPct: 0
    property var _lastCpu: null
    function pollCpu() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "file:///proc/stat")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            var t = xhr.responseText
            if (!t) return
            var nl = t.indexOf("\n")
            var first = (nl >= 0) ? t.substring(0, nl) : t
            var parts = first.split(/\s+/).slice(1)
            if (parts.length < 5) return
            var total = 0
            for (var i = 0; i < parts.length; ++i) total += parseInt(parts[i]) || 0
            var idle = (parseInt(parts[3]) || 0) + (parseInt(parts[4]) || 0)
            if (impl._lastCpu) {
                var dt = total - impl._lastCpu.total
                var di = idle  - impl._lastCpu.idle
                if (dt > 0) impl.cpuPct = Math.max(0, Math.min(100,
                    Math.round((dt - di) * 100 / dt)))
            }
            impl._lastCpu = { total: total, idle: idle }
        }
        xhr.send()
    }
    Timer { interval: 1000; running: true; repeat: true; triggeredOnStart: true
            onTriggered: pollCpu() }

    // --- SoC temperature ------------------------------------------------
    property real socTempC: 0
    function pollTemp() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "file:///sys/class/thermal/thermal_zone0/temp")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            var raw = parseInt(xhr.responseText)
            if (!isNaN(raw)) impl.socTempC = raw / 1000
        }
        xhr.send()
    }
    Timer { interval: 2000; running: true; repeat: true; triggeredOnStart: true
            onTriggered: pollTemp() }

    // --- heartbeat to shim while tab is visible -------------------------
    // The shim gates gst-launch decoding on this heartbeat (cam_thread
    // SIGSTOPs pipelines without a recent heartbeat). Each per-tab impl
    // sends its own beats; the shim treats any heartbeat as "screen is
    // showing a camera" — fine, because we only want decoding for
    // whichever camera tab the user is currently looking at, and only one
    // tab can have active=true at a time.
    Timer {
        interval: 1500
        running: impl.active
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            var xhr = new XMLHttpRequest()
            xhr.open("GET", "http://127.0.0.1:8765/heartbeat")
            xhr.send()
        }
    }

    // --- visuals ---------------------------------------------------------
    Item {
        anchors {
            left: parent.left; right: parent.right; bottom: parent.bottom
            top: parent.top; topMargin: impl.topGap
        }

        // No camera matched our tab id (config is loading, or cameras.json
        // is empty / mid-update). Show the tab name so the user has some
        // visual confirmation of where they are.
        Item {
            anchors.fill: parent
            visible: !impl.cam
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 12
                QText {
                    text: impl.tabName || "Camera"
                    color: "white"
                    font.pixelSize: 28
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }
                QText {
                    text: "no camera configured for this tab"
                    color: "#88aacc"
                    font.pixelSize: 16
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }

        // The (one) camera.
        Item {
            anchors.fill: parent
            visible: !!impl.cam

            property real fps: (impl.cam && impl.cam.fps > 0) ? impl.cam.fps : 5

            Rectangle { anchors.fill: parent; color: "black" }

            // Single Image cycled by Timer. The shim atomically rewrites
            // /tmp/qpext/cam/<name>.jpg, but Qt's Image keeps the prior
            // pixmap until the new source has fully loaded — so we briefly
            // clear the source to force a re-read each tick.
            Image {
                id: img
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                cache: false
                asynchronous: false
            }

            Timer {
                interval: Math.max(60, Math.round(1000 / parent.fps))
                running: !!impl.cam && impl.active
                repeat: true
                triggeredOnStart: true
                onTriggered: {
                    img.source = ""
                    img.source = "file:///tmp/qpext/cam/" + impl.cam.name + ".jpg?" + Date.now()
                }
            }

            // Footer with name + stats.
            Rectangle {
                anchors {
                    left: parent.left; right: parent.right; bottom: parent.bottom
                }
                height: 38
                color: "#80000000"
                QText {
                    anchors {
                        left: parent.left; verticalCenter: parent.verticalCenter
                        leftMargin: 14
                    }
                    text: impl.cam ? (impl.cam.label || impl.tabName || impl.cam.name || "camera") : ""
                    color: "white"
                    font.pixelSize: 18
                }
                QText {
                    anchors {
                        right: parent.right; verticalCenter: parent.verticalCenter
                        rightMargin: 14
                    }
                    text: (impl.cam ? impl.cam.fps + " fps" : "") +
                          "  ·  cpu " + impl.cpuPct + "%" +
                          "  ·  " + impl.socTempC.toFixed(0) + "°C"
                    color: "#88aacc"
                    font.pixelSize: 14
                }
            }
        }
    }

    Component.onCompleted: console.log("[qpext-cam] CamerasImpl rev=" + impl.revision +
                                       " loaded (tabId=" + impl.tabId + ")")
}
