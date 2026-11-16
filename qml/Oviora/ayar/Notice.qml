import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// A short note inside a card or page. tone: "info", "warn", "ok", "error"
Rectangle {
    id: notice
    property string text
    property string tone: "info"
    readonly property color toneColor: tone === "warn" ? Tema.warm : tone === "ok" ? Tema.ok : tone === "error" ? Tema.danger : Tema.accent
    default property alias extra: extraRow.data

    Layout.fillWidth: true
    width: parent ? parent.width : 400
    implicitHeight: col.implicitHeight + 24
    radius: Tema.radiusInput
    color: Qt.rgba(toneColor.r, toneColor.g, toneColor.b, 0.09)

    ColumnLayout {
        id: col
        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
            leftMargin: 16
            rightMargin: 16
        }
        spacing: 10
        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: notice.text
            wrapMode: Text.WordWrap
            font.family: Tema.body
            font.pixelSize: 14
            lineHeight: 1.1
            color: Tema.text
        }
        RowLayout {
            id: extraRow
            spacing: 8
            visible: children.length > 0
        }
    }
}
