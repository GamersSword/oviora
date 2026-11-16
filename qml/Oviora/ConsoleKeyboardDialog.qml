// PS5 on-screen keyboard: opens centered over the picture when connection.keyboard is true.
// Sends edits debounced (150 ms) via oviora.keyboardSetText; Enter accepts, Esc cancels.
// Floats over the picture: cut out of the native video window (videoCover).
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import "ayar" as S

Rectangle {
    id: dialog
    objectName: "console-keyboard-dialog"
    readonly property bool videoCover: visible
    readonly property real coverRadius: Tema.radiusPanel

    property bool active: false
    visible: active

    width: Math.min(560, (parent ? parent.width : 560) - 48)
    height: column.implicitHeight + 56
    x: Math.round(((parent ? parent.width : width) - width) / 2)
    y: Math.round(((parent ? parent.height : height) - height) / 2)
    radius: Tema.radiusPanel
    color: Tema.surface
    border.color: Tema.lineStrong
    border.width: 1

    // Keeps clicks and the wheel off the picture below
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: wheel => wheel.accepted = true
    }

    Timer {
        id: sendTimer
        interval: 150
        onTriggered: oviora.keyboardSetText(field.text)
    }

    function accept() {
        sendTimer.stop()
        oviora.keyboardSetText(field.text)
        oviora.keyboardAccept()
    }

    function reject() {
        sendTimer.stop()
        oviora.keyboardReject()
    }

    onVisibleChanged: {
        if (visible) {
            sendTimer.stop()
            field.text = (oviora.connection && oviora.connection.keyboardText) || ""
            field.cursorPosition = field.text.length
            field.forceActiveFocus()
        } else {
            sendTimer.stop()
            const w = dialog.Window.window
            if (w && w.contentItem) w.contentItem.forceActiveFocus()
        }
    }

    Shortcut {
        sequence: "Esc"
        enabled: dialog.visible
        onActivated: dialog.reject()
    }

    ColumnLayout {
        id: column
        anchors {
            fill: parent
            margins: 28
        }
        spacing: 14

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("PS5 klavyesi")
                font.family: Tema.display
                font.pixelSize: 22
                font.weight: Font.DemiBold
                color: Tema.text
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Yazdığın PS5'e gider. Enter ile onayla, Esc ile iptal et.")
                font.family: Tema.body
                font.pixelSize: Tema.smallSize
                color: Tema.muted
            }
        }

        S.InputField {
            id: field
            Layout.fillWidth: true
            Layout.topMargin: 4
            implicitHeight: 50
            font.pixelSize: 18
            onTextEdited: sendTimer.restart()
            Keys.onReturnPressed: dialog.accept()
            Keys.onEnterPressed: dialog.accept()
            Keys.onEscapePressed: dialog.reject()
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: 4
            spacing: 10
            S.PillButton {
                kind: "quiet"
                navUpDown: false
                focusPolicy: Qt.NoFocus
                text: qsTr("İptal")
                onClicked: dialog.reject()
            }
            S.PillButton {
                kind: "primary"
                navUpDown: false
                focusPolicy: Qt.NoFocus
                text: qsTr("Tamam")
                onClicked: dialog.accept()
            }
        }
    }
}
