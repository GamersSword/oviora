import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// A group of settings: heading, one plain sentence about what it is for, then the rows separated
// by hairlines. No box around it: groups are told apart by their headings and the space between.
Item {
    id: card
    property string title
    property string description
    property color accent: Tema.accent
    default property alias content: rows.data
    property alias headerExtra: headerSlot.data

    width: parent ? parent.width : 600
    implicitHeight: body.implicitHeight + 8
    height: implicitHeight

    ColumnLayout {
        id: body
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            leftMargin: 0
            rightMargin: 0
            topMargin: 0
        }
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            visible: card.title.length > 0 || card.description.length > 0
            spacing: 16
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: card.title.length > 0
                    text: card.title
                    wrapMode: Text.WordWrap
                    font.family: Tema.display
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                    font.letterSpacing: -0.3
                    color: Tema.text
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: card.description.length > 0
                    text: card.description
                    wrapMode: Text.WordWrap
                    font.family: Tema.body
                    font.pixelSize: 14
                    lineHeight: 1.25
                    color: Tema.muted
                }
            }
            Item {
                id: headerSlot
                Layout.alignment: Qt.AlignTop
                implicitWidth: childrenRect.width
                implicitHeight: childrenRect.height
            }
        }

        ColumnLayout {
            id: rows
            Layout.fillWidth: true
            Layout.topMargin: card.title.length > 0 || card.description.length > 0 ? 12 : 0
            spacing: 0
        }
    }
}
