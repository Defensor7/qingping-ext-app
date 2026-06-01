// climate.* widget: current temp + setpoint with +/- buttons.
import QtQuick 2.9
import QtQuick.Layouts 1.3
import Qing.Controls 1.0
import ".."

Frame {
    id: w
    on: !!(hass && hass.state !== "off" && hass.state !== "unavailable")

    property real currentTemp: hass && hass.attributes ? (hass.attributes.current_temperature || 0) : 0
    property real setTemp:     hass && hass.attributes ? (hass.attributes.temperature        || 0) : 0
    property string mode:      hass ? hass.state : "off"

    function adjust(delta) {
        if (!widget || !widget.entity) return
        var t = Math.round((w.setTemp + delta) * 10) / 10
        w.call("climate", "set_temperature", { "entity_id": widget.entity, "temperature": t })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            MdiIcon {
                Layout.preferredWidth: 36; Layout.preferredHeight: 36
                name: (widget && widget.icon) || "thermostat"
                size: 34
                color: w.on ? "#5ab8ff" : "#88aacc"
            }
            QText {
                Layout.fillWidth: true
                text: widget ? (widget.label || widget.entity) : ""
                color: "#88aacc"
                font.pixelSize: 18
                elide: Text.ElideRight
            }
        }

        Item { Layout.fillHeight: true }

        QText {
            Layout.fillWidth: true
            text: w.currentTemp.toFixed(1) + "°"
            color: "white"
            font.pixelSize: 34
            font.bold: true
            horizontalAlignment: Text.AlignRight
        }
        QText {
            Layout.fillWidth: true
            text: w.mode + " · set " + w.setTemp.toFixed(1) + "°"
            color: "#88aacc"
            font.pixelSize: 14
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                radius: 6
                color: minusArea.pressed ? "#1f4870" : "#0d2538"
                border.color: "#1f4870"; border.width: 1
                QText { anchors.centerIn: parent; text: "–"; color: "white"; font.pixelSize: 30 }
                MouseArea { id: minusArea; anchors.fill: parent; onClicked: w.adjust(-0.5) }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                radius: 6
                color: plusArea.pressed ? "#1f4870" : "#0d2538"
                border.color: "#1f4870"; border.width: 1
                QText { anchors.centerIn: parent; text: "+"; color: "white"; font.pixelSize: 30 }
                MouseArea { id: plusArea; anchors.fill: parent; onClicked: w.adjust(+0.5) }
            }
        }
    }
}
