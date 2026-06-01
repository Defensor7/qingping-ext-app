// Mirror of qrc:/main.qml. Only difference: mainPage loader -> file:///.
// All relative dir-imports are rewritten to absolute qrc:/... so they
// still resolve from the original embedded resources.
import QtQuick 2.9
import QtQuick.Controls 2.0
import QtQuick.Layouts 1.3
import Qing.Eggs 1.0
import Qing.Functions 1.0
import Qing.Styles 1.0
import Qing.Frames 1.0
import Qing.Controls 1.0
import "qrc:/screensaver"
import "qrc:/Guide"
import "qrc:/Main"
import "qrc:/Setting"
import "qrc:/History"
import "qrc:/Power"
import "qrc:/Forecast/Weather"
import "qrc:/Forecast/AQI"

ApplicationWindow {
    id: window
    visible: true
    width: 720
    height: 720
    flags: Qt.Window | (isDesktop ? 0 : Qt.FramelessWindowHint)

    Rectangle {
        id: background
        anchors.fill: parent
        color: Styles.windowBackgroundColor
    }
    QStackView {
        id: stackView1
        anchors.fill: parent
        initialItem: Item {
            QStackView {
                id: stackView2
                anchors.fill: parent
                initialItem: global.isInitialized ? mainPage : guidePage
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
        visible: isDesktop && !screenManager.isScreenOn
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }
    }

    Component {
        id: guidePage
        Loader {
            id: guideLoader
            source: "qrc:/Guide/GuidePage.qml"
            Connections {
                target: guideLoader.item
                onExited: { stackView2.replace(mainPage) }
            }
        }
    }

    // <<< qpext: loader points to our on-disk MainPage >>>
    Component {
        id: mainPage
        Loader {
            source: "file:///data/qpext/MainPage.qml"
        }
    }

    Component {
        id: screenSaverPage
        Loader {
            source: "qrc:/ScreenSaver/ScreenSaverPage.qml"
        }
    }

    Connections {
        target: global

        function onButtonLongPressed() {
            if (screenManager.isScreenOn) {
                stackView1.pop(null)
                stackView1.push(powerOffPage)
            } else {
                screenManager.requestScreenOn()
            }
        }
        function onPrepareForShutdown() { stackView1.push(watingShutdownPage) }
        function onTriggerAlarm(alarm, triggeredTimes) {
            stackView1.push("qrc:/Modules/Alarms/AlarmTrigger.qml", {
                                alarmId: alarm.alarmId,
                                hour: alarm.hour,
                                minute: alarm.minute,
                                delay: alarm.delay,
                                triggeredTimes: triggeredTimes,
                                dismiss: () => { stackView1.pop() }
                            });
        }
        function onTriggerOverLimit(scene) {
            console.log("onTriggerOverLimit:", scene)
            stackView1.push("qrc:/Modules/OverLimit/OverLimitTrigger.qml", {
                                sceneId: scene.id,
                                name: global.getAirName(scene.name),
                                unit: global.getAirUnit(scene.name),
                                value: scene.valueStr,
                                triggerValue: scene.triggerValueStr,
                                sOperator: scene.sOperator,
                                backgoundColor: scene.triggerColor,
                                dismiss: () => { stackView1.pop() }
                            })
        }
    }
    Connections {
        target: global
        onButtonPressDown:    { console.log("====> main.qml onButtonPressDown") }
        onButtonPressRelease: { console.log("====> main.qml onButtonPressRelease") }
    }

    Component {
        id: powerOffPage
        Loader {
            id: powerOffLoader
            source: "qrc:/Power/PowerOffPage.qml"
            Connections {
                target: powerOffLoader.item
                onExited: stackView1.pop()
            }
        }
    }
    Component {
        id: watingShutdownPage
        Loader { source: "qrc:/Power/WatingShutdown.qml" }
    }

    Connections {
        target: screenManager
        function onOpenScreenSaver() {
            if (1 == stackView2.depth) { stackView2.push(screenSaverPage) }
        }
        function onCloseScreenSaver() {
            while (stackView2.depth > 1) { stackView2.pop(); }
        }
    }

    Component.onCompleted: console.log("[qpext] main.qml loaded from /data/qpext")
}
