// Thin wrapper that polls HelloImpl.qml on disk and reloads on change.
// Edit HelloImpl.qml; `deploy.sh push-qml` pushes it; this picks it up in ~1.5s
// without restarting the app. The query string is what bypasses Qt's component
// cache so the new file is actually re-read.
import QtQuick 2.9

Item {
    id: shell
    implicitWidth: parent ? parent.width : 720
    implicitHeight: parent ? parent.height : 720

    // PathView delegate in MainPage.qml calls restore() on flickEnded.
    function restore() {
        if (loader.item && typeof loader.item.restore === "function") {
            loader.item.restore()
        }
    }

    property int version: 0
    property string lastSig: ""

    Loader {
        id: loader
        anchors.fill: parent
        source: "file:///data/qpext/Plugins/HelloImpl.qml?v=" + shell.version
        asynchronous: false
        onStatusChanged: {
            if (status === Loader.Error) {
                console.log("[qpext] HelloImpl.qml load error")
            } else if (status === Loader.Ready) {
                console.log("[qpext] HelloImpl.qml ready (v=" + shell.version + ")")
            }
        }
    }

    Timer {
        interval: 1500
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            var xhr = new XMLHttpRequest()
            xhr.open("GET", "file:///data/qpext/Plugins/HelloImpl.qml")
            xhr.onreadystatechange = function() {
                if (xhr.readyState !== XMLHttpRequest.DONE) return
                if (xhr.status !== 0 && xhr.status !== 200) return  // file:// often gives status=0
                var t = xhr.responseText
                if (!t) return
                // Cheap signature: length + first 64 + last 64 chars.
                var sig = t.length + ":" + t.substring(0, 64) + ":" + t.substring(t.length - 64)
                if (sig !== shell.lastSig) {
                    var first = (shell.lastSig === "")
                    shell.lastSig = sig
                    if (!first) {
                        console.log("[qpext] HelloImpl.qml changed (" + t.length + " bytes), reloading")
                        shell.version += 1
                    }
                }
            }
            xhr.send()
        }
    }
}
