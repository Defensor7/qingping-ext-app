import QtQuick 2.0
import QtQuick.Layouts 1.3
import Qing.Controls 1.0
import Qing.Styles 1.0

Item {
    id: control
    property alias timeVisible: time.visible
    implicitWidth: Styles.windowWidth
    implicitHeight: 72
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        spacing: 10
        WiFiIndicator {
            Layout.preferredWidth: 34
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignVCenter
            visible: wifiManager.wlanEnabled
        }
        TimeIndicator {
            id: time
        }
        Image {
            id: alarm
            source: "qrc:/modules/alarm/icon_alarm_small.png"
            visible:  alarmManager.hasAlarms
        }

        QText{
            visible: global.serverType === 0x100
            text: "[测]"
            Layout.alignment: Qt.AlignVCenter
        }

        Item { Layout.fillWidth: true }


        BatteryIndicator {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignVCenter
        }
        QText {
            visible: global.serverType === 0x100
            text: "OL:" + screenManager.outLightInfo + ", v:" + batteryManager.voltage + ", cc:" + batteryManager.current
            font.pixelSize: 20
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
