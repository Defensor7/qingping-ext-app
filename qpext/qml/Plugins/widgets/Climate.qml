// climate.* widget: current temp + setpoint with +/- buttons and a
// detail Popup (tap empty card area) that exposes HVAC mode / preset /
// fan-mode controls, like Home Assistant's own climate dialog.
import QtQuick 2.9
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
// Aliased: QtQuick.Controls 2.0 has its own `Frame` component which would
// shadow the project-local one (no `tapped`/`call` signals on the Qt copy),
// so importing it under a namespace lets us still use Popup without losing
// the Frame.qml from `widgets/`.
import QtQuick.Controls 2.0 as QQC2
import Qing.Controls 1.0
import ".."

Frame {
    id: w
    on: !!(hass && hass.state !== "off" && hass.state !== "unavailable")

    property real currentTemp: hass && hass.attributes ? (hass.attributes.current_temperature || 0) : 0
    property real setTemp:     hass && hass.attributes ? (hass.attributes.temperature        || 0) : 0
    property string mode:      hass ? hass.state : "off"
    property var hvacModes:    hass && hass.attributes ? (hass.attributes.hvac_modes   || []) : []
    property var presetModes:  hass && hass.attributes ? (hass.attributes.preset_modes || []) : []
    property var fanModes:     hass && hass.attributes ? (hass.attributes.fan_modes    || []) : []
    property string preset:    hass && hass.attributes ? (hass.attributes.preset_mode  || "") : ""
    property string fan:       hass && hass.attributes ? (hass.attributes.fan_mode     || "") : ""

    // Tap on any empty part of the card opens the detail popup with full
    // HVAC / preset / fan controls. The three buttons below have their own
    // MouseAreas so their clicks don't accidentally trigger this.
    onTapped: detailPopup.open()

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

    function setMode(m) {
        if (!widget || !widget.entity || !m) return
        w.call("climate", "set_hvac_mode",
               { "entity_id": widget.entity, "hvac_mode": m })
    }
    function setPreset(p) {
        if (!widget || !widget.entity || !p) return
        w.call("climate", "set_preset_mode",
               { "entity_id": widget.entity, "preset_mode": p })
    }
    function setFan(f) {
        if (!widget || !widget.entity || !f) return
        w.call("climate", "set_fan_mode",
               { "entity_id": widget.entity, "fan_mode": f })
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

        // Three equal buttons: − / + / power. Power on the right edge per
        // the user's request; it colour-codes itself ON/OFF.
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
        }
    }

    // ------------------------------------------------------------------
    // Detail popup — full-screen HA-style climate dialog. Parented to the
    // toplevel Window's contentItem so it overlays the entire PathView,
    // not just the card. Tap empty card area on the widget side to open
    // (Frame.onTapped above); the X button or tap-outside dismisses.
    // ------------------------------------------------------------------
    QQC2.Popup {
        id: detailPopup
        parent: Window.window ? Window.window.contentItem : w
        modal: true
        focus: true
        x: parent ? (parent.width  - width)  / 2 : 0
        y: parent ? (parent.height - height) / 2 : 0
        width:  parent ? parent.width  : 720
        height: parent ? parent.height : 720
        closePolicy: QQC2.Popup.CloseOnEscape | QQC2.Popup.CloseOnPressOutside
        background: Rectangle {
            color: "#0c1420"
            border.color: "#1f4870"
            border.width: 1
        }
        contentItem: Item {
            anchors.fill: parent

            RowLayout {
                id: popHeader
                anchors { top: parent.top; left: parent.left; right: parent.right
                          margins: 16 }
                spacing: 8
                MdiIcon {
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    name: w.on ? ((widget && widget.icon) || "thermostat")
                               : "power-off"
                    size: 30
                    color: w.on ? "#5ab8ff" : "#88aacc"
                }
                QText {
                    Layout.fillWidth: true
                    text: w.displayName
                    color: "white"
                    font.pixelSize: 22
                    font.bold: true
                    elide: Text.ElideRight
                }
                Rectangle {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    radius: 20
                    color: closeArea.pressed ? "#1f4870" : "transparent"
                    border.color: "#1f4870"; border.width: 1
                    QText { anchors.centerIn: parent; text: "✕"; color: "white"; font.pixelSize: 18 }
                    MouseArea { id: closeArea; anchors.fill: parent; onClicked: detailPopup.close() }
                }
            }

            Flickable {
                anchors { top: popHeader.bottom; left: parent.left
                          right: parent.right; bottom: parent.bottom
                          margins: 16; topMargin: 12 }
                contentWidth: width
                contentHeight: popBody.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ColumnLayout {
                    id: popBody
                    width: parent.width
                    spacing: 12

                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 96
                        QText {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: w.currentTemp.toFixed(1) + "°"
                            color: "white"
                            font.pixelSize: 72
                            font.bold: true
                        }
                        ColumnLayout {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            QText {
                                Layout.alignment: Qt.AlignRight
                                text: "set " + w.setTemp.toFixed(1) + "°"
                                color: "#88aacc"
                                font.pixelSize: 18
                            }
                            QText {
                                Layout.alignment: Qt.AlignRight
                                text: hass && hass.attributes && hass.attributes.hvac_action
                                      ? hass.attributes.hvac_action : w.mode
                                color: "white"
                                font.pixelSize: 16
                                font.bold: true
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: [-1, -0.5, 0.5, 1]
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 50
                                radius: 6
                                color: tapArea.pressed ? "#1f4870" : "#0d2538"
                                border.color: "#1f4870"; border.width: 1
                                QText {
                                    anchors.centerIn: parent
                                    text: (modelData > 0 ? "+" : "") + modelData + "°"
                                    color: "white"
                                    font.pixelSize: 20
                                    font.bold: true
                                }
                                MouseArea {
                                    id: tapArea
                                    anchors.fill: parent
                                    onClicked: w.adjust(modelData)
                                }
                            }
                        }
                    }

                    QText {
                        text: "Mode"; color: "#88aacc"; font.pixelSize: 14
                        Layout.topMargin: 6
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: w.hvacModes
                            delegate: Rectangle {
                                width: chipText.implicitWidth + 24
                                height: 36
                                radius: 18
                                color: w.mode === modelData
                                       ? "#1f5588"
                                       : (chipArea.pressed ? "#1f4870" : "#0d2538")
                                border.color: w.mode === modelData ? "#5ab8ff" : "#1f4870"
                                border.width: 1
                                QText {
                                    id: chipText
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: "white"
                                    font.pixelSize: 14
                                    font.bold: w.mode === modelData
                                }
                                MouseArea {
                                    id: chipArea
                                    anchors.fill: parent
                                    onClicked: w.setMode(modelData)
                                }
                            }
                        }
                    }

                    QText {
                        visible: w.presetModes.length > 0
                        text: "Preset"; color: "#88aacc"; font.pixelSize: 14
                        Layout.topMargin: 6
                    }
                    Flow {
                        visible: w.presetModes.length > 0
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: w.presetModes
                            delegate: Rectangle {
                                width: presetText.implicitWidth + 24
                                height: 36
                                radius: 18
                                color: w.preset === modelData
                                       ? "#5a3a1f"
                                       : (presetArea.pressed ? "#4a2f1f" : "#0d2538")
                                border.color: w.preset === modelData ? "#ff9a55" : "#1f4870"
                                border.width: 1
                                QText {
                                    id: presetText
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: "white"
                                    font.pixelSize: 14
                                    font.bold: w.preset === modelData
                                }
                                MouseArea {
                                    id: presetArea
                                    anchors.fill: parent
                                    onClicked: w.setPreset(modelData)
                                }
                            }
                        }
                    }

                    QText {
                        visible: w.fanModes.length > 0
                        text: "Fan"; color: "#88aacc"; font.pixelSize: 14
                        Layout.topMargin: 6
                    }
                    Flow {
                        visible: w.fanModes.length > 0
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: w.fanModes
                            delegate: Rectangle {
                                width: fanText.implicitWidth + 24
                                height: 36
                                radius: 18
                                color: w.fan === modelData
                                       ? "#1f5544"
                                       : (fanArea.pressed ? "#1f4844" : "#0d2538")
                                border.color: w.fan === modelData ? "#5affb8" : "#1f4870"
                                border.width: 1
                                QText {
                                    id: fanText
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: "white"
                                    font.pixelSize: 14
                                    font.bold: w.fan === modelData
                                }
                                MouseArea {
                                    id: fanArea
                                    anchors.fill: parent
                                    onClicked: w.setFan(modelData)
                                }
                            }
                        }
                    }
                    Item { Layout.preferredHeight: 8 }
                }
            }
        }
    }
}
