// Thin wrapper that polls CamerasImpl.qml on disk and reloads on change.
// Same hot-reload pattern as Hello.qml.
import QtQuick 2.9
import Qing.Controls 1.0

Item {
    id: shell
    objectName: "qpextCamerasShell"   // marker used by MainPage.qml delegate Binding
    implicitWidth: parent ? parent.width : 720
    implicitHeight: parent ? parent.height : 720
    function restore() {
        if (loader.item && typeof loader.item.restore === "function")
            loader.item.restore()
    }
    // Loaded via stackView.push so the user can dismiss back to the dashboard.
    signal exited()

    property int version: 0
    property string lastSig: ""
    // Detect whether we are inside a PathView (delegate role) or pushed onto
    // a StackView (cmd / tab_event). Outside PathView -> always active.
    property bool pathViewMode: parent && parent.PathView &&
                                parent.PathView.view !== null
    // Set by the PathView delegate's Binding (MainPage.qml). Tracks whether
    // THIS delegate is the one visually centered. PathView.isCurrentItem can't
    // be used because the visual current is (PathView.currentIndex + 1) mod n
    // due to the off-by-one in the PathLine geometry — see NOTES.md.
    property bool isVisualCurrent: false
    property bool active: !pathViewMode || isVisualCurrent

    Loader {
        id: loader
        anchors.fill: parent
        source: "file:///data/qpext/Plugins/CamerasImpl.qml?v=" + shell.version
        onStatusChanged: if (status === Loader.Error)
            console.log("[qpext] CamerasImpl.qml load error")
        Binding { target: loader.item; property: "active";     value: shell.active;       when: loader.item }
        Binding { target: loader.item; property: "inPathView"; value: shell.pathViewMode; when: loader.item }
    }


    Timer {
        interval: 1500
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            var xhr = new XMLHttpRequest()
            xhr.open("GET", "file:///data/qpext/Plugins/CamerasImpl.qml")
            xhr.onreadystatechange = function() {
                if (xhr.readyState !== XMLHttpRequest.DONE) return
                if (xhr.status !== 0 && xhr.status !== 200) return
                var t = xhr.responseText
                if (!t) return
                var sig = t.length + ":" + t.substring(0, 64) + ":" + t.substring(t.length - 64)
                if (sig !== shell.lastSig) {
                    var first = (shell.lastSig === "")
                    shell.lastSig = sig
                    if (!first) {
                        console.log("[qpext] CamerasImpl changed, reloading")
                        shell.version += 1
                    }
                }
            }
            xhr.send()
        }
    }
}
