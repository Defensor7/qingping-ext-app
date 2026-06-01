// Bump revision + save to trigger hot-reload via Cameras.qml.
import QtQuick 2.9
import QtQuick.Layouts 1.3
import QtQuick.Controls 2.0
import Qing.Controls 1.0
import "."   // for MdiIcon (one dir up via ../, but we're already in Plugins/)

Item {
    id: impl
    anchors.fill: parent
    readonly property int revision: 5

    // Set by Cameras.qml shell.  active=false → shim SIGSTOPs gst-launch.
    property bool active: false
    // When loaded as a PathView delegate we leave 72 px for the device's
    // HeaderBar.  When pushed onto the stack we use the whole screen.
    property bool inPathView: false
    readonly property int topGap: impl.inPathView ? 72 : 0
    function restore() {}

    // --- config (cameras.json, hot-reloaded) -----------------------------
    property var cameras: []
    property string camerasSig: ""

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
                console.log("[qpext-cam] cameras.json: " + impl.cameras.length + " cameras")
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
    property var _lastCpu: null     // {total, idle}
    function pollCpu() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "file:///proc/stat")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            var t = xhr.responseText
            if (!t) return
            // First line: "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
            var nl = t.indexOf("\n")
            var first = (nl >= 0) ? t.substring(0, nl) : t
            var parts = first.split(/\s+/).slice(1)
            if (parts.length < 5) return
            var total = 0
            for (var i = 0; i < parts.length; ++i) total += parseInt(parts[i]) || 0
            var idle = (parseInt(parts[3]) || 0) + (parseInt(parts[4]) || 0)  // idle + iowait
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

        // No cameras configured.
        Item {
            anchors.fill: parent
            visible: impl.cameras.length === 0
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 12
                QText {
                    text: "No cameras"
                    color: "white"
                    font.pixelSize: 28
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }
                QText {
                    text: "edit /data/qpext/cameras.json"
                    color: "#88aacc"
                    font.pixelSize: 16
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }

        // One or many cameras.
        SwipeView {
            id: view
            anchors.fill: parent
            visible: impl.cameras.length > 0
            currentIndex: 0
            Repeater {
                model: impl.cameras
                delegate: Item {
                    width: view.width
                    height: view.height
                    property var cam: modelData

                    Rectangle { anchors.fill: parent; color: "black" }

                    // Single Image cycled by Timer. The shim atomically rewrites
                    // /tmp/qpext/cam/<name>.jpg, but Qt's Image keeps the prior
                    // pixmap until the new source has fully loaded — so we
                    // briefly clear the source to force a re-read each tick.
                    property real fps: cam && cam.fps > 0 ? cam.fps : 5

                    Image {
                        id: img
                        anchors.fill: parent
                        fillMode: Image.PreserveAspectFit
                        cache: false
                        asynchronous: false
                    }

                    Timer {
                        interval: Math.max(60, Math.round(1000 / fps))
                        running: !!cam && impl.active
                        repeat: true
                        triggeredOnStart: true
                        onTriggered: {
                            img.source = ""
                            img.source = "file:///tmp/qpext/cam/" + cam.name + ".jpg?" + Date.now()
                        }
                    }

                    // Footer with name.
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
                            text: cam ? (cam.label || cam.name || "camera") : ""
                            color: "white"
                            font.pixelSize: 18
                        }
                        QText {
                            anchors {
                                right: parent.right; verticalCenter: parent.verticalCenter
                                rightMargin: 14
                            }
                            text: (cam ? cam.fps + " fps" : "") +
                                  "  ·  cpu " + impl.cpuPct + "%" +
                                  "  ·  " + impl.socTempC.toFixed(0) + "°C"
                            color: "#88aacc"
                            font.pixelSize: 14
                        }
                    }
                }
            }
        }

        PageIndicator {
            anchors {
                bottom: parent.bottom; horizontalCenter: parent.horizontalCenter
                bottomMargin: 50
            }
            count: impl.cameras.length
            currentIndex: view.currentIndex
            visible: impl.cameras.length > 1
        }
    }

    Component.onCompleted: console.log("[qpext-cam] CamerasImpl rev=" + impl.revision + " loaded")
}
