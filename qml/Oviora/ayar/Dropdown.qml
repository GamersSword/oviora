import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import "FocusNav.js" as Nav
import Oviora

// A list of choices. Enter/Space opens it; Up/Down move to the next control while it is closed.
T.ComboBox {
    id: control
    property int popupMinWidth: 0

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    implicitWidth: 300
    implicitHeight: 44
    leftPadding: 16
    rightPadding: 40
    font.family: Tema.body
    font.pixelSize: 15
    opacity: enabled ? 1 : 0.45

    Keys.onPressed: (event) => {
        if (popup.visible)
            return;
        if (Nav.handleUpDown(control, event))
            return;
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            popup.open();
            event.accepted = true;
        }
    }
    Keys.onReleased: (event) => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space)
            event.accepted = true;
    }

    background: Rectangle {
        radius: Tema.radiusInput
        color: Tema.surface2
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus || control.popup.visible ? Tema.accent : (control.hovered ? Tema.lineStrong : Tema.line)
    }

    contentItem: Label {
        textFormat: Text.PlainText
        text: control.displayText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        font: control.font
        color: Tema.text
    }

    indicator: Canvas {
        x: control.width - width - 16
        y: (control.height - height) / 2
        width: 12
        height: 7
        contextType: "2d"
        rotation: control.popup.visible ? 180 : 0
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.strokeStyle = "#8E8899";
            ctx.lineWidth = 2;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            ctx.beginPath();
            ctx.moveTo(1, 1);
            ctx.lineTo(width / 2, height - 1);
            ctx.lineTo(width - 1, 1);
            ctx.stroke();
        }
    }

    delegate: T.ItemDelegate {
        id: item
        required property int index
        required property var modelData
        readonly property bool selected: control.currentIndex === index
        width: ListView.view ? ListView.view.width : control.width
        implicitHeight: 42
        leftPadding: 18
        rightPadding: 14
        hoverEnabled: true
        highlighted: control.highlightedIndex === index
        contentItem: Label {
            textFormat: Text.PlainText
            text: control.textRole ? (Array.isArray(control.model) ? item.modelData[control.textRole] : item.modelData) : item.modelData
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            font.family: Tema.body
            font.pixelSize: 15
            font.weight: item.selected ? Font.DemiBold : Font.Normal
            color: item.selected ? Tema.accentText : Tema.text
        }
        background: Rectangle {
            radius: 9
            color: item.highlighted || item.hovered ? Tema.line : "transparent"
            Rectangle {
                visible: false
                anchors {
                    left: parent.left
                    leftMargin: 6
                    verticalCenter: parent.verticalCenter
                }
                width: 4
                height: 18
                radius: 2
                color: Tema.accent
            }
        }
    }

    popup: T.Popup {
        y: control.height + 6
        width: Math.max(control.width, control.popupMinWidth)
        implicitHeight: Math.min(contentItem.implicitHeight + 12, 360)
        padding: 6
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            T.ScrollBar.vertical: T.ScrollBar {
                id: popupBar
                policy: T.ScrollBar.AsNeeded
                padding: 2
                contentItem: Rectangle {
                    implicitWidth: 5
                    radius: 2.5
                    color: Tema.lineStrong
                }
            }
        }
        background: Rectangle {
            radius: 12
            color: Tema.surface2
            border.color: Tema.lineStrong
        }
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 110 }
        }
    }
}
