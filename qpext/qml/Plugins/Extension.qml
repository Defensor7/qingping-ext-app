// Thin wrapper that polls ExtensionImpl.qml on disk and reloads on change.
// Edit ExtensionImpl.qml; `deploy.sh push-qml` pushes it; this picks it up in
// ~1.5 s without restarting the app. The query string is what bypasses Qt's
// component cache so the new file is actually re-read.
//
// Per-tab data plumbing: MainPage.qml's PathView delegate binds tabId/tabName
// onto THIS shell (via objectName == "qpextExtensionShell"). The shell then
// forwards them onto the inner ExtensionImpl through a Binding so the impl
// can filter widgets.json's tabs[] to its own slice and render the right
// header text.
import QtQuick 2.9

Item {
    id: shell
    objectName: "qpextExtensionShell"
    implicitWidth: parent ? parent.width : 720
    implicitHeight: parent ? parent.height : 720

    // Forwarded by MainPage.qml's delegate Bindings.
    property string tabId: ""
    property string tabName: ""

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
        source: "file:///data/qpext/Plugins/ExtensionImpl.qml?v=" + shell.version
        asynchronous: false
        onStatusChanged: {
            if (status === Loader.Error) {
                console.log("[qpext] ExtensionImpl.qml load error")
            } else if (status === Loader.Ready) {
                console.log("[qpext] ExtensionImpl.qml ready (v=" + shell.version +
                            ", tabId=" + shell.tabId + ")")
            }
        }
        Binding { target: loader.item; property: "tabId";   value: shell.tabId;   when: loader.item }
        Binding { target: loader.item; property: "tabName"; value: shell.tabName; when: loader.item }
    }

    Timer {
        interval: 1500
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            var xhr = new XMLHttpRequest()
            xhr.open("GET", "file:///data/qpext/Plugins/ExtensionImpl.qml")
            xhr.onreadystatechange = function() {
                if (xhr.readyState !== XMLHttpRequest.DONE) return
                if (xhr.status !== 0 && xhr.status !== 200) return  // file:// often gives status=0
                var t = xhr.responseText
                if (!t) return
                var sig = t.length + ":" + t.substring(0, 64) + ":" + t.substring(t.length - 64)
                if (sig !== shell.lastSig) {
                    var first = (shell.lastSig === "")
                    shell.lastSig = sig
                    if (!first) {
                        console.log("[qpext] ExtensionImpl.qml changed (" + t.length + " bytes), reloading")
                        shell.version += 1
                    }
                }
            }
            xhr.send()
        }
    }
}
