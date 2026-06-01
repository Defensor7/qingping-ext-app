// script.* and scene.* widgets — one-shot trigger.
import QtQuick 2.9
import QtQuick.Layouts 1.3
import Qing.Controls 1.0
import ".."

Frame {
    id: w
    available: true
    color: w.bgColor || (tapArea.pressed ? "#1a3a55" : "#142b45")
    border.color: "#1f4870"

    onTapped: {
        if (!widget || !widget.entity) return
        var domain = widget.entity.split(".")[0]   // "script" or "scene"
        w.call(domain, "turn_on", { "entity_id": widget.entity })
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 14
        MdiIcon {
            Layout.preferredWidth: w.iconSize || 56
            Layout.preferredHeight: w.iconSize || 56
            name: (widget && widget.icon) ||
                  (widget && widget.entity && widget.entity.indexOf("scene.") === 0
                       ? "play-circle" : "script-text")
            size: w.iconSize || 52
            color: w.iconColor || "#5ab8ff"
        }
        QText {
            Layout.fillWidth: true
            text: w.displayName
            color: w.titleColor || "white"
            font.pixelSize: w.titleSize || 24
            font.bold: true
            elide: Text.ElideRight
            wrapMode: Text.WordWrap
            maximumLineCount: 2
        }
    }
    MouseArea { id: tapArea; anchors.fill: parent; onClicked: w.tapped() }
}
