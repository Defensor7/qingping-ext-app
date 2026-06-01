import QtQuick 2.0
import QtQuick.Layouts 1.3
import Qing.Snow 1.0
import Qing.Controls 1.0

Item {
    RowLayout {
        anchors.fill: parent
        spacing: 10
        Item { Layout.fillWidth: true }
        QText {
            Layout.alignment: Qt.AlignVCenter
            text: batteryManager.capacity + "%"
            font.pixelSize: 26
            visible: batteryManager.isShowBatteryPrecentage
        }
        Image {
            Layout.alignment: Qt.AlignVCenter
            source: "qrc:/main/battery/battery_charging.png"
            visible: batteryManager.isCharging
            smooth: true
            Layout.preferredWidth: 16
            Layout.preferredHeight: 23
        }
        Image {
            Layout.preferredWidth: 53
            Layout.preferredHeight: 24
            Layout.alignment: Qt.AlignVCenter
            source: "qrc:/main/battery/battery_background.png"
            smooth: true
            clip: true
            Item {
                anchors.fill: parent
                anchors.leftMargin: 5
                anchors.rightMargin: 8
                anchors.topMargin: 5
                anchors.bottomMargin: 5
                Rectangle{
                    anchors.left: parent.left
                    width: parent.width * (batteryManager.capacity/100.0)
                    height: parent.height
                    radius: 3
                    clip: true
                    color: batteryManager.isCharging ?  "#00e676" : "#ffffff"
                }
            }
        }

    }
}
