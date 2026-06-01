// Mirror of qrc:/Main/MainPage.qml with one extra ListElement appended to
// the page model. Header/Notification dir-imports rewritten to qrc:/.
import QtQuick 2.0
import QtQuick.Controls 2.0
import Qing.Styles 1.0
import Qing.Controls 1.0
import Qing.Frames 1.0
import Qing.Snow 1.0
import "Header"
import "Notification"

Item {
    id: control
    property bool isSummary: false
    signal goToSettingPage()
    implicitWidth: Styles.windowWidth
    implicitHeight: Styles.windowHeight

    Rectangle {
        id: background
        anchors.fill: parent
        color: Styles.windowBackgroundColor
    }

    // qpext: poll /tmp/qpext/grab.req → if mtime changed, grab current window
    // into a PNG so I can see what's actually on screen.
    property int qpextGrabSeen: 0
    Timer {
        interval: 500
        running: true; repeat: true; triggeredOnStart: true
        onTriggered: {
            var xhr = new XMLHttpRequest()
            xhr.open("GET", "file:///tmp/qpext/grab.req")
            xhr.onreadystatechange = function() {
                if (xhr.readyState !== XMLHttpRequest.DONE) return
                var t = parseInt(xhr.responseText) || 0
                if (!t || t === qpextGrabSeen) return
                qpextGrabSeen = t
                control.grabToImage(function(res) {
                    res.saveToFile("/tmp/qpext/grab.png")
                    console.log("[qpext] saved screenshot, ci=" +
                                (view ? view.currentIndex : "?") +
                                " off=" + (view ? view.offset.toFixed(2) : "?"))
                })
            }
            xhr.send()
        }
    }
    QStackView {
        id: stackView
        anchors.fill: parent
        initialItem: Item {
            NotificationBar {
                id: hydraMode
                y:20
                z:11
                text: "数据采集模式..."
                visible: !!isHydra
                enabled: false
                opacity: 0.5
            }

            NotificationBar {
                y: 20
                z: 10
                text: qsTr("Firmware update available")
                visible: updateController.isNeedNotification
                onClosed: { updateController.ignoreVersion() }
                onConfirmed: openPage("qrc:/Modules/SystemUpdate/SystemUpdatePage.qml")
            }

            HeaderBar {
                id: headerBar
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 72
                timeVisible: !control.isSummary && datetimeManager.isInitialized
            }
            PageIndicator {
                id: indicator
                count: view.count
                currentIndex: (view.currentIndex + 1) % model.count
                anchors.top: view.top
                anchors.topMargin: 30
                anchors.horizontalCenter: parent.horizontalCenter
                delegate: Rectangle {
                    implicitWidth: 10
                    implicitHeight: 10
                    radius: width / 2
                    color: "white"
                    opacity: index === indicator.currentIndex ? 0.95 : pressed ? 0.8 : 0.3
                    Behavior on opacity { NumberAnimation { duration: 150 } }
                }
            }
            PathView {
                id: view
                anchors.fill: parent
                snapMode: PathView.SnapOneItem
                highlightRangeMode: PathView.StrictlyEnforceRange
                focus: true
                currentIndex: (model.count - 1)
                model: ListModel {
                    id: model
                    ListElement {
                        name: "airDatasView"
                        source: "qrc:/Main/Views/AirDatas/AirDatasView.qml"
                    }
                    ListElement {
                        name: "settingView"
                        source: "qrc:/Main/Views/Setting/SettingView.qml"
                    }
                    ListElement {
                        name: "appView"
                        source: "qrc:/Main/Views/App/AppView.qml"
                    }
                    // <<< qpext custom tabs >>>
                    ListElement {
                        name: "qpextView"
                        source: "file:///data/qpext/Plugins/Hello.qml"
                    }
                    ListElement {
                        name: "qpextCamerasView"
                        source: "file:///data/qpext/Plugins/Cameras.qml"
                    }
                    Component.onCompleted: {
                        if (!global.isShowAppView || (global.product === Global.PROD_HAIER)) {
                            removePage("appView");
                        }
                        if (datetimeManager.isInitialized) {
                            insertPage("airDatasView", "summaryView", "qrc:/Main/Views/Summary/SummaryView.qml");
                        }
                        // qpext: don't restore the saved index — it desyncs
                        // PathView's currentIndex from its visual offset and
                        // breaks programmatic switching from MQTT/tab_event.
                        // Always start on the first tab.
                        view.currentIndex = 0
                        global.currentMainPageIndex = 0
                    }
                }
                delegate: Component {
                    id: delegate
                    Loader {
                        id: loader
                        property int modelIndex: index
                        // The visually-centered delegate is the one at
                        // (currentIndex + 1) mod count, not currentIndex itself —
                        // see PathView off-by-one note in NOTES.md. Cameras.qml's
                        // `active` (which gates the frame Timer + heartbeat to
                        // the shim) is driven from here.
                        property bool isVisualCurrent: view
                            ? ((view.currentIndex + 1) % view.count === modelIndex)
                            : false
                        width: view.width
                        height: view.height
                        source: model.source
                        Binding {
                            target: loader.item
                            property: "isVisualCurrent"
                            value: loader.isVisualCurrent
                            // Only delegates that opt in via objectName accept
                            // this — keeps QML from warning on qrc-shipped views
                            // that don't have the property.
                            when: loader.item !== null &&
                                  loader.item.objectName === "qpextCamerasShell"
                        }
                        Connections {
                            target: view
                            function onFlickEnded() {
                                if (loader.item && loader.item.restore) loader.item.restore()
                            }
                        }
                    }
                }
                path: Path {
                    startX: -view.width / 2
                    startY: view.height / 2
                    PathLine {
                        relativeX: view.width * view.model.count
                        relativeY: 0
                    }
                }
                onCurrentIndexChanged: {
                    saveCurrentPageIndexTimer.restart()
                    var it = model.get(currentIndex)
                    var nm = it ? it.name : "?"
                    console.log("[qpext] ci-> " + currentIndex + " (" + nm + ") off=" + view.offset.toFixed(2))
                    if (nm === "airDatasView") {
                        control.isSummary = true;
                    } else {
                        control.isSummary = false;
                    }
                }
                onOffsetChanged: {
                    // Throttle: log only when offset is near an integer (i.e. snap completed).
                    var nearInt = Math.abs(offset - Math.round(offset)) < 0.05
                    if (nearInt && !flicking && !moving) {
                        var it = model.get(currentIndex)
                        console.log("[qpext] off-> " + offset.toFixed(2) + " ci=" + currentIndex +
                                    " (" + (it ? it.name : "?") + ")")
                    }
                }
            }
        }
    }
    Component {
        id: childPage
        Loader {
            id: loader
            Connections {
                target: loader.item
                onExited: { if (stackView.depth > 1) { stackView.pop(); } }
            }
        }
    }
    function openPage(url) {
        if (url !== "") { stackView.push(childPage, {"source": url }); }
    }
    function insertPage(target, name, source) {
        for (var i = 0; i < model.count; ++i) {
            if (model.get(i).name === name) { return; }
        }
        for (var j = 0; j < model.count; ++j) {
            if (model.get(j).name === target) {
                model.insert(j + 1, { "name": name, "source": source })
                break;
            }
        }
    }
    function removePage(name) {
        for (var i = 0; i < model.count; ++i) {
            if (model.get(i).name === name) {
                model.remove(i)
                break;
            }
        }
    }
    // Debug helper: log all model entries with their indices, plus current
    // view state. Called on load and on every tab_event so we can diagnose
    // ci/offset/visual desync.
    function logModelState(label) {
        if (!view || !view.model) { console.log("[qpext] " + label + " no view/model"); return }
        var m = view.model
        var s = ""
        for (var i = 0; i < m.count; ++i) {
            var it = m.get(i)
            s += i + ":" + (it ? it.name : "?") + " "
        }
        console.log("[qpext] " + label + " ci=" + view.currentIndex +
                    " off=" + view.offset.toFixed(2) +
                    " count=" + m.count +
                    " model=[" + s + "]")
    }

    function doSwitchByName(name) {
        var m = view ? view.model : null
        if (!m) return
        var n = m.count
        var j = -1
        for (var i = 0; i < n; ++i)
            if (m.get(i).name === name) { j = i; break }
        if (j < 0) {
            console.log("[qpext] tab '" + name + "' not in model (count=" + n + ")")
            return
        }
        if (view.flicking || view.moving) view.cancelFlick()
        // The PathView path is `PathLine startX=-w/2 relativeX=w*count` — the
        // current item snaps at startX which is OFF-SCREEN LEFT, so the item
        // VISUALLY in the center is the next one along the path. Visual index
        // = (currentIndex + 1) mod n. The page indicator above already
        // compensates with the same +1 offset. To land visually on tab j we
        // therefore set currentIndex = (j - 1 + n) mod n.
        var targetCI = (j - 1 + n) % n
        var cur = view.currentIndex
        logModelState("before switch -> " + name + " (j=" + j + " → targetCI=" + targetCI + ")")
        if (cur === targetCI) {
            console.log("[qpext] tab -> " + name + " already current (ci=" + cur +
                        " visual=" + ((cur + 1) % n) + " off=" + view.offset.toFixed(2) + ")")
            return
        }
        view.currentIndex = targetCI
        console.log("[qpext] tab -> " + name + " setCI(" + targetCI +
                    ") for visual j=" + j +
                    " (was ci=" + cur + " off=" + view.offset.toFixed(2) + ")")
    }
    function switchToName(name) {
        while (stackView.depth > 1) stackView.pop()
        doSwitchByName(name)
        return true
    }
    // <<< qpext: shim writes /tmp/qpext/tab_event when an HA trigger fires.
    // We poll the file every 250ms and switch to the named page on a new ts.
    property int qpextLastTs: 0
    Timer {
        interval: 250
        running: true
        repeat: true
        onTriggered: {
            var xhr = new XMLHttpRequest()
            xhr.open("GET", "file:///tmp/qpext/tab_event")
            xhr.onreadystatechange = function() {
                if (xhr.readyState !== XMLHttpRequest.DONE) return
                if (xhr.status !== 0 && xhr.status !== 200) return
                var t = xhr.responseText
                if (!t) return
                try {
                    var ev = JSON.parse(t)
                    if (ev && ev.ts && ev.ts !== qpextLastTs) {
                        qpextLastTs = ev.ts
                        console.log("[qpext] tab_event:", ev.switch_to, "ts=" + ev.ts)
                        if (ev.switch_to) switchToName(ev.switch_to)
                    }
                } catch (e) {}
            }
            xhr.send()
        }
    }
    Connections {
        target: datetimeManager
        function onIsInitializedChanged() {
            if (datetimeManager.isInitialized) {
                insertPage("airDatasView", "summaryView", "qrc:/Main/Views/Summary/SummaryView.qml");
            }
        }
    }
    Connections {
        target: global
        function onIsShowAppViewChanged() {
            if (!global.isShowAppView) { removePage("appView") }
        }
    }

    Timer {
        id: saveCurrentPageIndexTimer
        interval: 1000
        running: false
        repeat: false
        onTriggered:  { global.currentMainPageIndex = view.currentIndex }
    }

    Component.onCompleted: {
        console.log("[qpext] MainPage.qml loaded from /data/qpext, pages:", model.count)
        logModelState("on load")
    }
}
