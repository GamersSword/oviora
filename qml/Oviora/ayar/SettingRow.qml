import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// One setting: what it is and what it does on the left, the control on the right.
// With stacked: true the control sits under the text (wide controls such as several dropdowns).
Item {
    id: row
    readonly property bool isSettingRow: true
    property string title
    property string help
    property bool stacked: false
    property bool separator: true
    property int controlWidth: -1
    default property alias control: slot.data
    readonly property bool current: slot.activeFocus

    Layout.fillWidth: true
    implicitHeight: (stacked ? texts.implicitHeight + 12 + slot.implicitHeight
                             : Math.max(texts.implicitHeight, slot.implicitHeight)) + 32

    Rectangle {
        visible: row.separator
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        height: 1
        color: Tema.line
    }

    // The light bar marks the row being changed
    Rectangle {
        anchors {
            left: parent.left
            leftMargin: -18
            verticalCenter: parent.verticalCenter
        }
        width: 3
        height: Math.min(parent.height - 24, 28)
        radius: 1.5
        color: Tema.accent
        opacity: row.current ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }

    ColumnLayout {
        id: texts
        anchors {
            left: parent.left
            right: row.stacked ? parent.right : slot.left
            rightMargin: row.stacked ? 0 : 24
            top: row.stacked ? parent.top : undefined
            topMargin: row.stacked ? 15 : 0
            verticalCenter: row.stacked ? undefined : parent.verticalCenter
        }
        spacing: 3
        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: row.title
            visible: text.length > 0
            wrapMode: Text.WordWrap
            font.family: Tema.body
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: row.enabled ? Tema.text : Tema.faint
        }
        Label {
            Layout.fillWidth: true
            text: row.help
            visible: text.length > 0
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            font.family: Tema.body
            font.pixelSize: 14
            lineHeight: 1.2
            color: row.enabled ? Tema.muted : Tema.faint
        }
    }

    FocusScope {
        id: slot
        anchors {
            right: parent.right
            left: row.stacked ? parent.left : undefined
            verticalCenter: row.stacked ? undefined : parent.verticalCenter
            top: row.stacked ? texts.bottom : undefined
            topMargin: row.stacked ? 12 : 0
        }
        width: row.stacked ? undefined : (row.controlWidth > 0 ? row.controlWidth : childrenRect.width)
        implicitWidth: childrenRect.width
        implicitHeight: childrenRect.height
        height: implicitHeight
    }
}
