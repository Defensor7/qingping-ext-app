import QtQuick 2.9
import QtQuick.Layouts 1.3
import Qing.Controls 1.0
import ".."

Frame {
    id: w
    on: !!(hass && hass.state === "on")

    onTapped: {
        if (!widget || !widget.entity) return
        var domain = widget.entity.split(".")[0]
        w.call(domain, "toggle", { "entity_id": widget.entity })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            MdiIcon {
                Layout.preferredWidth: w.iconSize || 44
                Layout.preferredHeight: w.iconSize || 44
                name: {
                    if (widget && widget.icon) return widget.icon
                    if (widget && widget.entity && widget.entity.indexOf("light.") === 0)
                        return w.on ? "lightbulb" : "lightbulb-off"
                    return w.on ? "toggle-switch" : "toggle-switch-off"
                }
                size: w.iconSize || 42
                color: w.iconColor || (w.on ? "white" : "#88aacc")
            }
            QText {
                Layout.fillWidth: true
                text: w.displayName
                color: w.titleColor || (w.on ? "#bfe1ff" : "#88aacc")
                font.pixelSize: w.titleSize || 20
                elide: Text.ElideRight
                wrapMode: Text.WordWrap
                maximumLineCount: 2
            }
        }

        Item { Layout.fillHeight: true }

        QText {
            Layout.fillWidth: true
            text: hass ? (hass.state === "on" ? "ON" :
                          hass.state === "off" ? "OFF" : hass.state) : "--"
            color: w.valueColor || "white"
            font.pixelSize: w.valueSize || 30
            font.bold: true
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
        }
    }
}
