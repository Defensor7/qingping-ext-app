import QtQuick 2.7
import QtQuick.Layouts 1.3
import QtQuick.Controls 2.2
import Qing.Controls 1.0
import QtGraphicalEffects 1.0

Item {
    id: control
    signal closed()
    signal confirmed()
    property alias text: tipText.text
    implicitWidth: 720
    implicitHeight: 80
    clip: true
    visible: true

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 20
        anchors.rightMargin: 20
        spacing: 20
        Rectangle {
            Layout.preferredWidth: 80
            Layout.fillHeight: true
            color: "white"
            opacity: 0.95
            radius: 24
            visible: control.enabled
            Image {
                source: "qrc:/common/cancel_normal.png"
                anchors.centerIn: parent
            }
            MouseArea {
                anchors.fill: parent
                onPressed: parent.opacity = 0.7
                onReleased: parent.opacity = 0.95
                onCanceled: parent.opacity = 0.95
                onClicked: control.closed()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "white"
            opacity: 0.95
            radius: 24
            QText {
                id: tipText
                anchors.centerIn: parent
                // wrapMode: Text.WordWrap
                color: "#10102a"
                font.pixelSize: 30
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideMiddle
                width: 480
            }
            Image {
                source: "qrc:/common/enter_black.png"
                visible: control.enabled
                anchors {
                    verticalCenter: parent.verticalCenter
                    right: parent.right
                    rightMargin: 30
                }
            }
            MouseArea{
                anchors.fill: parent
                onPressed: parent.opacity = 0.7
                onReleased: parent.opacity = 0.95
                onCanceled: parent.opacity = 0.95
                onClicked: control.confirmed()
            }
        }
    }
}
