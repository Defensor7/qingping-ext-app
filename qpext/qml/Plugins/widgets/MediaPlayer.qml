// media_player.* widget: shows title + play/pause/next/prev row.
import QtQuick 2.9
import QtQuick.Layouts 1.3
import Qing.Controls 1.0
import ".."

Frame {
    id: w
    on: !!(hass && hass.state === "playing")

    function svc(name) {
        if (widget && widget.entity)
            w.call("media_player", name, { "entity_id": widget.entity })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            MdiIcon {
                Layout.preferredWidth: 36; Layout.preferredHeight: 36
                name: (widget && widget.icon) || "television"
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

        QText {
            Layout.fillWidth: true
            text: {
                if (!hass) return "--"
                var a = hass.attributes || {}
                var title = a.media_title || a.app_name || ""
                if (title) return title
                return hass.state
            }
            color: "white"
            font.pixelSize: 18
            elide: Text.ElideRight
            maximumLineCount: 2
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: [
                    { ic: "skip-previous", svc: "media_previous_track" },
                    { ic: w.on ? "pause" : "play",
                      svc: w.on ? "media_pause" : "media_play" },
                    { ic: "skip-next",     svc: "media_next_track" }
                ]
                delegate: Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    radius: 6
                    color: bma.pressed ? "#1f4870" : "#0d2538"
                    border.color: "#1f4870"; border.width: 1
                    MdiIcon { anchors.centerIn: parent; name: modelData.ic; size: 28; color: "white" }
                    MouseArea { id: bma; anchors.fill: parent; onClicked: w.svc(modelData.svc) }
                }
            }
        }
    }
}
