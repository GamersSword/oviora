import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Templates as T
import "FocusNav.js" as Nav
import Oviora

// "Gelişmiş" part at the end of a card: closed by default, the rare settings live in it
ColumnLayout {
    id: section
    property string title: qsTr("Gelişmiş ayarlar")
    property bool expanded: false
    default property alias content: inner.data

    Layout.fillWidth: true
    spacing: 0

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: Tema.line
    }

    T.AbstractButton {
        id: toggle
        Layout.fillWidth: true
        Layout.topMargin: 6
        implicitHeight: 48
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        onClicked: section.expanded = !section.expanded
        Keys.onPressed: (event) => {
            if (Nav.handleUpDown(toggle, event))
                return;
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                toggle.clicked();
                event.accepted = true;
            }
        }
        background: Rectangle {
            radius: 12
            anchors {
                fill: parent
                leftMargin: -12
                rightMargin: -12
            }
            color: toggle.hovered ? Tema.surface2 : "transparent"
            border.width: toggle.visualFocus ? 2 : 0
            border.color: Tema.accent
        }
        contentItem: RowLayout {
            spacing: 10
            Canvas {
                Layout.preferredWidth: 12
                Layout.preferredHeight: 12
                rotation: section.expanded ? 90 : 0
                Behavior on rotation { NumberAnimation { duration: 140 } }
                contextType: "2d"
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    ctx.strokeStyle = "#A996FF";
                    ctx.lineWidth = 2;
                    ctx.lineCap = "round";
                    ctx.lineJoin = "round";
                    ctx.beginPath();
                    ctx.moveTo(4, 1.5);
                    ctx.lineTo(9, 6);
                    ctx.lineTo(4, 10.5);
                    ctx.stroke();
                }
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: section.title
                font.family: Tema.body
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Tema.accentText
            }
            Label {
                textFormat: Text.PlainText
                text: section.expanded ? qsTr("Gizle") : qsTr("Göster")
                font.family: Tema.body
                font.pixelSize: 13
                color: Tema.muted
            }
        }
    }

    ColumnLayout {
        id: inner
        Layout.fillWidth: true
        visible: section.expanded
        spacing: 0
    }
}
