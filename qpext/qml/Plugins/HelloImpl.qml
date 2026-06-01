// Bumping revision + saving triggers hot-reload through Hello.qml.
// Renders widgets.json as a grid; each widget type lives in widgets/<Type>.qml.
import QtQuick 2.9
import QtQuick.Layouts 1.3
import Qing.Styles 1.0
import Qing.Controls 1.0

Item {
    id: impl
    anchors.fill: parent
    readonly property int revision: 16
    readonly property int topGap: 72
    readonly property int widgetHeight: 180
    // Loaded once on creation from /data/qpext/version.txt, which deploy.sh
    // refreshes from `version.sh` (git describe) on every push.
    property string version: ""

    function restore() {}

    // --- MDI font --------------------------------------------------------
    FontLoader {
        id: mdiFontLoader
        source: "file:///data/qpext/fonts/mdi.ttf"
        onStatusChanged: if (status === FontLoader.Error)
            console.log("[qpext] mdi font load error")
    }

    // --- HA credentials (user-managed, /data/qpext/ha.json) ---------------
    // Used only for outgoing REST service-calls. State pull happens on the
    // shim side over WebSocket; QML reads /data/qpext/state.json (below).
    property var haConfig: ({ base_url: "", token: "" })
    property string haConfigSig: ""

    function loadHaConfig() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "file:///data/qpext/ha.json")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status !== 0 && xhr.status !== 200) return
            var t = xhr.responseText
            if (!t) return
            var sig = t.length + ":" + t.substring(0, 32)
            if (sig === impl.haConfigSig) return
            impl.haConfigSig = sig
            try { impl.haConfig = JSON.parse(t) }
            catch (e) { console.log("[qpext] ha.json parse error:", e) }
        }
        xhr.send()
    }

    // --- dashboard composition (HA-integration-managed, /data/qpext/widgets.json)
    // Pure {widgets, events} object; written by qpext_mqtt.cpp on every
    // qpext/<mac>/dashboard/set retained message. No ha section any more.
    property var config: ({ widgets: [] })
    property string configSig: ""

    function loadConfig(silent) {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "file:///data/qpext/widgets.json")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status !== 0 && xhr.status !== 200) return
            var t = xhr.responseText
            if (!t) return
            var sig = t.length + ":" + t.substring(0, 32)
            if (sig === impl.configSig) return
            impl.configSig = sig
            try {
                impl.config = JSON.parse(t)
                console.log("[qpext] widgets.json: " + (impl.config.widgets || []).length + " widgets")
            } catch (e) {
                if (!silent) console.log("[qpext] widgets.json parse error:", e)
            }
        }
        xhr.send()
    }

    // --- HA state cache via /data/qpext/state.json (written by qpext.so WS) -
    property var haState: ({})
    property int haTick: 0
    property string lastStateSig: ""
    property int stateLoads: 0

    function entityState(eid) { return impl.haState[eid] }

    function pollState() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "file:///data/qpext/state.json")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status !== 0 && xhr.status !== 200) return
            var t = xhr.responseText
            if (!t || t.length < 2) return
            var sig = t.length + ":" + t.charCodeAt(0) + ":" + t.charCodeAt(t.length - 1) +
                      ":" + t.substring(0, 32)
            if (sig === impl.lastStateSig) return
            impl.lastStateSig = sig
            try {
                impl.haState = JSON.parse(t)
                impl.haTick++
                impl.stateLoads++
                if (impl.stateLoads === 1 || impl.stateLoads % 50 === 0) {
                    var n = 0; for (var k in impl.haState) n++
                    console.log("[qpext] state load #" + impl.stateLoads +
                                ": " + n + " entities, " + t.length + "B")
                }
            } catch (e) {
                console.log("[qpext] state.json parse error:", e)
            }
        }
        xhr.send()
    }

    function callService(domain, service, data) {
        if (!haConfig.base_url || !haConfig.token) return
        var xhr = new XMLHttpRequest()
        xhr.open("POST", haConfig.base_url + "/api/services/" + domain + "/" + service)
        xhr.setRequestHeader("Authorization", "Bearer " + haConfig.token)
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status !== 200)
                console.log("[qpext] call", domain + "/" + service, "->", xhr.status,
                            (xhr.responseText || "").substring(0, 120))
        }
        xhr.send(JSON.stringify(data || {}))
    }

    Timer { interval: 250; running: true; repeat: true; onTriggered: pollState() }
    Timer { interval: 1500; running: true; repeat: true; triggeredOnStart: true
            onTriggered: loadConfig(true) }
    // ha.json changes less often (only on install / dev/switch-device.sh),
    // a slow poll is enough — picks up token rotation without restart.
    Timer { interval: 5000; running: true; repeat: true; triggeredOnStart: true
            onTriggered: loadHaConfig() }

    // --- helpers for widgets --------------------------------------------
    // Resolve component path. snake_case → CamelCase, so "media_player" → "MediaPlayer.qml".
    function widgetSource(type) {
        if (!type) return "widgets/Sensor.qml"
        var parts = ("" + type).split("_")
        var name = ""
        for (var i = 0; i < parts.length; ++i) {
            if (!parts[i].length) continue
            name += parts[i].charAt(0).toUpperCase() + parts[i].substring(1)
        }
        return "widgets/" + name + ".qml"
    }

    // --- layout ----------------------------------------------------------
    ColumnLayout {
        anchors {
            left: parent.left; right: parent.right; bottom: parent.bottom
            top: parent.top; topMargin: impl.topGap + 8
            leftMargin: 18; rightMargin: 18; bottomMargin: 18
        }
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            QText {
                Layout.fillWidth: true
                text: "Home Assistant"
                color: "white"
                font.pixelSize: 28
                font.bold: true
            }
            QText {
                text: impl.version || "rev " + impl.revision
                color: "#5577aa"
                font.pixelSize: 14
            }
        }

        QText {
            Layout.fillWidth: true
            visible: !haConfig.base_url || !haConfig.token ||
                     haConfig.token.indexOf("PUT_") === 0
            text: "edit /data/qpext/ha.json: set base_url and token"
            wrapMode: Text.WrapAnywhere
            color: "#ffaa55"
            font.pixelSize: 13
        }

        QText {
            Layout.fillWidth: true
            visible: (config.widgets || []).length === 0 &&
                     haConfig.base_url && haConfig.token &&
                     haConfig.token.indexOf("PUT_") !== 0
            text: "no widgets configured — add them via the qpext_airmonitor integration in HA"
            wrapMode: Text.WrapAnywhere
            color: "#88aacc"
            font.pixelSize: 13
        }

        // Wrapper: the Flickable is inset on the right to make a dedicated
        // gutter for the scroll indicator. The indicator is a SIBLING of the
        // Flickable (otherwise it'd be a child of contentItem and scroll along).
        Item {
            id: dashboardArea
            Layout.fillWidth: true
            Layout.fillHeight: true

            readonly property int gutter: 10

            Flickable {
                id: dashboardScroll
                anchors.fill: parent
                anchors.rightMargin: dashboardArea.gutter
                clip: true
                contentWidth: width
                contentHeight: dashboardGrid.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.VerticalFlick

                GridLayout {
                    id: dashboardGrid
                    width: dashboardScroll.width
                    columns: 2
                    columnSpacing: 10
                    rowSpacing: 10

                    Repeater {
                        model: config.widgets || []
                        delegate: Loader {
                            Layout.fillWidth: true
                            Layout.preferredHeight: impl.widgetHeight
                            Layout.minimumHeight: impl.widgetHeight
                            asynchronous: false
                            property var widgetData: modelData
                            property var stateData: (impl.haTick,
                                modelData && modelData.entity ? impl.entityState(modelData.entity) : null)
                            source: impl.widgetSource(modelData ? modelData.type : "sensor")
                            onStatusChanged: if (status === Loader.Error)
                                console.log("[qpext] widget load error:", source)
                            Binding { target: item; property: "widget"; value: widgetData; when: item !== null }
                            Binding { target: item; property: "hass";   value: stateData;  when: item !== null }
                            Connections {
                                target: item
                                ignoreUnknownSignals: true
                                function onCall(domain, service, data) { impl.callService(domain, service, data) }
                            }
                        }
                    }
                }
            }

            // Scroll indicator track + thumb. Lives outside the Flickable so it
            // doesn't scroll with the content.
            Rectangle {
                id: scrollTrack
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 4
                radius: 2
                color: "#1a2a40"
                opacity: 0.4
                visible: scrollThumb.visible
            }
            Rectangle {
                id: scrollThumb
                anchors.right: parent.right
                width: 4
                radius: 2
                color: "#5ab8ff"
                opacity: 0.7
                visible: dashboardScroll.contentHeight > dashboardScroll.height + 1
                y: dashboardScroll.contentY *
                   (dashboardScroll.height /
                    Math.max(1, dashboardScroll.contentHeight))
                height: Math.max(24, dashboardScroll.height *
                                     (dashboardScroll.height /
                                      Math.max(1, dashboardScroll.contentHeight)))
            }
        }
    }

    function loadVersion() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "file:///data/qpext/version.txt")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status !== 0 && xhr.status !== 200) return
            var t = (xhr.responseText || "").replace(/\s+$/, "")
            if (t) impl.version = t
        }
        xhr.send()
    }
    Component.onCompleted: {
        console.log("[qpext] HelloImpl.qml rev=" + impl.revision + " loaded")
        loadVersion()
        loadHaConfig()
        loadConfig(false)
    }
}
