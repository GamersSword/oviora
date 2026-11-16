import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import "FocusNav.js" as Nav
import Oviora

// A slider with its value written next to it. moved() fires for mouse and keyboard changes.
T.Slider {
    id: control
    property string valueText: value.toFixed(0)
    property int valueWidth: 112

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    implicitWidth: 340
    implicitHeight: 36
    leftPadding: 10
    rightPadding: valueWidth + 14
    opacity: enabled ? 1 : 0.45

    Keys.onPressed: (event) => Nav.handleUpDown(control, event)

    background: Item {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 6
        Rectangle {
            anchors.fill: parent
            radius: 3
            color: Tema.line
        }
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 3
            color: Tema.accent
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: 20
        height: 20
        radius: 10
        color: control.pressed ? "#FFFFFF" : Tema.text
        border.width: control.visualFocus ? 3 : 0
        border.color: Tema.accent
        Rectangle {
            anchors.centerIn: parent
            visible: control.visualFocus
            width: 30
            height: 30
            radius: 15
            color: "transparent"
            border.width: 2
            border.color: Tema.accent
            opacity: 0.6
        }
    }

    Label {
        textFormat: Text.PlainText
        anchors {
            right: parent.right
            verticalCenter: parent.verticalCenter
        }
        width: control.valueWidth
        horizontalAlignment: Text.AlignRight
        elide: Text.ElideRight
        text: control.valueText
        font.family: Tema.body
        font.pixelSize: 14
        font.weight: Font.DemiBold
        color: Tema.text
    }
}
