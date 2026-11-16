import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import "FocusNav.js" as Nav
import Oviora

// Text entry. invalid: true paints the border red.
T.TextField {
    id: control
    property bool invalid: false

    implicitWidth: 300
    implicitHeight: 44
    leftPadding: 16
    rightPadding: 16
    verticalAlignment: TextInput.AlignVCenter
    font.family: Tema.body
    font.pixelSize: 15
    color: Tema.text
    selectionColor: Tema.accent
    selectedTextColor: "#FFFFFF"
    placeholderTextColor: Tema.faint
    selectByMouse: true
    opacity: enabled ? 1 : 0.45

    Keys.onPressed: (event) => Nav.handleUpDown(control, event)

    Label {
        textFormat: Text.PlainText
        x: control.leftPadding
        y: control.topPadding
        // (a text field has no availableWidth/Height of its own)
        width: control.width - control.leftPadding - control.rightPadding
        height: control.height - control.topPadding - control.bottomPadding
        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        verticalAlignment: Text.AlignVCenter
        visible: !control.length && !control.preeditText
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: Tema.radiusInput
        color: Tema.surface2
        border.width: control.activeFocus || control.invalid ? 2 : 1
        border.color: control.invalid ? Tema.danger : (control.activeFocus ? Tema.accent : Tema.line)
    }
}
