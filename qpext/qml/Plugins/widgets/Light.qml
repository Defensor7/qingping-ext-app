// Light widget: tap upper area = toggle; drag the bar at the bottom = brightness.
import QtQuick 2.9
import QtQuick.Layouts 1.3
import Qing.Controls 1.0
import ".."

Frame {
    id: w
    on: !!(hass && hass.state === "on")

    property int brightnessPct: {
        if (!hass || !hass.attributes || hass.attributes.brightness === undefined ||
            hass.attributes.brightness === null) return 0
        return Math.round(hass.attributes.brightness * 100 / 255)
    }

    onTapped: {
        if (!widget || !widget.entity) return
        var domain = widget.entity.split(".")[0]
        w.call(domain, "toggle", { "entity_id": widget.entity })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            MdiIcon {
                Layout.preferredWidth: 44; Layout.preferredHeight: 44
                name: (widget && widget.icon) || (w.on ? "lightbulb" : "lightbulb-off")
                size: 42
                color: w.on ? "white" : "#88aacc"
            }
            QText {
                Layout.fillWidth: true
                text: w.displayName
                color: w.on ? "#bfe1ff" : "#88aacc"
                font.pixelSize: 20
                elide: Text.ElideRight
                wrapMode: Text.WordWrap
                maximumLineCount: 2
            }
        }

        Item { Layout.fillHeight: true }

        QText {
            Layout.fillWidth: true
            text: w.on ? (w.brightnessPct + "%") : "OFF"
            color: "white"
            font.pixelSize: 30
            font.bold: true
            horizontalAlignment: Text.AlignRight
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 22
            radius: 6
            color: "#0d2538"
            border.width: 1
            border.color: "#1f4870"
            Rectangle {
                anchors {
                    left: parent.left; top: parent.top; bottom: parent.bottom
                    margins: 2
                }
                width: w.on ? Math.max(0, (parent.width - 4) * w.brightnessPct / 100) : 0
                radius: 4
                color: "#5ab8ff"
                Behavior on width { NumberAnimation { duration: 150 } }
            }
            MouseArea {
                anchors.fill: parent
                onClicked: setFromX(mouse.x)
                onPositionChanged: if (pressed) setFromX(mouse.x)
                function setFromX(x) {
                    var pct = Math.max(1, Math.min(100, Math.round(x * 100 / width)))
                    w.call("light", "turn_on",
                           { "entity_id": widget.entity, "brightness_pct": pct })
                }
            }
        }
    }
}
