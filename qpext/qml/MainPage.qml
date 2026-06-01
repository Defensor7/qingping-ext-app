// Mirror of qrc:/Main/MainPage.qml with extra user-configurable tabs appended
// to the page model. Each user tab is built from /data/qpext/widgets.json's
// `tabs[]` array (see ha_integration/qpext_airmonitor/tabs.py for the schema).
//
// A widgets-tab tab carries a slice of widget cards, a camera-tab tab carries
// a single RTSP feed. Both are addressed by id in MQTT switch_tab payloads
// (PathView entry name = "qpext_<tab.id>"). The header on a user tab shows
// the tab's display name in place of the stock "Home Assistant" string.
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
                        qpextTab: false
                        tabId: ""
                        tabName: ""
                        tabType: ""
                    }
                    ListElement {
                        name: "settingView"
                        source: "qrc:/Main/Views/Setting/SettingView.qml"
                        qpextTab: false
                        tabId: ""
                        tabName: ""
                        tabType: ""
                    }
                    ListElement {
                        name: "appView"
                        source: "qrc:/Main/Views/App/AppView.qml"
                        qpextTab: false
                        tabId: ""
                        tabName: ""
                        tabType: ""
                    }
                    // User-defined tabs (widgets | camera) are appended by
                    // pollTabs() below from /data/qpext/widgets.json.
                    Component.onCompleted: {
                        if (!global.isShowAppView || (global.product === Global.PROD_HAIER)) {
                            removePage("appView");
                        }
                        if (datetimeManager.isInitialized) {
                            insertPage("airDatasView", "summaryView",
                                       "qrc:/Main/Views/Summary/SummaryView.qml");
                        }
                        // qpext: don't restore the saved index — start at 0.
                        view.currentIndex = 0
                        global.currentMainPageIndex = 0
                    }
                }
                delegate: Component {
                    id: delegate
                    Loader {
                        id: loader
                        property int modelIndex: index
                        // The visually-centered delegate is (currentIndex + 1)
                        // mod count — PathView off-by-one (NOTES.md).
                        property bool isVisualCurrent: view
                            ? ((view.currentIndex + 1) % view.count === modelIndex)
                            : false
                        width: view.width
                        height: view.height
                        source: model.source
                        // Per-tab properties forwarded to the user-tab impl.
                        // For static qrc:// tabs the bindings stay inert
                        // because the target items don't declare these props
                        // (Binding's `when` is gated on objectName below).
                        Binding {
                            target: loader.item
                            property: "tabId"
                            value: model.tabId
                            when: loader.item !== null &&
                                  (loader.item.objectName === "qpextExtensionShell" ||
                                   loader.item.objectName === "qpextCamerasShell")
                        }
                        Binding {
                            target: loader.item
                            property: "tabName"
                            value: model.tabName
                            when: loader.item !== null &&
                                  (loader.item.objectName === "qpextExtensionShell" ||
                                   loader.item.objectName === "qpextCamerasShell")
                        }
                        Binding {
                            target: loader.item
                            property: "isVisualCurrent"
                            value: loader.isVisualCurrent
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
                model.insert(j + 1, { "name": name, "source": source,
                                      "qpextTab": false, "tabId": "",
                                      "tabName": "", "tabType": "" })
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
        // PathView startX=-w/2 puts the model's currentIndex off-screen left;
        // the VISUAL center is (currentIndex+1) mod n. To land visually on j
        // we set currentIndex = (j - 1 + n) mod n.
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

    // --- widgets.json polling: keep PathView model in sync with tabs[] ----
    property string tabsSig: ""

    // The single source of truth for what a user-tab looks like in the model.
    // Stock qrc tabs use qpextTab=false; user tabs use qpextTab=true plus
    // tabId / tabName / tabType so the delegate Bindings can forward them
    // to ExtensionImpl / CamerasImpl.
    function _buildUserEntry(t) {
        var isCamera = (t.type === "camera")
        return {
            "name":     "qpext_" + t.id,
            "source":   isCamera
                ? "file:///data/qpext/Plugins/Cameras.qml"
                : "file:///data/qpext/Plugins/Extension.qml",
            "qpextTab": true,
            "tabId":    t.id,
            "tabName":  t.name || t.id,
            "tabType":  t.type || "widgets"
        }
    }

    function _userTabIndices() {
        var out = []
        for (var i = 0; i < model.count; ++i)
            if (model.get(i).qpextTab) out.push(i)
        return out
    }

    function syncTabs(tabs) {
        // Reuse existing user-tab slots in place where possible so the
        // PathView's currentIndex doesn't jump every refresh; only add /
        // remove the tail when the count differs.
        var existing = _userTabIndices()
        var visualBefore = (view.currentIndex + 1) % model.count

        // Pass 1: update in place / append new.
        for (var i = 0; i < tabs.length; ++i) {
            var entry = _buildUserEntry(tabs[i])
            if (i < existing.length) {
                var idx = existing[i]
                var cur = model.get(idx)
                if (cur.name !== entry.name ||
                    cur.source !== entry.source ||
                    cur.tabName !== entry.tabName ||
                    cur.tabType !== entry.tabType ||
                    cur.tabId !== entry.tabId) {
                    model.set(idx, entry)
                }
            } else {
                model.append(entry)
            }
        }
        // Pass 2: drop any extras from the tail.
        if (tabs.length < existing.length) {
            for (var k = existing.length - 1; k >= tabs.length; --k) {
                var deadIdx = existing[k]
                // If we're about to remove the visually-current item, snap
                // first so PathView doesn't end up anchored on a missing one.
                if (visualBefore === deadIdx) view.currentIndex = 0
                model.remove(deadIdx)
            }
        }
        console.log("[qpext] syncTabs: " + tabs.length + " user tab(s), model.count=" + model.count)
    }

    function pollTabs() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "file:///data/qpext/widgets.json")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            var t = xhr.responseText || ""
            var sig = t.length + ":" + t.substring(0, 48)
            if (sig === control.tabsSig) return
            control.tabsSig = sig
            var tabs = []
            try {
                var parsed = JSON.parse(t)
                tabs = (parsed && parsed.tabs) || []
            } catch (e) {}
            syncTabs(tabs)
        }
        xhr.send()
    }
    Timer { interval: 1500; running: true; repeat: true; triggeredOnStart: true
            onTriggered: pollTabs() }

    // --- tab_event poll: shim writes /tmp/qpext/tab_event on HA cmd. ------
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
                insertPage("airDatasView", "summaryView",
                           "qrc:/Main/Views/Summary/SummaryView.qml");
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
