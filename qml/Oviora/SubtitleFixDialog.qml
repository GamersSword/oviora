// Old Oviora's "Çeviriyi düzelt" (Ctrl+E or the stream menu's "Düzelt"; old StreamView correction
// dialog + CorrectionPanel.qml): the subtitle line on screen, a field for a better Turkish, and what
// others suggested with votes (the Oviora server's shared translation memory, `subtitles`).
// Floats over the picture: cut out of the native video window (videoCover).
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import "ayar" as S

Rectangle {
    id: dialog
    readonly property bool videoCover: visible
    readonly property real coverRadius: Tema.radiusPanel
    visible: false
    width: Math.min(620, (parent ? parent.width : 620) - 48)
    height: Math.min((parent ? parent.height : 560) - 48, column.implicitHeight + 56)
    x: Math.round(((parent ? parent.width : width) - width) / 2)
    y: Math.round(((parent ? parent.height : height) - height) / 2)
    radius: Tema.radiusPanel
    color: Tema.surface
    border.color: Tema.line
    border.width: 1
    // Keeps clicks and the wheel off the picture below
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => wheel.accepted = true }

    property string english: ""
    property string current: ""
    property var variants: []
    property double shown: 0
    property string error: ""
    property bool loading: false

    function start(en, tr) {
        english = en
        current = tr
        variants = []
        error = ""
        loading = true
        fixField.text = tr
        visible = true
        fixField.forceActiveFocus()
        subtitles.loadVariants(en)
    }
    function close() {
        visible = false
        const w = dialog.Window.window
        if (w && w.contentItem) w.contentItem.forceActiveFocus()
    }
    function send() {
        const text = fixField.text.trim()
        if (!text.length || text === current) {
            close()
            return
        }
        loading = true
        subtitles.correct(english, text)
        current = text
    }

    Connections {
        target: subtitles
        function onVariantsLoaded(en, shownId, list, err) {
            if (en !== dialog.english)
                return
            dialog.loading = false
            dialog.error = err
            if (!err.length) {
                dialog.variants = list
                dialog.shown = shownId
            }
        }
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
                text: qsTr("Çeviriyi düzelt")
                font.family: Tema.display
                font.pixelSize: 24
                font.weight: Font.DemiBold
                font.letterSpacing: -0.3
                color: Tema.text
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Düzeltmen Oviora'da bu satırı görecek herkese önerilir. Enter ile gönder, Esc ile kapat.")
                font.family: Tema.body
                font.pixelSize: 13
                color: Tema.muted
            }
        }
        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: dialog.english
            font.family: Tema.body
            font.pixelSize: 15
            font.italic: true
            color: Tema.muted
        }
        S.InputField {
            id: fixField
            Layout.fillWidth: true
            implicitHeight: 50
            font.pixelSize: 17
            maximumLength: 600
            placeholderText: qsTr("Daha iyi Türkçesi")
            Keys.onReturnPressed: dialog.send()
            Keys.onEnterPressed: dialog.send()
            Keys.onEscapePressed: dialog.close()
        }
        Label {
            textFormat: Text.PlainText
            visible: dialog.visible && !subtitles.sharedMemoryActive()
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Ortak çeviri hafızası kapalı ya da hesaba giriş yapılmamış; düzeltme yalnız bu bilgisayarda kalır.")
            font.family: Tema.body
            font.pixelSize: 13
            color: Tema.warm
        }
        Label {
            textFormat: Text.PlainText
            visible: dialog.error.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: dialog.error
            font.family: Tema.body
            font.pixelSize: 13
            color: Tema.warm
        }

        // Others' suggestions for this line, best first; the one in use is marked
        Label {
            textFormat: Text.PlainText
            visible: dialog.variants.length > 0
            Layout.topMargin: 4
            text: qsTr("Önerilen çeviriler")
            font.family: Tema.body
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: Tema.faint
        }
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 220)
            visible: dialog.variants.length > 0
            clip: true
            spacing: 6
            model: dialog.variants
            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                spacing: 8
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: modelData.tr
                    font.family: Tema.body
                    font.pixelSize: 15
                    font.weight: modelData.id === dialog.shown ? Font.DemiBold : Font.Normal
                    color: modelData.id === dialog.shown ? Tema.text : Tema.muted
                }
                Label {
                    textFormat: Text.PlainText
                    text: (modelData.score > 0 ? "+" : "") + modelData.score
                    font.family: Tema.body
                    font.pixelSize: 14
                    color: Tema.faint
                }
                S.PillButton {
                    visible: !modelData.by_me
                    kind: modelData.mine === 1 ? "primary" : "quiet"
                    navUpDown: false
                    focusPolicy: Qt.NoFocus
                    text: "▲"
                    onClicked: subtitles.vote(modelData.id, modelData.mine === 1 ? 0 : 1, dialog.english)
                }
                S.PillButton {
                    visible: !modelData.by_me
                    kind: modelData.mine === -1 ? "primary" : "quiet"
                    navUpDown: false
                    focusPolicy: Qt.NoFocus
                    text: "▼"
                    onClicked: subtitles.vote(modelData.id, modelData.mine === -1 ? 0 : -1, dialog.english)
                }
                Label {
                    textFormat: Text.PlainText
                    visible: !!modelData.by_me
                    text: qsTr("senin")
                    font.family: Tema.body
                    font.pixelSize: 13
                    color: Tema.faint
                }
            }
        }

        Row {
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: 4
            spacing: 10
            S.PillButton {
                kind: "quiet"
                navUpDown: false
                focusPolicy: Qt.NoFocus
                text: qsTr("Kapat")
                onClicked: dialog.close()
            }
            S.PillButton {
                kind: "primary"
                navUpDown: false
                focusPolicy: Qt.NoFocus
                enabled: !dialog.loading && fixField.text.trim().length > 0 && fixField.text.trim() !== dialog.current
                text: qsTr("Düzelt")
                onClicked: dialog.send()
            }
        }
    }
}
