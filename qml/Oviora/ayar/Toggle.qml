import QtQuick
import QtQuick.Templates as T
import "FocusNav.js" as Nav
import Oviora

// On/off switch. Emits toggled() only when the user flips it (like CheckBox.onToggled).
T.AbstractButton {
    id: control
    checkable: true
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    implicitWidth: 52
    implicitHeight: 30
    padding: 0

    Keys.onPressed: (event) => {
        if (Nav.handleUpDown(control, event))
            return;
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            control.toggle();
            control.toggled();
            event.accepted = true;
        }
    }

    background: Item {}
    contentItem: Item {
        Rectangle {
            id: track
            anchors.fill: parent
            radius: height / 2
            color: control.checked ? Tema.accent : (control.hovered ? Tema.line : Tema.surface2)
            border.width: control.checked ? 0 : 1
            border.color: Tema.lineStrong
            opacity: control.enabled ? 1 : 0.4
            Behavior on color { ColorAnimation { duration: 120 } }
        }
        Rectangle {
            width: 22
            height: 22
            radius: 11
            anchors.verticalCenter: parent.verticalCenter
            x: control.checked ? parent.width - width - 4 : 4
            color: control.checked ? Tema.inkOnAccent : Tema.muted
            opacity: control.enabled ? 1 : 0.5
            Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        }
        // Keyboard focus ring
        Rectangle {
            anchors {
                fill: parent
                margins: -4
            }
            radius: height / 2
            color: "transparent"
            border.width: 2
            border.color: Tema.accent
            visible: control.visualFocus
        }
    }
}
