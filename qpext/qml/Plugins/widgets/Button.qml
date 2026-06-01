import QtQuick 2.9
import QtQuick.Layouts 1.3
import Qing.Controls 1.0
import ".."

Frame {
    id: w
    available: true
    color: tapArea.pressed ? "#3a1a1a" : "#231414"
    border.color: "#5a2828"

    onTapped: {
        if (!widget || !widget.service) return
        var p = widget.service.split(".")
        if (p.length === 2) w.call(p[0], p[1], widget.data || {})
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 14
        MdiIcon {
            Layout.preferredWidth: 56; Layout.preferredHeight: 56
            name: (widget && widget.icon) || "power"
            size: 52
            color: "#ff9a7a"
        }
        QText {
            Layout.fillWidth: true
            text: widget ? (widget.label || widget.service || "") : ""
            color: "white"
            font.pixelSize: 24
            font.bold: true
            elide: Text.ElideRight
            wrapMode: Text.WordWrap
            maximumLineCount: 2
        }
    }
    MouseArea { id: tapArea; anchors.fill: parent; onClicked: w.tapped() }
}
