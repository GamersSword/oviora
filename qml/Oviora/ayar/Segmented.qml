import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import "FocusNav.js" as Nav
import Oviora

// A choice of 2-4 options shown side by side. Left/Right change it, Up/Down move on.
T.Control {
    id: control
    property var model: []
    property int currentIndex: -1
    property int segmentWidth: 0       // 0: as wide as the text
    signal activated(int index)

    function choose(index) {
        if (index < 0 || index >= model.length || !enabled)
            return;
        if (index !== currentIndex)
            activated(index);
    }

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    padding: 4
    implicitWidth: segments.implicitWidth + leftPadding + rightPadding
    implicitHeight: 44
    opacity: enabled ? 1 : 0.45

    Keys.onPressed: (event) => {
        if (Nav.handleUpDown(control, event))
            return;
        switch (event.key) {
        case Qt.Key_Left:
            choose(currentIndex - 1);
            event.accepted = true;
            break;
        case Qt.Key_Right:
            choose(currentIndex + 1);
            event.accepted = true;
            break;
        }
    }

    background: Rectangle {
        radius: 22
        color: Tema.bg
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? Tema.accent : Tema.line
    }

    contentItem: Row {
        id: segments
        spacing: 2
        Repeater {
            model: control.model
            delegate: T.AbstractButton {
                id: seg
                required property int index
                required property var modelData
                readonly property bool selected: index === control.currentIndex
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                height: segments.height
                width: control.segmentWidth > 0 ? control.segmentWidth : segLabel.implicitWidth + 30
                onClicked: {
                    control.forceActiveFocus(Qt.MouseFocusReason);
                    control.choose(index);
                }
                background: Rectangle {
                    radius: height / 2
                    color: seg.selected ? Tema.accentSoft : (seg.hovered ? Tema.surface2 : "transparent")
                    Behavior on color { ColorAnimation { duration: 110 } }
                    // light bar under the chosen one
                    Rectangle {
                        anchors {
                            bottom: parent.bottom
                            bottomMargin: 4
                            horizontalCenter: parent.horizontalCenter
                        }
                        width: 0
                        visible: false
                        height: 3
                        radius: 1.5
                        color: Tema.accent
                        Behavior on width { NumberAnimation { duration: 140 } }
                    }
                }
                contentItem: Label {
                    textFormat: Text.PlainText
                    id: segLabel
                    text: seg.modelData
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.family: Tema.body
                    font.pixelSize: 14
                    font.weight: seg.selected ? Font.DemiBold : Font.Normal
                    color: seg.selected ? Tema.text : (seg.hovered ? Tema.text : Tema.muted)
                }
            }
        }
    }
}
