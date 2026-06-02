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

    function toggleOnOff() {
        if (!widget || !widget.entity) return
        var svc = (w.mode === "off") ? "turn_on" : "turn_off"
        w.call("climate", svc, { "entity_id": widget.entity })
    }

    // cycleMode() retained for any future UI that wants HVAC-mode switching.
    function cycleMode() {
        if (!widget || !widget.entity || !hass || !hass.attributes) return
        var modes = hass.attributes.hvac_modes || []
        if (modes.length === 0) return
        var i = modes.indexOf(w.mode)
        var next = modes[(i + 1) % modes.length]
        w.call("climate", "set_hvac_mode",
               { "entity_id": widget.entity, "hvac_mode": next })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            MdiIcon {
                Layout.preferredWidth: w.iconSize || 36
                Layout.preferredHeight: w.iconSize || 36
                name: w.on ? ((widget && widget.icon) || "thermostat")
                           : "power-off"
                size: w.iconSize || 34
                color: w.iconColor || (w.on ? "#5ab8ff" : "#88aacc")
            }
            QText {
                Layout.fillWidth: true
                text: w.displayName
                color: w.titleColor || "#88aacc"
                font.pixelSize: w.titleSize || 18
                elide: Text.ElideRight
            }
        }

        Item { Layout.fillHeight: true }

        QText {
            Layout.fillWidth: true
            text: w.currentTemp.toFixed(1) + "°"
            color: w.valueColor || "white"
            font.pixelSize: w.valueSize || 34
            font.bold: true
            horizontalAlignment: Text.AlignRight
        }
        // Static mode/setpoint label (no longer a button — user didn't want
        // the extra control on the card; HVAC mode is left to HA-side
        // automations).
        QText {
            Layout.fillWidth: true
            text: w.mode + " · set " + w.setTemp.toFixed(1) + "°"
            color: "#88aacc"
            font.pixelSize: 14
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
        }

        // Three equal buttons: − / power / + . Power swaps icon and
        // colour on state so it's obvious which side is currently armed.
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
                color: powerArea.pressed
                       ? "#2f6db0"
                       : (w.on ? "#1f5588" : "#0d2538")
                border.color: w.on ? "#5ab8ff" : "#1f4870"
                border.width: 1
                MdiIcon {
                    anchors.centerIn: parent
                    name: "power"
                    size: 24
                    color: w.on ? "white" : "#88aacc"
                }
                MouseArea { id: powerArea; anchors.fill: parent; onClicked: w.toggleOnOff() }
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
