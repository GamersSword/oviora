import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Templates as T
import Oviora

// The old Oviora's controller mapping dialog, simplified for the new app: every DualSense button
// with the pad button that presses it. Click a box, then press the pad button that should stand
// in for that DualSense button. Saved as oviora.preferences.padButtonMap (only changed buttons);
// the bridge applies it before the state goes to the PS5 (bridge/GameInput.cpp).
Popup {
    id: dialog
    // Cross … PS, in ControllerState bit order
    property var buttonNames: []
    property var draft: []
    property int capturing: -1
    property int lastPressed: 0

    readonly property bool altered: {
        const saved = oviora.preferences.padButtonMap || {};
        for (let i = 0; i < 16; i++) {
            const was = saved[String(i)] !== undefined ? saved[String(i)] : i;
            if (draft[i] !== was)
                return true;
        }
        return false;
    }

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(920, parent.width - 64)
    modal: true
    dim: true
    padding: 28
    onAboutToShow: {
        const saved = oviora.preferences.padButtonMap || {};
        const d = [];
        for (let i = 0; i < 16; i++)
            d.push(saved[String(i)] !== undefined ? saved[String(i)] : i);
        draft = d;
        capturing = -1;
        lastPressed = oviora.padPressed;
        oviora.padCapture(true);
    }
    onClosed: {
        capturing = -1;
        oviora.padCapture(false);
    }

    Connections {
        target: oviora
        function onPadPressedChanged() {
            const now = oviora.padPressed;
            const fresh = now & ~dialog.lastPressed;
            dialog.lastPressed = now;
            if (dialog.capturing < 0 || !fresh)
                return;
            let bit = 0;
            while (bit < 16 && !(fresh & (1 << bit)))
                bit++;
            if (bit >= 16)
                return;
            const d = dialog.draft.slice();
            d[dialog.capturing] = bit;
            dialog.draft = d;
            dialog.capturing = -1;
        }
    }

    background: Rectangle { radius: Tema.radiusPanel; color: Tema.surface; border.color: Tema.lineStrong }

    // One DualSense button's box: the pad button that presses it
    component KeySlot: T.AbstractButton {
        id: slot
        property bool changed: false
        property bool waiting: false
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        implicitWidth: 132
        implicitHeight: 42
        background: Rectangle {
            radius: Tema.radiusInput
            color: slot.down ? Tema.line : (slot.hovered || slot.waiting ? Tema.surface2 : "transparent")
            border.width: slot.visualFocus || slot.waiting ? 2 : 1
            border.color: slot.visualFocus || slot.waiting ? Tema.accent : (slot.changed ? Tema.lineStrong : Tema.line)
        }
        contentItem: Label {
            textFormat: Text.PlainText
            text: slot.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            font.family: Tema.body
            font.pixelSize: 14
            font.weight: slot.changed || slot.waiting ? Font.DemiBold : Font.Normal
            color: slot.waiting ? Tema.accent : (slot.changed ? Tema.text : Tema.muted)
        }
    }

    contentItem: ColumnLayout {
        spacing: 16
        Label {
            textFormat: Text.PlainText
            text: qsTr("Tuş eşleme")
            font.family: Tema.display
            font.pixelSize: 22
            color: Tema.text
        }
        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: oviora.ps5.controller ? qsTr("Bir kutuya tıkla, sonra kumandada o DualSense tuşunun yerine geçecek tuşa bas")
                                        : qsTr("Bilgisayara bağlı kumanda bulunamadı. DualSense'i kabloyla ya da Bluetooth ile bağla.")
            font.family: Tema.body
            font.pixelSize: Tema.bodySize
            color: oviora.ps5.controller ? Tema.muted : Tema.warm
        }
        GridLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 8
            columns: 3
            rowSpacing: 6
            columnSpacing: 40
            Repeater {
                model: dialog.buttonNames.length
                delegate: RowLayout {
                    required property int index
                    spacing: 8
                    Label {
                        textFormat: Text.PlainText
                        Layout.preferredWidth: 140
                        text: dialog.buttonNames[index]
                        elide: Text.ElideRight
                        font.family: Tema.body
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        color: Tema.text
                    }
                    KeySlot {
                        waiting: dialog.capturing === index
                        changed: dialog.draft[index] !== index
                        text: waiting ? qsTr("Bir tuşa bas…") : (dialog.buttonNames[dialog.draft[index]] || "")
                        onClicked: dialog.capturing = waiting ? -1 : index
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 8
            spacing: 8
            PillButton {
                kind: "quiet"
                text: qsTr("Varsayılana döndür")
                onClicked: {
                    const d = [];
                    for (let i = 0; i < 16; i++)
                        d.push(i);
                    dialog.draft = d;
                    dialog.capturing = -1;
                }
            }
            Item { Layout.fillWidth: true }
            PillButton { text: qsTr("Vazgeç"); onClicked: dialog.close() }
            PillButton {
                kind: "primary"
                text: qsTr("Eşlemeyi kaydet")
                enabled: dialog.altered
                onClicked: {
                    const map = {};
                    for (let i = 0; i < 16; i++)
                        if (dialog.draft[i] !== i)
                            map[String(i)] = dialog.draft[i];
                    oviora.setPreference("padButtonMap", map);
                    dialog.close();
                }
            }
        }
    }
}
