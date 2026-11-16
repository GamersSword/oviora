import QtQuick
import QtQuick.Controls
import Oviora

// A keyboard key drawn as a key cap
Item {
    id: cap
    property string text
    property bool highlighted: false
    property bool warn: false
    property bool small: false

    implicitWidth: Math.max(small ? 30 : 46, label.implicitWidth + (small ? 14 : 22))
    implicitHeight: small ? 26 : 36

    // the key's lower edge
    Rectangle {
        anchors.fill: parent
        radius: cap.small ? 6 : 8
        color: cap.warn ? Tema.soft(Tema.warm, 0.35) : (cap.highlighted ? Tema.accentSoft : Tema.bg)
    }
    Rectangle {
        anchors {
            fill: parent
            bottomMargin: cap.small ? 2 : 3
        }
        radius: cap.small ? 6 : 8
        color: cap.warn ? Tema.soft(Tema.warm, 0.12) : (cap.highlighted ? Tema.surface2 : Tema.line)
        border.width: 1
        border.color: cap.warn ? Tema.warm : (cap.highlighted ? Tema.accent : Tema.lineStrong)
        Label {
            textFormat: Text.PlainText
            id: label
            anchors.centerIn: parent
            text: cap.text
            font.family: Tema.display
            font.pixelSize: cap.small ? 12 : 15
            font.weight: Font.DemiBold
            color: cap.warn ? Tema.warm : Tema.text
        }
    }
}
