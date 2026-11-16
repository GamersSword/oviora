import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import "FocusNav.js" as Nav
import Oviora

// Pill button. kind: "primary" (purple fill, the main action), "secondary" (outlined), "danger"
// (outlined, red text), "quiet" (text only)
T.AbstractButton {
    id: control
    property string kind: "secondary"
    property bool compact: false
    property bool navUpDown: true

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    implicitHeight: compact ? 36 : 42
    implicitWidth: Math.max(implicitHeight, label.implicitWidth + (compact ? 28 : 40))
    opacity: enabled ? 1 : 0.4

    Keys.onPressed: (event) => {
        if (control.navUpDown && Nav.handleUpDown(control, event))
            return;
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            control.clicked();
            event.accepted = true;
        }
    }

    background: Rectangle {
        radius: height / 2
        color: {
            switch (control.kind) {
            case "primary": return control.down ? Tema.accentDown : (control.hovered ? Tema.accentHover : Tema.accent);
            case "quiet": return control.down ? Tema.line : (control.hovered ? Tema.surface2 : "transparent");
            default: return control.down ? Tema.line : (control.hovered ? Tema.surface2 : "transparent");
            }
        }
        border.width: control.kind === "primary" || control.kind === "quiet" ? 0 : 1
        border.color: control.kind === "danger" ? Tema.soft(Tema.danger, control.hovered ? 0.6 : 0.35)
                                                : (control.hovered ? Tema.lineStrong : Tema.line)
        // keyboard focus
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
    contentItem: Label {
        textFormat: Text.PlainText
        id: label
        text: control.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.family: Tema.body
        font.pixelSize: control.compact ? 14 : 15
        font.weight: Font.DemiBold
        color: {
            switch (control.kind) {
            case "primary": return Tema.inkOnAccent;
            case "danger": return Tema.danger;
            case "quiet": return control.hovered ? Tema.text : Tema.accentText;
            default: return Tema.text;
            }
        }
    }
}
