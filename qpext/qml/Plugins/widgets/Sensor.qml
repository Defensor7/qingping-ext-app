import QtQuick 2.9
import QtQuick.Layouts 1.3
import Qing.Controls 1.0
import ".."

Frame {
    id: w
    onTapped: { /* sensor is read-only */ }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            MdiIcon {
                Layout.preferredWidth: 36; Layout.preferredHeight: 36
                name: widget && widget.icon ? widget.icon : "thermometer"
                size: 36
                color: "#88aacc"
            }
            QText {
                Layout.fillWidth: true
                text: w.displayName
                color: "#88aacc"
                font.pixelSize: 18
                elide: Text.ElideRight
            }
        }

        Item { Layout.fillHeight: true }

        QText {
            Layout.fillWidth: true
            text: {
                if (!hass) return "--"
                var v  = hass.state
                var u  = (widget && widget.unit) ||
                         (hass.attributes ? hass.attributes.unit_of_measurement || "" : "")
                return v + (u ? " " + u : "")
            }
            color: "white"
            font.pixelSize: 36
            font.bold: true
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
        }
    }
}
