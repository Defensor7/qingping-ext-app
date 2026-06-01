import QtQuick 2.0
import QtQuick.Layouts 1.3
import Qing.Snow 1.0

Item {
    id: control
    property real index: 1
    RowLayout {
        anchors.fill: parent
        spacing: 10
        Image {
            id: icon
            Layout.alignment: Qt.AlignVCenter
            source: {
                if (WiFiState.ConnectedState === wifiManager.state) {
                    return "qrc:/main/wifi/wifi_signal"+ wifiManager.rssiNum +".png"
                } else if (WiFiState.UnusableState === wifiManager.state) {
                    return "qrc:/main/wifi/wifi_unusable.png";
                } else if (WiFiState.ConnectingState === wifiManager.state || WiFiState.ObtainingIpState === wifiManager.state) {
                    return "qrc:/main/wifi/wifi_signal"+ control.index +".png";
                } else {
                    return "qrc:/main/wifi/wifi_disconnected.png";
                }
            }
            smooth: true
        }
        Item { Layout.fillWidth: true }
    }
    Timer {
        id: wifiAnimation
        interval: 500
        repeat: true
        triggeredOnStart: true
        running: WiFiState.ConnectingState === wifiManager.state || WiFiState.ObtainingIpState === wifiManager.state
        onTriggered: {
            var i = control.index + 1;
            if (i > 4) {
                i = 1;
            }
            control.index = i;
        }
    }
}
