// The open conversation of MessageCenter (the context property "messages"): the bubbles, oldest
// first, and the line to write in. Shared by the home's chat popup and the stream menu's
// "Mesajlar" panel; each puts its own title, notes and way back around it.
//
// The bubbles live in a ListModel kept in step with messages.chat by message id, so a new message
// slides in on its own while the rest stay put; a whole new conversation is loaded without motion.
// Bubbles of one sender in a row sit close together, a day line starts each day, my messages carry
// ✓ (on the server) and ✓✓ (seen; in a group: by everyone, else how many), a failed one "Tekrar dene".
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic

ColumnLayout {
    id: chatView
    spacing: 10
    property alias field: chatField
    // Group conversations: the sender's name on top of the first bubble of each run that is not mine
    property bool showSender: messages.chatIsGroup
    function focusField() { chatField.forceActiveFocus() }

    // ── The model, in step with messages.chat ──
    ListModel { id: bubbles }
    property string loadedFor: ""
    // Messages that came while the list was scrolled up (the "↓" button counts them)
    property int unseenBelow: 0

    function dayLabel(ms) {
        const d = new Date(ms), today = new Date()
        const start = t => new Date(t.getFullYear(), t.getMonth(), t.getDate()).getTime()
        const days = Math.round((start(today) - start(d)) / 86400000)
        if (days === 0) return qsTr("Bugün")
        if (days === 1) return qsTr("Dün")
        return d.toLocaleDateString(Qt.locale(), d.getFullYear() === today.getFullYear() ? "d MMMM" : "d MMMM yyyy")
    }
    function rowOf(m, prev, next) {
        const day = new Date(m.at).toDateString()
        const sameRun = (a, b) => !!a && !!b && a.mine === b.mine && (a.from || "") === (b.from || "")
                                  && new Date(a.at).toDateString() === new Date(b.at).toDateString() && Math.abs(b.at - a.at) < 5 * 60000
        return {
            key: m.mid || ("id" + m.id), mine: !!m.mine, text: m.text || "", at: Number(m.at) || 0,
            failed: !!m.failed, sending: !!m.sending, from: m.from || "",
            read: m.read === true, seenBy: m.seenBy !== undefined ? m.seenBy : -1, seenOf: m.seenOf !== undefined ? m.seenOf : 0,
            dayHead: !prev || new Date(prev.at).toDateString() !== day ? dayLabel(m.at) : "",
            first: !sameRun(prev, m), last: !sameRun(m, next)
        }
    }
    function sync() {
        const list = messages.chat || []
        const convo = messages.chatWith + "|" + messages.chatGroup
        const fresh = convo !== loadedFor
        const wasAtEnd = chatList.atYEnd || chatList.count === 0
        let added = 0
        if (fresh) {
            loadedFor = convo
            unseenBelow = 0
            chatList.animateNew = false
            bubbles.clear()
        }
        for (let i = 0; i < list.length; i++) {
            const row = rowOf(list[i], i > 0 ? list[i - 1] : null, i + 1 < list.length ? list[i + 1] : null)
            if (i < bubbles.count && bubbles.get(i).key === row.key) {
                // Only what changed: a bubble whose state moved (sent, seen) keeps its place
                const cur = bubbles.get(i)
                for (const k in row) if (cur[k] !== row[k]) bubbles.setProperty(i, k, row[k])
                continue
            }
            let j = -1
            for (let k = i + 1; k < bubbles.count; k++) if (bubbles.get(k).key === row.key) { j = k; break }
            if (j >= 0) {
                bubbles.move(j, i, 1)
                for (const k in row) if (bubbles.get(i)[k] !== row[k]) bubbles.setProperty(i, k, row[k])
            } else {
                bubbles.insert(i, row)
                added++
            }
        }
        if (bubbles.count > list.length) bubbles.remove(list.length, bubbles.count - list.length)
        if (fresh) {
            chatList.positionViewAtEnd()
            Qt.callLater(() => { chatList.positionViewAtEnd(); chatList.animateNew = true })
        } else if (added > 0) {
            // Mine always brings the list down; another's only when it was at the bottom already
            const mineAdded = list.length > 0 && list[list.length - 1].mine
            if (wasAtEnd || mineAdded) Qt.callLater(chatList.glideToEnd)
            else unseenBelow += added
        }
    }
    Connections {
        target: messages
        function onChatChanged() { chatView.sync() }
        function onTypingChanged() { if (chatList.atYEnd) Qt.callLater(chatList.glideToEnd) }
    }
    Component.onCompleted: sync()

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        ListView {
            id: chatList
            anchors.fill: parent
            clip: true
            spacing: 0
            model: bubbles
            boundsBehavior: Flickable.StopAtBounds
            property bool animateNew: false
            ScrollBar.vertical: VScrollBar { }
            // Room for the scroll bar beside the bubbles
            readonly property real lane: width - 12
            onAtYEndChanged: if (atYEnd) chatView.unseenBelow = 0
            onHeightChanged: if (atYEnd || count === 0) positionViewAtEnd()
            // Down to the newest message in a short glide, not a jump
            function glideToEnd() {
                const from = contentY
                positionViewAtEnd()
                const to = contentY
                if (Math.abs(to - from) < 2) return
                contentY = from
                glide.from = from
                glide.to = to
                glide.restart()
            }
            NumberAnimation { id: glide; target: chatList; property: "contentY"; duration: 260; easing.type: Easing.OutCubic }
            displaced: Transition { NumberAnimation { properties: "y"; duration: 220; easing.type: Easing.OutCubic } }

            delegate: Item {
                id: row
                required property int index
                required property bool mine
                required property string text
                required property double at
                required property bool failed
                required property bool sending
                required property string from
                required property bool read
                required property int seenBy
                required property int seenOf
                required property string dayHead
                required property bool first
                required property bool last
                required property string key
                width: chatList.lane
                implicitHeight: (dayHead.length ? dayLine.height + 10 : 0) + bubble.height + (mine && failed ? 24 : 0) + (last ? 10 : 3)
                // A new bubble: in from its own side, rising a little, fading in
                property real enter: 1
                Component.onCompleted: if (chatList.animateNew) { enter = 0; enterAnim.start() }
                NumberAnimation { id: enterAnim; target: row; property: "enter"; to: 1; duration: 320; easing.type: Easing.OutCubic }

                // ── The day line ──
                Item {
                    id: dayLine
                    visible: row.dayHead.length > 0
                    width: parent.width
                    height: visible ? 26 : 0
                    Rectangle {
                        anchors.centerIn: parent
                        width: dayText.implicitWidth + 22
                        height: 22
                        radius: 11
                        color: Tema.surface2
                        Text {
                            id: dayText
                            textFormat: Text.PlainText
                            anchors.centerIn: parent
                            text: row.dayHead
                            font.family: Tema.body
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: Tema.muted
                        }
                    }
                }

                Rectangle {
                    id: bubble
                    y: (row.dayHead.length ? dayLine.height + 10 : 0) + (1 - row.enter) * 10
                    x: (row.mine ? parent.width - width : 0) + (1 - row.enter) * (row.mine ? 18 : -18)
                    opacity: row.enter
                    width: Math.min(chatList.lane * 0.8, Math.max(bubbleText.implicitWidth, metaRow.implicitWidth,
                                                                senderName.visible ? senderName.implicitWidth : 0) + 26)
                    height: bubbleText.y + bubbleText.implicitHeight + metaRow.implicitHeight + 12
                    // Rounder outside a run, tighter where bubbles of one run meet
                    readonly property real big: 18
                    readonly property real small: 6
                    topLeftRadius: !row.mine && !row.first ? small : big
                    bottomLeftRadius: !row.mine && !row.last ? small : big
                    topRightRadius: row.mine && !row.first ? small : big
                    bottomRightRadius: row.mine && !row.last ? small : big
                    color: row.mine ? (row.failed ? Qt.darker(Tema.accent, 1.6) : Tema.accent) : Tema.surface2
                    Behavior on color { ColorAnimation { duration: 200 } }
                    Text {
                        id: senderName
                        textFormat: Text.PlainText
                        visible: chatView.showSender && !row.mine && row.first && row.from.length > 0
                        x: 13
                        y: 8
                        width: Math.min(chatList.lane * 0.8 - 26, implicitWidth)
                        text: row.from
                        elide: Text.ElideRight
                        font.family: Tema.body
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        color: Tema.accentText
                    }
                    Text {
                        id: bubbleText
                        textFormat: Text.PlainText
                        x: 13
                        y: senderName.visible ? senderName.y + senderName.implicitHeight + 2 : 9
                        width: Math.min(chatList.lane * 0.8 - 26, implicitWidth)
                        wrapMode: Text.Wrap
                        text: row.text
                        font.family: Tema.body
                        font.pixelSize: 15
                        lineHeight: 1.08
                        color: row.mine ? Tema.inkOnAccent : (row.failed ? Tema.warm : Tema.text)
                    }
                    // Time, and on mine: ✓ sent, ✓✓ seen, the clock while sending
                    Row {
                        id: metaRow
                        anchors.right: parent.right
                        anchors.rightMargin: 11
                        anchors.top: bubbleText.bottom
                        anchors.topMargin: 1
                        spacing: 5
                        Text {
                            textFormat: Text.PlainText
                            text: Qt.formatDateTime(new Date(row.at), "HH:mm")
                            font.family: Tema.body
                            font.pixelSize: 11
                            color: row.mine ? Tema.inkOnAccent : Tema.faint
                            opacity: 0.75
                        }
                        Text {
                            id: tick
                            visible: row.mine && !row.failed
                            textFormat: Text.PlainText
                            readonly property bool allSeen: row.read || (row.seenOf > 0 && row.seenBy >= row.seenOf)
                            text: row.sending ? "◷" : allSeen ? "✓✓" : row.seenBy > 0 ? "✓ " + row.seenBy : "✓"
                            font.family: Tema.body
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            color: allSeen ? "#9FE3FF" : Tema.inkOnAccent
                            opacity: row.sending ? 0.55 : 0.9
                            Behavior on color { ColorAnimation { duration: 300 } }
                            // A tick that changes pops once
                            scale: 1
                            onTextChanged: if (!row.sending) tickPop.restart()
                            SequentialAnimation {
                                id: tickPop
                                NumberAnimation { target: tick; property: "scale"; to: 1.35; duration: 110; easing.type: Easing.OutCubic }
                                NumberAnimation { target: tick; property: "scale"; to: 1; duration: 180; easing.type: Easing.OutBack }
                            }
                        }
                    }
                    HoverHandler { id: bubbleHover }
                    ToolTip.visible: bubbleHover.hovered && row.mine && row.seenOf > 0 && row.seenBy >= 0
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("%1 / %2 kişi gördü").arg(row.seenBy).arg(row.seenOf)
                }
                // A message of mine that did not go: the reason and a way to try again
                AbstractButton {
                    id: retry
                    visible: row.mine && row.failed
                    anchors.right: bubble.right
                    anchors.top: bubble.bottom
                    anchors.topMargin: 2
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    implicitHeight: 20
                    implicitWidth: retryText.implicitWidth
                    onClicked: messages.resend(row.key)
                    contentItem: Text {
                        id: retryText
                        textFormat: Text.PlainText
                        text: qsTr("Gönderilemedi · Tekrar dene")
                        font.family: Tema.body
                        font.pixelSize: 12
                        font.underline: retry.hovered
                        color: Tema.danger
                    }
                }
            }

            // ── "yazıyor…": three dots rising one after another, under the newest message ──
            footer: Item {
                width: chatList.lane
                readonly property var who: messages.chatTyping || []
                readonly property bool on: who.length > 0
                height: on ? typingBubble.height + 12 : 0
                Behavior on height { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                clip: true
                Rectangle {
                    id: typingBubble
                    y: 4
                    width: typingRow.implicitWidth + 26
                    height: 34
                    radius: 17
                    color: Tema.surface2
                    opacity: parent.on ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 180 } }
                    Row {
                        id: typingRow
                        anchors.centerIn: parent
                        spacing: 8
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 4
                            Repeater {
                                model: 3
                                Rectangle {
                                    id: dot
                                    required property int index
                                    width: 7
                                    height: 7
                                    radius: 3.5
                                    color: Tema.muted
                                    property real lift: 0
                                    transform: Translate { y: -dot.lift }
                                    SequentialAnimation on lift {
                                        running: typingBubble.opacity > 0
                                        loops: Animation.Infinite
                                        PauseAnimation { duration: dot.index * 140 }
                                        NumberAnimation { to: 4; duration: 220; easing.type: Easing.OutQuad }
                                        NumberAnimation { to: 0; duration: 220; easing.type: Easing.InQuad }
                                        PauseAnimation { duration: (2 - dot.index) * 140 + 260 }
                                    }
                                }
                            }
                        }
                        Text {
                            textFormat: Text.PlainText
                            anchors.verticalCenter: parent.verticalCenter
                            readonly property var who: messages.chatTyping || []
                            visible: messages.chatIsGroup
                            text: who.length === 1 ? qsTr("%1 yazıyor").arg(who[0])
                                : who.length > 1 ? qsTr("%1 kişi yazıyor").arg(who.length) : ""
                            font.family: Tema.body
                            font.pixelSize: 12
                            color: Tema.muted
                        }
                    }
                }
            }
        }

        // Nothing yet
        Column {
            anchors.centerIn: parent
            width: parent.width - 40
            spacing: 6
            visible: bubbles.count === 0
            opacity: visible ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 200 } }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "💬"
                font.pixelSize: 30
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("Henüz mesaj yok. İlk mesajı sen yaz.")
                font.family: Tema.body
                font.pixelSize: 15
                color: Tema.muted
            }
        }

        // ── Scrolled up: a round button back down, with the count of what came meanwhile ──
        AbstractButton {
            id: toEnd
            readonly property bool wanted: !chatList.atYEnd && chatList.contentHeight > chatList.height + 40
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.bottom: parent.bottom
            anchors.bottomMargin: wanted ? 10 : -50
            Behavior on anchors.bottomMargin { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            opacity: wanted ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 180 } }
            visible: opacity > 0
            width: 40
            height: 40
            hoverEnabled: true
            focusPolicy: Qt.NoFocus
            onClicked: chatList.glideToEnd()
            background: Rectangle {
                radius: 20
                color: toEnd.hovered ? Tema.lineStrong : Tema.surface2
                border.color: Tema.line
            }
            contentItem: Text {
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "↓"
                font.pixelSize: 18
                color: Tema.text
            }
            Rectangle {
                visible: chatView.unseenBelow > 0
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: -4
                width: Math.max(20, unseenText.implicitWidth + 10)
                height: 20
                radius: 10
                color: Tema.accent
                Text {
                    id: unseenText
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    text: String(chatView.unseenBelow)
                    font.family: Tema.body
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: Tema.inkOnAccent
                }
            }
        }
    }

    // ── The line to write in: grows to five lines; Enter sends, Shift+Enter starts a new line ──
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.min(chatField.implicitHeight, 5 * 21 + 20)
            Behavior on implicitHeight { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
            radius: Tema.radiusInput
            color: Tema.surface2
            border.width: chatField.activeFocus ? 2 : 1
            border.color: chatField.activeFocus ? Tema.accent : Tema.line
            Behavior on border.color { ColorAnimation { duration: 150 } }
            Flickable {
                id: fieldFlick
                anchors.fill: parent
                contentWidth: width
                contentHeight: chatField.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                function keep(r) {
                    if (r.y < contentY) contentY = r.y
                    else if (r.y + r.height > contentY + height) contentY = r.y + r.height - height + chatField.bottomPadding
                }
                // Basic style, as the home's fields: Material's floating label would sit on the border
                Basic.TextArea {
                    id: chatField
                    width: fieldFlick.width
                    placeholderText: messages.ready ? qsTr("Mesaj yaz") : qsTr("Mesajlaşma hazırlanıyor…")
                    enabled: messages.ready
                    wrapMode: TextEdit.Wrap
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.text
                    placeholderTextColor: Tema.faint
                    selectionColor: Tema.accent
                    selectedTextColor: Tema.inkOnAccent
                    leftPadding: 16
                    rightPadding: 16
                    topPadding: 10
                    bottomPadding: 10
                    background: null
                    onCursorRectangleChanged: fieldFlick.keep(cursorRectangle)
                    onTextChanged: {
                        if (text.length > 2000) remove(2000, text.length)
                        if (text.trim().length > 0) messages.typing()
                        else messages.stopTyping()
                    }
                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier)
                                && !(event.modifiers & Qt.ControlModifier)) {
                            sendButton.clicked()
                            event.accepted = true
                        }
                    }
                }
            }
        }
        // A round send button: comes alive with text, sinks in when pressed
        AbstractButton {
            id: sendButton
            Layout.alignment: Qt.AlignBottom
            enabled: messages.ready && chatField.text.trim().length > 0
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            implicitWidth: 42
            implicitHeight: 42
            scale: down ? 0.9 : (enabled ? 1 : 0.92)
            Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutBack } }
            onClicked: {
                if (!enabled) return
                messages.send(chatField.text)
                chatField.text = ""
            }
            background: Rectangle {
                radius: 21
                color: !sendButton.enabled ? Tema.surface2 : sendButton.down ? Tema.accentDown : (sendButton.hovered ? Tema.accentHover : Tema.accent)
                Behavior on color { ColorAnimation { duration: 160 } }
            }
            contentItem: Text {
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "➤"
                font.pixelSize: 17
                color: sendButton.enabled ? Tema.inkOnAccent : Tema.faint
                rotation: sendButton.enabled ? 0 : -30
                Behavior on rotation { NumberAnimation { duration: 200; easing.type: Easing.OutBack } }
            }
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: qsTr("Gönder (Enter) · Yeni satır: Shift+Enter")
        }
    }
}
