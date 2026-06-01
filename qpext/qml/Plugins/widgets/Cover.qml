// cover.* widget: tap = toggle, long-press = stop. Shows open/closed/percent.
import QtQuick 2.9
import QtQuick.Layouts 1.3
import Qing.Controls 1.0
import ".."

Frame {
    id: w
    on: !!(hass && (hass.state === "open" || hass.state === "opening"))
    longPressed: function() {
        if (!widget || !widget.entity) return
        w.call("cover", "stop_cover", { "entity_id": widget.entity })
    }

    onTapped: {
        if (!widget || !widget.entity) return
        var svc = (hass && hass.state === "open") ? "close_cover" : "open_cover"
        w.call("cover", svc, { "entity_id": widget.entity })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            MdiIcon {
                Layout.preferredWidth: 44; Layout.preferredHeight: 44
                name: {
                    if (widget && widget.icon) return widget.icon
                    var s = hass ? hass.state : "closed"
                    if (s === "open" || s === "opening") return "window-open"
                    return "window-closed"
                }
                size: 42
                color: w.on ? "#5ab8ff" : "#88aacc"
            }
            QText {
                Layout.fillWidth: true
                text: w.displayName
                color: "#88aacc"
                font.pixelSize: 20
                elide: Text.ElideRight
            }
        }

        Item { Layout.fillHeight: true }

        QText {
            Layout.fillWidth: true
            text: {
                if (!hass) return "--"
                var pos = hass.attributes ? hass.attributes.current_position : undefined
                if (pos !== undefined && pos !== null) return pos + "%"
                return hass.state
            }
            color: "white"
            font.pixelSize: 30
            font.bold: true
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
        }
    }
}
