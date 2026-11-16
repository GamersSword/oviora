import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Controls.Basic as Basic


// Home of a platform package: nothing works before signing in. Then a greeting, the PS5s this
// account can reach as plain rows under two quiet section labels ("PSN üzerinden", "Bu ağda"), and
// friends with sharing in a quiet column on the right. Colours and faces come from Tema.
Pane {
    id: home
    padding: 0

    // Oviora (new): what the old main window did for this page
    signal requestSettings()
    signal requestManualHost()
    signal connectionStarted(var retryFn)
    function showConfirmDialog(title, text, action) {
        confirmLoader.active = true;
        if (confirmLoader.item) confirmLoader.item.popup.ask(title, text, action);
    }
    function showInfoDialog(title, text) {
        confirmLoader.active = true;
        if (confirmLoader.item) confirmLoader.item.popup.tell(title, text);
    }
    // --dialog update (test runs): the update screen with these notes, without an update
    property string updatePreview: ""
    function showToast(title, text) { toast.show(title, text) }
    function showPsnLinkDialog() {
        psnLinkLoader.active = true;
        if (psnLinkLoader.item) psnLinkLoader.item.popup.start();
    }
    // End-to-end encrypted messages (MessageCenter, the context property "messages")
    readonly property var msgs: messages

    readonly property var server: platform
    readonly property bool streamerMode: oviora.preferences.streamerMode === true
    // --test-register: the register form even when signed in (screenshots of the form)
    readonly property bool testRegister: (Qt.application.arguments || []).indexOf("--test-register") >= 0
    readonly property bool signedIn: server.status == "active" && !testRegister
    property bool registering: testRegister
    property bool waiting: false
    // Invite code at sign-up / sign-in: shown on request, checked as it is typed
    property bool showInvite: false
    property string inviteFrom: ""
    property string inviteError: ""

    // "12 Kas 21:30" for a unix time (seconds)
    // "bugün 21:30", "dün 23:44", "13 Kas 10:11" (Turkish month names whatever the system language)
    function shortDate(sec) {
        const d = new Date(sec * 1000);
        const tr = Qt.locale("tr_TR");
        const today = new Date();
        today.setHours(0, 0, 0, 0);
        const day = new Date(d);
        day.setHours(0, 0, 0, 0);
        const days = Math.round((today - day) / 86400000);
        const time = tr.toString(d, "HH:mm");
        if (days === 0)
            return qsTr("bugün %1").arg(time);
        if (days === 1)
            return qsTr("dün %1").arg(time);
        return tr.toString(d, days < 300 ? "d MMM HH:mm" : "d MMM yyyy HH:mm");
    }
    // "1 sa 20 dk", "45 dk", "1 dk"
    function lengthText(sec) {
        const m = Math.max(1, Math.round(sec / 60));
        const h = Math.floor(m / 60);
        if (h > 0)
            return m % 60 ? qsTr("%1 sa %2 dk").arg(h).arg(m % 60) : qsTr("%1 sa").arg(h);
        return qsTr("%1 dk").arg(m);
    }

    background: Rectangle { color: Tema.bg }

    // Üstte açık pencere kalmasın: başka bir sayfa (Ayarlar vb.) ana ekranın üstüne gelince kapanır
    function closePopups() {
        if (notifLoader.item) notifLoader.item.popup.close();
        if (historyLoader.item) historyLoader.item.popup.close();
    }
    StackView.onDeactivating: closePopups()
    // Güncelleme ekranı (bu sayfanın içinde, açılır pencerelerin altında kalan bir katman) belirince
    Connections {
        target: home.server
        function onUpdateChanged() {
            if (home.server.updateRequired)
                home.closePopups();
        }
    }

    StackView.onActivated: {
        forceActiveFocus(Qt.TabFocusReason);
        if (signedIn)
            server.refresh();
    }

    Connections {
        target: home.server
        function onChanged() { home.waiting = false; }
        function onMessage() { home.waiting = false; }
    }

    function submit() {
        if (!userField.text.trim().length || !passField.text.length)
            return;
        if (registering && !termsBox.checked) {
            home.showInfoDialog(qsTr("Kayıt olunamadı"), qsTr("Hesap açmak için Kullanım Koşulları'nı kabul etmelisin."));
            return;
        }        if (registering && passField.text !== pass2Field.text) {
            home.showInfoDialog(qsTr("Kayıt olunamadı"), qsTr("Şifreler aynı değil."));
            return;
        }
        waiting = true;
        const invite = showInvite ? inviteField.text : "";
        if (registering)
            server.registerUser(userField.text, passField.text, invite);
        else
            server.login(userField.text, passField.text, invite);
    }

    // PS5s found on this PC's network (the PSN ones of this PC's own login are left out)
    readonly property int localCount: {
        let n = 0;
        for (const h of (oviora.lanConsoles || []))
            if (h && h.display && (!h.duid || h.discovered))
                n++;
        return n;
    }

    // My console with this duid, from the account view
    function myConsole(duid) {
        for (const c of (server.consoles || []))
            if (c && c.duid == duid)
                return c;
        return null;
    }

    // Two letters for a name's circle
    function initials(name) {
        const s = (name || "").trim();
        return s.length ? s.substring(0, Math.min(2, s.length)).toLocaleUpperCase(Qt.locale("tr_TR")) : "?";
    }

    // ── Pieces ───────────────────────────────────────────────────────────
    // kind: "primary" (purple fill, the main action of a row), "secondary" (outlined), "quiet" (text only)
    component TextButton: AbstractButton {
        id: tb
        property string kind: "secondary"
        property bool large: false
        property color tone: kind === "quiet" ? Tema.muted : Tema.text
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        implicitHeight: large ? 52 : 40
        implicitWidth: label.implicitWidth + (kind === "quiet" ? 20 : (large ? 56 : 36))
        background: Rectangle {
            radius: height / 2
            opacity: tb.enabled ? 1 : 0.45
            color: {
                if (tb.kind === "primary")
                    return tb.down ? Tema.accentDown : (tb.hovered ? Tema.accentHover : Tema.accent);
                return tb.down ? Tema.line : (tb.hovered ? Tema.surface2 : "transparent");
            }
            border.width: tb.kind === "secondary" ? 1 : 0
            border.color: tb.hovered ? Tema.lineStrong : Tema.line
        }
        contentItem: Label {
            textFormat: Text.PlainText
            id: label
            text: tb.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font.family: Tema.body
            font.pixelSize: tb.large ? 17 : 15
            font.weight: tb.kind === "quiet" ? Font.Medium : Font.DemiBold
            color: tb.kind === "primary" ? Tema.inkOnAccent : (tb.kind === "quiet" && tb.hovered ? Tema.text : tb.tone)
            opacity: tb.enabled ? 1 : 0.55
        }
    }

    // Basic style: Material's floating label would sit on the border
    component Field: Basic.TextField {
        Layout.fillWidth: true
        font.family: Tema.body
        font.pixelSize: 17
        color: Tema.text
        placeholderTextColor: Tema.faint
        selectionColor: Tema.accent
        selectedTextColor: Tema.inkOnAccent
        leftPadding: 16
        rightPadding: 16
        topPadding: 13
        bottomPadding: 13
        background: Rectangle {
            radius: Tema.radiusInput
            color: Tema.surface2
            border.width: parent.activeFocus ? 2 : 1
            border.color: parent.activeFocus ? Tema.accent : Tema.line
        }
    }

    // A quiet section label with a small "?" that explains it on hover
    component SectionLabel: RowLayout {
        id: sl
        property string text
        property string info
        spacing: 8
        Label {
            textFormat: Text.PlainText
            text: sl.text
            font.family: Tema.body
            font.pixelSize: Tema.labelSize
            font.weight: Font.DemiBold
            color: Tema.muted
        }
        Rectangle {
            visible: sl.info.length > 0
            implicitWidth: 18
            implicitHeight: 18
            radius: 9
            color: infoHover.hovered ? Tema.surface2 : "transparent"
            border.color: infoHover.hovered ? Tema.lineStrong : Tema.line
            Label {
                textFormat: Text.PlainText
                anchors.centerIn: parent
                text: "?"
                font.family: Tema.body
                font.pixelSize: 11
                font.weight: Font.DemiBold
                color: Tema.muted
            }
            HoverHandler { id: infoHover }
            ToolTip {
                visible: infoHover.hovered
                delay: 150
                x: 26
                y: -6
                width: Math.min(360, infoText.implicitWidth + 28)
                padding: 12
                contentItem: Text {
                    textFormat: Text.PlainText
                    id: infoText
                    text: sl.info
                    wrapMode: Text.WordWrap
                    font.family: Tema.body
                    font.pixelSize: 14
                    lineHeight: 1.25
                    color: Tema.text
                }
                background: Rectangle {
                    radius: 10
                    color: Tema.surface2
                    border.color: Tema.lineStrong
                }
            }
        }
    }

    // State line under a console's name: a small light and a few words
    component StateLine: RowLayout {
        id: st
        property color tone: Tema.muted
        property string text
        spacing: 8
        Rectangle {
            implicitWidth: 7
            implicitHeight: 7
            radius: 3.5
            color: st.tone
        }
        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: st.text
            elide: Text.ElideRight
            font.family: Tema.body
            font.pixelSize: Tema.smallSize
            color: Tema.muted
        }
    }

    // One console: glyph in its state colour, name and state, actions at the end
    component ConsoleRow: Item {
        id: cr
        property color tone: Tema.muted
        property string name
        property string stateText
        default property alias actions: actionRow.data
        Layout.fillWidth: true
        implicitHeight: 78

        KonsolSimge {
            id: crGlyph
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 64
            height: 18
            tone: cr.tone
        }
        ColumnLayout {
            anchors {
                left: crGlyph.right
                right: actionRow.left
                verticalCenter: parent.verticalCenter
                leftMargin: 18
                rightMargin: 16
            }
            spacing: 3
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: cr.name
                elide: Text.ElideRight
                font.family: Tema.body
                font.pixelSize: 18
                font.weight: Font.DemiBold
                color: Tema.text
            }
            StateLine {
                Layout.fillWidth: true
                tone: cr.tone
                text: cr.stateText
            }
        }
        RowLayout {
            id: actionRow
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
        }
        // rows are separated by hairlines; the first one also parts the list from its label
        Rectangle {
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
            }
            height: 1
            color: Tema.line
        }
    }

    // A plain sentence where a list is empty
    component EmptyNote: Label {
        Layout.fillWidth: true
        Layout.topMargin: 10
        Layout.bottomMargin: 6
        wrapMode: Text.WordWrap
        font.family: Tema.body
        font.pixelSize: 15
        lineHeight: 1.3
        color: Tema.muted
    }

    // ── Top bar ──────────────────────────────────────────────────────────
    RowLayout {
        id: topBar
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            leftMargin: 56
            rightMargin: 40
            topMargin: 26
        }
        height: 48
        spacing: 10

        OvioraMark {
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
        }
        Label {
            textFormat: Text.PlainText
            text: "Oviora"
            font.family: Tema.display
            font.pixelSize: 24
            font.weight: Font.DemiBold
            font.letterSpacing: -0.5
            color: Tema.text
        }
        Item { Layout.fillWidth: true }
        // Notifications: recent events, a count of the unseen ones
        TextButton {
            id: bellButton
            visible: home.signedIn
            kind: "quiet"
            text: qsTr("Bildirimler")
            onClicked: {
                if (notifLoader.item && notifLoader.item.popup.opened)
                    notifLoader.item.popup.close();
                else {
                    if (historyLoader.item) historyLoader.item.popup.close(); // iki liste üst üste binmesin
                    notifLoader.active = true;
                    if (notifLoader.item) notifLoader.item.popup.open();
                    home.server.markNotificationsRead();
                }
            }
            Rectangle {
                visible: home.server.unreadCount > 0
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.rightMargin: -10
                anchors.topMargin: -2
                width: Math.max(18, badge.implicitWidth + 8)
                height: 18
                radius: 9
                color: Tema.accent
                Label {
                    textFormat: Text.PlainText
                    id: badge
                    anchors.centerIn: parent
                    text: home.server.unreadCount > 9 ? "9+" : home.server.unreadCount
                    font.family: Tema.body
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: Tema.inkOnAccent
                }
            }
            // Testing: YAKAMOZ_TEST_BILDIRIMLER opens the list
            Timer {
                interval: 20000
                running: "".length > 0
                onTriggered: {
                    notifLoader.active = true;
                    if (notifLoader.item) notifLoader.item.popup.open();
                }
            }
            Loader {
                id: notifLoader
                anchors.fill: parent
                active: false
                sourceComponent: Component {
                    Item {
                        property alias popup: notifPopup
                        Popup {
                            id: notifPopup
                            // İki liste üst üste binmesin; güncelleme ekranının üstünde de açılmasın
                            onAboutToShow: if (historyLoader.item) historyLoader.item.popup.close()
                onOpened: if (home.server.updateRequired) close()
                y: parent.height + 8
                x: parent.width - width
                width: 380
                padding: 16
                background: Rectangle {
                    radius: Tema.radiusPanel
                    color: Tema.surface
                    border.color: Tema.lineStrong
                }
                contentItem: ColumnLayout {
                    spacing: 0
                    Label {
                        textFormat: Text.PlainText
                        Layout.bottomMargin: 8
                        text: qsTr("Bildirimler")
                        font.family: Tema.body
                        font.pixelSize: Tema.labelSize
                        font.weight: Font.DemiBold
                        color: Tema.muted
                    }
                    Label {
                        textFormat: Text.PlainText
                        visible: home.server.notifications.length == 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Henüz bildirim yok.")
                        font.family: Tema.body
                        font.pixelSize: 15
                        color: Tema.muted
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, 420)
                        visible: count > 0
                        clip: true
                        model: home.server.notifications
                        delegate: ColumnLayout {
                            required property var modelData
                            width: ListView.view.width
                            spacing: 2
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Tema.line }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 8
                                Label {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: modelData.title
                                    elide: Text.ElideRight
                                    font.family: Tema.body
                                    font.pixelSize: 15
                                    font.weight: Font.DemiBold
                                    color: Tema.text
                                }
                                Label {
                                    textFormat: Text.PlainText
                                    text: modelData.time
                                    font.family: Tema.body
                                    font.pixelSize: 12
                                    color: Tema.faint
                                }
                            }
                            Label {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                Layout.bottomMargin: 8
                                wrapMode: Text.WordWrap
                                text: modelData.text
                                font.family: Tema.body
                                font.pixelSize: 14
                                color: Tema.muted
                            }
                        }
                    }
                }
            }
                    }
                }
            }
        }
        // "Oturum geçmişim": who played where and how long
        TextButton {
            id: historyButton
            visible: home.signedIn
            kind: "quiet"
            text: qsTr("Geçmiş")
            onClicked: {
                if (historyLoader.item && historyLoader.item.popup.opened)
                    historyLoader.item.popup.close();
                else {
                    if (notifLoader.item) notifLoader.item.popup.close(); // iki liste üst üste binmesin
                    home.server.loadHistory(false);
                    historyLoader.active = true;
                    if (historyLoader.item) historyLoader.item.popup.open();
                }
            }
            // Testing: YAKAMOZ_TEST_GECMIS opens the list
            Timer {
                interval: 9000
                running: "".length > 0
                onTriggered: historyButton.clicked()
            }
            Loader {
                id: historyLoader
                anchors.fill: parent
                active: false
                sourceComponent: Component {
                    Item {
                        property alias popup: historyPopup
                        Popup {
                            id: historyPopup
                            onAboutToShow: if (notifLoader.item) notifLoader.item.popup.close()
                onOpened: if (home.server.updateRequired) close()
                y: parent.height + 8
                x: parent.width - width
                width: 420
                padding: 16
                background: Rectangle {
                    radius: Tema.radiusPanel
                    color: Tema.surface
                    border.color: Tema.lineStrong
                }
                contentItem: ColumnLayout {
                    spacing: 0
                    Label {
                        textFormat: Text.PlainText
                        text: qsTr("Oturum geçmişim")
                        font.family: Tema.body
                        font.pixelSize: Tema.labelSize
                        font.weight: Font.DemiBold
                        color: Tema.muted
                    }
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        Layout.bottomMargin: 8
                        wrapMode: Text.WordWrap
                        text: qsTr("Arkadaşlarının PS5'lerinde oynadıkların ve senin PS5'inde oynayanlar, son 180 gün.")
                        font.family: Tema.body
                        font.pixelSize: 13
                        color: Tema.faint
                    }
                    Label {
                        textFormat: Text.PlainText
                        visible: !home.server.historyLoading && home.server.history.length == 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Henüz bir oturum yok.")
                        font.family: Tema.body
                        font.pixelSize: 15
                        color: Tema.muted
                    }
                    ListView {
                        id: historyList
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, 440)
                        visible: count > 0
                        clip: true
                        model: home.server.history
                        // The next page comes when the end of the list is reached
                        onAtYEndChanged: if (atYEnd && home.server.historyMore) home.server.loadHistory(true)
                        delegate: ColumnLayout {
                            required property var modelData
                            width: ListView.view.width
                            spacing: 2
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Tema.line }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 8
                                Label {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: modelData.role === "guest" ? qsTr("%1'in PS5'inde oynadın").arg(modelData.other)
                                        : modelData.role === "host" ? qsTr("%1 senin PS5'inde oynadı").arg(modelData.other)
                                        : qsTr("Kendi PS5'inde oynadın")
                                    font.family: Tema.body
                                    font.pixelSize: 15
                                    font.weight: Font.DemiBold
                                    color: Tema.text
                                }
                                Label {
                                    textFormat: Text.PlainText
                                    text: modelData.live ? qsTr("şu an sürüyor") : qsTr("%1 oynandı").arg(home.lengthText(modelData.length))
                                    font.family: Tema.body
                                    font.pixelSize: 14
                                    color: modelData.live ? Tema.ok : Tema.muted
                                }
                            }
                            Label {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                Layout.bottomMargin: 8
                                elide: Text.ElideRight
                                text: (home.streamerMode ? qsTr("PS5") : modelData.console) + " · " + home.shortDate(modelData.start)
                                font.family: Tema.body
                                font.pixelSize: 13
                                color: Tema.faint
                            }
                        }
                    }
                    Label {
                        textFormat: Text.PlainText
                        visible: home.server.historyLoading
                        Layout.topMargin: 8
                        text: qsTr("Yükleniyor…")
                        font.family: Tema.body
                        font.pixelSize: 13
                        color: Tema.faint
                    }
                }
            }
                    }
                }
            }
        }
        TextButton {
            text: qsTr("Ayarlar")
            kind: "quiet"
            visible: home.signedIn
            onClicked: home.requestSettings()
        }
        TextButton {
            text: qsTr("Kapat")
            kind: "quiet"
            onClicked: Qt.quit()
        }
        // Who is signed in: the first letter in the brand colour
        Rectangle {
            visible: home.signedIn
            Layout.leftMargin: 10
            implicitWidth: 34
            implicitHeight: 34
            radius: 17
            color: Tema.accent
            Label {
                textFormat: Text.PlainText
                anchors.centerIn: parent
                text: home.streamerMode ? "•" : home.initials(home.server.user).substring(0, 1)
                font.family: Tema.body
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Tema.inkOnAccent
            }
        }
    }

    // ── Messages: one conversation at a time, end-to-end encrypted ──
    function openChat(name) {
        msgs.openChat(name);
        chatLoader.active = true;
        if (chatLoader.item) chatLoader.item.popup.open();
    }
    // Testing: --test-chat <friend> opens that conversation (only opens it)
    Timer {
        interval: 9000
        running: typeof vyTest !== "undefined" && !!vyTest.chat
        onTriggered: home.openChat(vyTest.chat)
    }
    Loader {
        id: chatLoader
        active: false
        sourceComponent: Component {
            Item {
                property alias popup: chatPopup
                Popup {
                    id: chatPopup
        parent: Overlay.overlay
        x: Math.round((home.Window.width - width) / 2)
        y: Math.round((home.Window.height - height) / 2)
        width: Math.min(560, home.width - 64)
        height: Math.min(640, home.Window.height - 64)
        modal: true
        dim: true
        padding: 24
        property bool showCode: false
        onClosed: {
            showCode = false;
            msgs.openChat("");
        }
        onOpened: chatField.forceActiveFocus()
        background: Rectangle {
            radius: Tema.radiusPanel
            color: Tema.surface
            border.color: Tema.lineStrong
        }
        contentItem: ColumnLayout {
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: msgs.chatWith
                    elide: Text.ElideRight
                    font.family: Tema.display
                    font.pixelSize: 26
                    font.weight: Font.ExtraBold
                    color: Tema.text
                }
                TextButton {
                    kind: "quiet"
                    text: chatPopup.showCode ? qsTr("Mesajlar") : qsTr("Güvenlik kodu")
                    onClicked: chatPopup.showCode = !chatPopup.showCode
                }
                TextButton {
                    kind: "quiet"
                    text: qsTr("Kapat")
                    onClicked: chatPopup.close()
                }
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Uçtan uca şifreli: mesajları yalnız sen ve %1 okuyabilir, Oviora sunucusu bile göremez.").arg(msgs.chatWith)
                font.family: Tema.body
                font.pixelSize: 13
                color: Tema.faint
            }
            // A changed key: someone may be in between, or the friend set up a new device
            Rectangle {
                visible: msgs.chatKeyChanged
                Layout.fillWidth: true
                implicitHeight: keyWarn.implicitHeight + 20
                radius: 8
                color: "transparent"
                border.color: Tema.warm
                Label {
                    textFormat: Text.PlainText
                    id: keyWarn
                    anchors.fill: parent
                    anchors.margins: 10
                    wrapMode: Text.WordWrap
                    text: qsTr("%1 için güvenlik anahtarı değişti. Yeni bir cihaz kurmuş olabilir; emin olmak için güvenlik kodunu onunla karşılaştır.").arg(msgs.chatWith)
                    font.family: Tema.body
                    font.pixelSize: 13
                    color: Tema.warm
                }
            }
            // The safety code, compared out of band (in person, on the phone)
            ColumnLayout {
                visible: chatPopup.showCode
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Bu kod sende ve %1 tarafında aynıysa mesajlarınızı aranızdaki kimse okuyamaz. Kodu yüz yüze ya da telefonda karşılaştırın.").arg(msgs.chatWith)
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.muted
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: {
                        msgs.chat;	// refresh with the keys
                        const c = msgs.safetyCode(msgs.chatWith);
                        return c.length ? c : qsTr("Anahtar henüz alınmadı.");
                    }
                    font.family: "Consolas"
                    font.pixelSize: 20
                    lineHeight: 1.4
                    color: Tema.text
                }
                TextButton {
                    kind: "primary"
                    text: qsTr("Kodlar aynı, güven")
                    onClicked: {
                        msgs.confirmKeys(msgs.chatWith);
                        chatPopup.showCode = false;
                    }
                }
                Item { Layout.fillHeight: true }
            }
            ListView {
                id: chatList
                visible: !chatPopup.showCode
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: msgs.chat
                onCountChanged: positionViewAtEnd()
                delegate: Item {
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: bubble.height
                    Rectangle {
                        id: bubble
                        anchors.right: modelData.mine ? parent.right : undefined
                        anchors.left: modelData.mine ? undefined : parent.left
                        width: Math.min(parent.width * 0.78, bubbleText.implicitWidth + 24)
                        height: bubbleText.implicitHeight + bubbleTime.implicitHeight + 18
                        radius: 12
                        color: modelData.mine ? Tema.accent : Tema.surface2
                        Label {
                            textFormat: Text.PlainText
                            id: bubbleText
                            x: 12
                            y: 8
                            width: Math.min(chatList.width * 0.78 - 24, implicitWidth)
                            wrapMode: Text.Wrap
                            text: modelData.text
                            font.family: Tema.body
                            font.pixelSize: 15
                            color: modelData.mine ? Tema.inkOnAccent : (modelData.failed ? Tema.warm : Tema.text)
                        }
                        Label {
                            textFormat: Text.PlainText
                            id: bubbleTime
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.top: bubbleText.bottom
                            text: modelData.failed && modelData.mine ? qsTr("gönderilemedi")
                                : modelData.sending ? qsTr("gönderiliyor…") : Qt.formatDateTime(new Date(modelData.at), "HH:mm")
                            font.family: Tema.body
                            font.pixelSize: 11
                            color: modelData.mine ? Tema.inkOnAccent : Tema.faint
                            opacity: 0.8
                        }
                    }
                }
                Label {
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    visible: chatList.count === 0
                    text: qsTr("Henüz mesaj yok. İlk mesajı sen yaz.")
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.muted
                }
            }
            RowLayout {
                visible: !chatPopup.showCode
                Layout.fillWidth: true
                spacing: 8
                Field {
                    id: chatField
                    placeholderText: msgs.ready ? qsTr("Mesaj yaz") : qsTr("Mesajlaşma hazırlanıyor…")
                    enabled: msgs.ready
                    maximumLength: 2000
                    font.pixelSize: 15
                    topPadding: 10
                    bottomPadding: 10
                    onAccepted: sendButton.clicked()
                }
                TextButton {
                    id: sendButton
                    kind: "primary"
                    text: qsTr("Gönder")
                    enabled: msgs.ready && chatField.text.trim().length > 0
                    onClicked: {
                        msgs.send(chatField.text);
                        chatField.text = "";
                    }
                }
            }
        }
    }
            }
        }
    }
    Connections {
        target: msgs
        function onIncoming(from, preview) { home.showToast(qsTr("%1 mesaj gönderdi").arg(from), preview); }
        function onFailed(text) { home.showToast(qsTr("Mesaj gönderilemedi"), text); }
    }

    // ── Invite: my link and code as text and QR, and a friend's code ──
    Loader {
        id: inviteLoader
        active: false
        sourceComponent: Component {
            Item {
                property alias popup: invitePopup
                Popup {
                    id: invitePopup
                    parent: Overlay.overlay
                    x: Math.round((home.Window.width - width) / 2)
                    y: Math.round((home.Window.height - height) / 2)
                    dim: true
                    width: Math.min(560, home.width - 64)
                    height: Math.min(implicitHeight, home.Window.height - 48)
                    modal: true
                    padding: 28
                    readonly property var info: home.server.invite || ({})
                    readonly property string link: info.url || info.url_ip || ""
        onOpened: acceptField.text = ""
        background: Rectangle {
            radius: Tema.radiusPanel
            color: Tema.surface
            border.color: Tema.lineStrong
        }
        Overlay.modal: Rectangle { color: "#99000000" }
        contentItem: ColumnLayout {
            spacing: 12
            // Clipboard through a hidden text field, as elsewhere in the app
            TextEdit {
                id: inviteClip
                visible: false
                Layout.preferredHeight: 0
                text: invitePopup.link
            }
            Label {
                textFormat: Text.PlainText
                text: qsTr("Arkadaşını davet et")
                font.family: Tema.display
                font.pixelSize: 28
                font.weight: Font.ExtraBold
                font.letterSpacing: -0.6
                color: Tema.text
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: invitePopup.info.approve
                    ? qsTr("Bağlantıyla gelen kişi sana arkadaşlık isteği gönderir; sen onaylarsın.")
                    : qsTr("Bağlantıyla kayıt olan ya da giriş yapan kişiyle hemen arkadaş olursunuz.")
                font.family: Tema.body
                font.pixelSize: 15
                color: Tema.muted
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 20
                Rectangle {
                    implicitWidth: 168
                    implicitHeight: 168
                    radius: 10
                    color: "#ffffff"
                    Image {
                        visible: !home.streamerMode
                        anchors.fill: parent
                        anchors.margins: 4
                        source: home.server.inviteQr
                        fillMode: Image.PreserveAspectFit
                        smooth: false
                        sourceSize: Qt.size(320, 320)
                    }
                    Label {
                        textFormat: Text.PlainText
                        visible: home.streamerMode
                        anchors.centerIn: parent
                        text: qsTr("Gizli")
                        font.family: Tema.body
                        font.pixelSize: 15
                        color: Tema.muted
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        textFormat: Text.PlainText
                        text: qsTr("Davet kodu")
                        font.family: Tema.body
                        font.pixelSize: 13
                        color: Tema.faint
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: home.streamerMode ? "••••••" : (invitePopup.info.code ? invitePopup.info.code : "…")
                        font.family: Tema.display
                        font.pixelSize: 32
                        font.weight: Font.Bold
                        font.letterSpacing: 2
                        color: Tema.text
                    }
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        text: home.streamerMode ? qsTr("Yayıncı modunda gizli") : invitePopup.link
                        font.family: Tema.body
                        font.pixelSize: 13
                        color: Tema.muted
                    }
                    Label {
                        textFormat: Text.PlainText
                        visible: (invitePopup.info.uses || 0) > 0
                        text: qsTr("Bu davetle %1 kişi geldi.").arg(invitePopup.info.uses || 0)
                        font.family: Tema.body
                        font.pixelSize: 13
                        color: Tema.faint
                    }
                }
            }
            RowLayout {
                Layout.topMargin: 6
                spacing: 8
                TextButton {
                    kind: "primary"
                    enabled: invitePopup.link.length > 0
                    text: copiedTimer.running ? qsTr("Kopyalandı") : qsTr("Bağlantıyı kopyala")
                    onClicked: {
                        inviteClip.selectAll();
                        inviteClip.copy();
                        inviteClip.deselect();
                        copiedTimer.restart();
                    }
                    Timer { id: copiedTimer; interval: 2000 }
                }
                TextButton {
                    text: qsTr("Yeni kod")
                    onClicked: home.showConfirmDialog(qsTr("Yeni davet kodu"),
                        qsTr("Eski kod ve bağlantı hemen geçersiz olur. Yeni kod alınsın mı?"),
                        () => home.server.loadInvite(true))
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Davetle geleni ben onaylayayım")
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.text
                }
                Switch {
                    checked: !!invitePopup.info.approve
                    onToggled: home.server.setInviteApprove(checked)
                }
            }
            Rectangle { Layout.fillWidth: true; Layout.topMargin: 8; implicitHeight: 1; color: Tema.line }
            Label {
                textFormat: Text.PlainText
                Layout.topMargin: 6
                text: qsTr("Sana bir davet mi geldi?")
                font.family: Tema.body
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Tema.text
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Field {
                    id: acceptField
                    placeholderText: qsTr("Davet kodu ya da bağlantısı")
                    font.pixelSize: 15
                    topPadding: 9
                    bottomPadding: 9
                    onAccepted: acceptButton.clicked()
                }
                TextButton {
                    id: acceptButton
                    text: qsTr("Kullan")
                    enabled: home.server.inviteCode(acceptField.text).length > 0
                    onClicked: {
                        home.server.acceptInvite(acceptField.text);
                        acceptField.text = "";
                    }
                }
            }
            TextButton {
                Layout.alignment: Qt.AlignRight
                Layout.topMargin: 4
                kind: "quiet"
                text: qsTr("Kapat")
                onClicked: invitePopup.close()
            }
        }
    }
            }
        }
    }

    // ── Sign in / register ───────────────────────────────────────────────
    ColumnLayout {
        visible: !home.signedIn
        anchors.centerIn: parent
        width: Math.min(parent.width - 96, 460)
        spacing: 12

        Label {
            textFormat: Text.PlainText
            text: home.registering ? qsTr("Hesap aç") : qsTr("Giriş yap")
            font.family: Tema.display
            font.pixelSize: 48
            font.weight: Font.ExtraBold
            font.letterSpacing: -1.4
            color: Tema.text
        }
        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            Layout.bottomMargin: 22
            wrapMode: Text.WordWrap
            text: qsTr("PS5'ini arkadaşlarınla paylaş, onlarınkinde oyna.")
            font.family: Tema.body
            font.pixelSize: 19
            color: Tema.muted
        }
        Field {
            id: userField
            placeholderText: qsTr("Kullanıcı adı")
            focus: true
            onAccepted: home.submit()
        }
        Field {
            id: passField
            placeholderText: qsTr("Şifre")
            echoMode: TextInput.Password
            onAccepted: home.submit()
        }
        Field {
            id: pass2Field
            visible: home.registering
            placeholderText: qsTr("Şifre (tekrar)")
            echoMode: TextInput.Password
            onAccepted: home.submit()
        }
        TextButton {
            visible: !home.showInvite
            Layout.leftMargin: -10
            kind: "quiet"
            text: qsTr("Davet kodum var")
            onClicked: {
                home.showInvite = true;
                inviteField.forceActiveFocus();
            }
        }
        Field {
            id: inviteField
            visible: home.showInvite
            placeholderText: qsTr("Davet kodu ya da bağlantısı")
            onTextChanged: {
                home.inviteFrom = "";
                home.inviteError = "";
                inviteCheckTimer.restart();
            }
            onAccepted: home.submit()
        }
        Label {
            textFormat: Text.PlainText
            visible: home.showInvite && (home.inviteFrom.length > 0 || home.inviteError.length > 0)
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: home.inviteFrom.length ? qsTr("%1 seni Oviora'ya davet etti.").arg(home.inviteFrom) : home.inviteError
            font.family: Tema.body
            font.pixelSize: 15
            color: home.inviteFrom.length ? Tema.ok : Tema.warm
        }
        Timer {
            id: inviteCheckTimer
            interval: 600
            onTriggered: home.server.checkInvite(inviteField.text)
        }
        Connections {
            target: home.server
            function onInviteChecked(code, from, error) {
                if (code !== home.server.inviteCode(inviteField.text))
                    return;
                home.inviteFrom = from;
                home.inviteError = code.length ? error : "";
            }
        }
        // Registering accepts the terms (opens on the site)
        RowLayout {
            visible: home.registering
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 12
            AbstractButton {
                id: termsBox
                checkable: true
                focusPolicy: Qt.StrongFocus
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 2
                implicitWidth: 22
                implicitHeight: 22
                background: Rectangle {
                    radius: 6
                    color: termsBox.checked ? Tema.accent : (termsBox.hovered ? Tema.line : Tema.surface2)
                    border.width: termsBox.checked ? 0 : 1
                    border.color: termsBox.visualFocus ? Tema.accent : Tema.lineStrong
                }
                contentItem: Text {
                    textFormat: Text.PlainText
                    text: "\u2713"
                    visible: termsBox.checked
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 15
                    font.bold: true
                    color: Tema.inkOnAccent
                }
                Keys.onReturnPressed: termsBox.toggle()
                Keys.onEnterPressed: termsBox.toggle()
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.StyledText
                linkColor: Tema.text
                text: qsTr("<a href=\"kosullar.php\">Kullanım Koşulları</a>'nı okudum ve kabul ediyorum.")
                font.family: Tema.body
                font.pixelSize: 15
                color: Tema.muted
                onLinkActivated: (link) => Qt.openUrlExternally(home.server.legalUrl(link))
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    cursorShape: parent.hoveredLink.length ? Qt.PointingHandCursor : Qt.ArrowCursor
                }
                TapHandler {
                    enabled: parent.hoveredLink.length === 0
                    onTapped: termsBox.toggle()
                }
            }
        }
        // The privacy notice is information, not something to accept (kept apart from the terms box)
        Label {
            visible: home.registering
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.StyledText
            linkColor: Tema.text
            text: qsTr("Kişisel verilerinin nasıl işlendiği (sunucu Türkiye'de, İstanbul'da) <a href=\"gizlilik.php\">Gizlilik Politikası ve KVKK Aydınlatma Metni</a>'nde anlatılır.")
            font.family: Tema.body
            font.pixelSize: 14
            color: Tema.muted
            onLinkActivated: (link) => Qt.openUrlExternally(home.server.legalUrl(link))
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                cursorShape: parent.hoveredLink.length ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
        }        RowLayout {
            Layout.topMargin: 18
            spacing: 12
            TextButton {
                kind: "primary"
                large: true
                enabled: !home.waiting
                text: home.waiting ? qsTr("Bekle…") : (home.registering ? qsTr("Kayıt ol") : qsTr("Giriş yap"))
                onClicked: home.submit()
            }
            TextButton {
                kind: "quiet"
                text: home.registering ? qsTr("Hesabım var, giriş yap") : qsTr("Hesabım yok, kayıt ol")
                onClicked: home.registering = !home.registering
            }
        }
    }

    // ── Signed in ────────────────────────────────────────────────────────
    RowLayout {
        visible: home.signedIn
        anchors {
            top: topBar.bottom
            left: parent.left
            right: parent.right
            bottom: updateBar.top
            leftMargin: 56
            rightMargin: 40
            topMargin: 34
            bottomMargin: 8
        }
        spacing: 64

        // PS5s to connect to
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.maximumWidth: 900
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: parent.width
                spacing: 0

                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: home.streamerMode ? qsTr("Merhaba") : qsTr("Merhaba, %1").arg(home.server.user)
                    elide: Text.ElideRight
                    font.family: Tema.display
                    font.pixelSize: 40
                    font.weight: Font.ExtraBold
                    font.letterSpacing: -1.2
                    color: Tema.text
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 30
                    text: qsTr("Hangi PS5'te oynuyorsun?")
                    font.family: Tema.body
                    font.pixelSize: 20
                    color: Tema.muted
                }

                // ── Over PSN: own PS5s and the ones friends opened, reachable from anywhere ──
                RowLayout {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 2
                    SectionLabel {
                        text: qsTr("PSN üzerinden")
                        info: qsTr("İnternet üzerinden bağlanır; PS5 evde, sen başka yerde olsan da olur.")
                    }
                    Item { Layout.fillWidth: true }
                    TextButton {
                        kind: "quiet"
                        visible: home.server.psnLinked
                        enabled: !home.server.busy
                        text: qsTr("PS5 listemi yenile")
                        onClicked: home.server.refreshConsoles()
                    }
                }

                // Own PS5 not added yet
                ColumnLayout {
                    visible: home.server.loaded && !home.server.psnLinked
                    Layout.fillWidth: true
                    Layout.topMargin: 10
                    Layout.bottomMargin: 8
                    spacing: 6
                    Label {
                        textFormat: Text.PlainText
                        text: qsTr("Kendi PS5'ini ekle")
                        font.family: Tema.body
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                        color: Tema.text
                    }
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        Layout.maximumWidth: 640
                        wrapMode: Text.WordWrap
                        text: qsTr("PSN hesabınla bir kez giriş yap; PS5'lerin buraya gelir. Sonra sağdan arkadaşlarına açabilirsin. Giriş bilgin sunucuda şifreli durur, sadece senin izin verdiğin arkadaşa bağlandığı süre için geçici giriş verilir.")
                        font.family: Tema.body
                        font.pixelSize: 15
                        lineHeight: 1.3
                        color: Tema.muted
                    }
                    TextButton {
                        Layout.topMargin: 10
                        kind: "primary"
                        enabled: !home.server.busy
                        text: home.server.busy ? qsTr("Bağlanıyor…") : qsTr("PSN hesabını bağla")
                        onClicked: home.showPsnLinkDialog()
                    }
                }

                EmptyNote {
                    visible: home.server.loaded && home.server.available.length == 0 && home.server.psnLinked
                    text: qsTr("PSN hesabında PS5 bulunamadı. PS5'te Ayarlar → Sistem → Uzaktan Oynatma açık olmalı; sonra \"PS5 listemi yenile\".")
                }

                Repeater {
                    model: home.server.available
                    delegate: ConsoleRow {
                        id: card
                        required property var modelData
                        required property int index
                        readonly property var mine: modelData.mine ? home.myConsole(modelData.duid) : null
                        readonly property bool here: home.server.sessionDuid == modelData.duid
                        readonly property bool taken: modelData.busy.length > 0
                        tone: taken && !here ? Tema.warm : Tema.ok
                        name: modelData.name
                        stateText: {
                            let state;
                            if (card.here)
                                state = home.server.sessionState == "asking" ? qsTr("bağlanılıyor…") : qsTr("bağlısın");
                            else if (card.taken)
                                state = qsTr("%1 oynuyor").arg(card.modelData.busy);
                            else
                                state = qsTr("boşta");
                            const line = card.modelData.mine ? qsTr("Senin konsolun, %1").arg(state)
                                                             : qsTr("%1 paylaşıyor, %2").arg(card.modelData.host).arg(state);
                            // What the PS5 runs, as its owner's app last reported it (me.available[].app)
                            const app = card.modelData.app;
                            return app && app.name ? line + " · " + qsTr("Şu an açık: %1").arg(app.name) : line;
                        }

                        // Someone else on my PS5: I can always take it back
                        TextButton {
                            visible: !!card.mine && card.taken && card.mine.sid.length > 0
                            kind: "quiet"
                            tone: Tema.warm
                            text: qsTr("Çıkar")
                            onClicked: home.showConfirmDialog(qsTr("Çıkar"),
                                qsTr("%1 oturumu birkaç saniye içinde kapanır ve sen tekrar açana kadar bu PS5'e bağlanamaz.").arg(card.modelData.busy),
                                () => home.server.kick(card.mine.sid))
                        }
                        // Someone plays on a friend's PS5: wait in line (servers without a queue: just "Dolu")
                        readonly property bool canQueue: card.taken && !card.here && !card.modelData.mine
                            && card.modelData.queued !== undefined
                        readonly property bool queued: !!card.modelData.queued
                        TextButton {
                            visible: card.canQueue
                            kind: card.queued ? "quiet" : "secondary"
                            tone: card.queued ? Tema.accentText : Tema.text
                            text: card.queued
                                ? (card.modelData.queue_pos > 0 ? qsTr("Sıradasın (%1.)").arg(card.modelData.queue_pos) : qsTr("Sıradasın"))
                                : (card.modelData.queue_len > 0 ? qsTr("Sıraya gir (%1 kişi)").arg(card.modelData.queue_len) : qsTr("Sıraya gir"))
                            onClicked: {
                                if (card.queued)
                                    home.showConfirmDialog(qsTr("Sıradan çık"),
                                        qsTr("%1 için sıradan çıkılsın mı?").arg(card.modelData.name),
                                        () => home.server.setQueue(card.modelData.duid, false));
                                else
                                    home.server.setQueue(card.modelData.duid, true);
                            }
                        }
                        TextButton {
                            visible: !card.canQueue
                            kind: card.taken ? "secondary" : "primary"
                            enabled: !card.taken && home.server.sessionState == "idle"
                            text: card.taken ? qsTr("Dolu") : ((card.modelData.turn > 0) ? qsTr("Bağlan (sıra sende)") : qsTr("Bağlan"))
                            onClicked: {
                                const h = card.modelData.host;
                                const d = card.modelData.duid;
                                home.connectionStarted(() => home.server.connectTo(h, d));
                                home.server.connectTo(h, d);
                            }
                        }
                    }
                }

                // ── This network: PS5s found next to this PC, straight connection without PSN ──
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 40
                    Layout.bottomMargin: 2
                    spacing: 4
                    SectionLabel {
                        text: qsTr("Bu ağda")
                        info: qsTr("Bu bilgisayarla aynı ağdaki PS5'lere doğrudan bağlanır; daha hızlıdır, PSN gerekmez.")
                    }
                    Item { Layout.fillWidth: true }
                    TextButton {
                        kind: "quiet"
                        text: oviora.lanSearch ? qsTr("Ağda aramayı kapat") : qsTr("Ağda aramayı aç")
                        onClicked: oviora.lanSearch = !oviora.lanSearch
                    }
                    TextButton {
                        kind: "quiet"
                        text: qsTr("Konsol ekle")
                        onClicked: home.requestManualHost()
                    }
                }
                EmptyNote {
                    visible: home.localCount == 0
                    text: oviora.lanSearch
                        ? qsTr("Bu ağda PS5 bulunamadı. PS5 açık ya da dinlenme modunda ve bu bilgisayarla aynı ağda olmalı; bulunamazsa IP adresiyle ekleyebilirsin.")
                        : qsTr("Ağda arama kapalı. Açınca bu ağdaki PS5'ler burada görünür.")
                }

                Repeater {
                    model: oviora.lanConsoles
                    delegate: ConsoleRow {
                        id: local
                        required property var modelData
                        required property int index
                        // PSN hosts of this PC's own login belong to the section above, unless the PS5 was found
                        // on this network: then it is listed here and connected to directly (an unregistered one is
                        // registered with the PSN account, no PIN). Hidden before, so a PS5 next to the PC went over PSN.
                        visible: modelData.display && (!modelData.duid || modelData.discovered)
                        Layout.preferredHeight: visible ? implicitHeight : 0
                        readonly property bool ready: modelData.state == "ready"
                        readonly property bool resting: modelData.state == "standby"
                        tone: ready ? Tema.ok : resting ? Tema.warm : Tema.faint
                        name: modelData.name
                        stateText: {
                            const h = local.modelData;
                            let parts = [h.discovered ? (local.ready ? qsTr("Açık") : local.resting ? qsTr("Dinlenme modunda") : qsTr("Durumu bilinmiyor")) : qsTr("Ağda görünmüyor")];
                            if (h.app)
                                parts.push(qsTr("Şu an açık: %1").arg(h.app));
                            if (h.address)
                                parts.push(home.streamerMode ? qsTr("adres gizli") : h.address);
                            if (!h.registered)
                                parts.push(qsTr("bu bilgisayara kayıtlı değil"));
                            return parts.join(", ");
                        }

                        function connect() {
                            const k = modelData.key;
                            home.connectionStarted(() => oviora.lanConnect(k));
                            oviora.lanConnect(k);
                        }

                        TextButton {
                            kind: "quiet"
                            visible: local.modelData.manual || (local.modelData.discovered && !local.modelData.registered)
                            text: local.modelData.manual ? qsTr("Sil") : qsTr("Gizle")
                            onClicked: {
                                const h = local.modelData;
                                if (h.manual)
                                    home.showConfirmDialog(qsTr("Konsolu sil"), qsTr("%1 listeden silinsin mi?").arg(h.name), () => oviora.lanForget(local.modelData.key));
                                else
                                    home.showConfirmDialog(qsTr("Konsolu gizle"), qsTr("%1 gizlensin mi? Ayarlar → Konsollar'dan geri açabilirsin.").arg(h.name), () => oviora.lanHide(local.modelData.key));
                            }
                        }
                        // A registered console: its address, or another PSN account (new code from the console)
                        TextButton {
                            kind: "quiet"
                            visible: local.modelData.registered === true
                            text: qsTr("Düzenle")
                            onClicked: oviora.lanEdit(local.modelData.key)
                        }
                        // A registered console off this PC (its pairing file); registering again needs a new code
                        TextButton {
                            kind: "quiet"
                            visible: local.modelData.registered === true
                            text: qsTr("Sil")
                            onClicked: {
                                const h = local.modelData;
                                home.showConfirmDialog(qsTr("Konsolu sil"),
                                    qsTr("%1 bu bilgisayardan silinsin mi? Tekrar bağlanmak için yeniden kaydetmen gerekir.").arg(h.name),
                                    () => oviora.lanForget(h.key));
                            }
                        }
                        TextButton {
                            visible: local.modelData.registered && !local.modelData.discovered
                            text: qsTr("Uyandır")
                            onClicked: oviora.lanWake(local.modelData.key)
                        }
                        TextButton {
                            kind: "primary"
                            enabled: home.server.sessionState == "idle"
                            // Not registered yet: connecting starts the one-time registration with the PS5's code
                            text: local.modelData.registered ? qsTr("Bağlan") : qsTr("Kaydet ve bağlan")
                            onClicked: local.connect()
                        }
                    }
                }
                Item { Layout.preferredHeight: 24 }
            }
        }

        // Friends and sharing: a quiet column, no box
        ScrollView {
            Layout.preferredWidth: 320
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: parent.width
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 14
                    SectionLabel {
                        text: qsTr("Arkadaşlar")
                        info: qsTr("Arkadaşının Oviora kullanıcı adını yaz ve ekle; o kabul edince birbirinize PS5'lerinizi açabilirsiniz.")
                    }
                    Item { Layout.fillWidth: true }
                    TextButton {
                        id: inviteButton
                        kind: "quiet"
                        text: qsTr("Davet et")
                        onClicked: {
                            home.server.loadInvite(false);
                            inviteLoader.active = true;
                            if (inviteLoader.item) inviteLoader.item.popup.open();
                        }
                        // Testing: YAKAMOZ_TEST_DAVET opens the invite card
                        Timer {
                            interval: 9000
                            running: "".length > 0
                            onTriggered: inviteButton.clicked()
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    Layout.bottomMargin: 6
                    spacing: 8
                    Field {
                        id: friendField
                        placeholderText: qsTr("Kullanıcı adıyla ekle")
                        font.pixelSize: 15
                        topPadding: 9
                        bottomPadding: 9
                        leftPadding: 14
                        onAccepted: addButton.clicked()
                    }
                    TextButton {
                        id: addButton
                        text: qsTr("Ekle")
                        enabled: friendField.text.trim().length > 0
                        onClicked: {
                            home.server.addFriend(friendField.text);
                            friendField.text = "";
                        }
                    }
                }

                EmptyNote {
                    visible: home.server.loaded && home.server.friends.length == 0 && home.server.incoming.length == 0 && home.server.outgoing.length == 0
                    text: qsTr("Henüz arkadaşın yok.")
                }

                Repeater {
                    model: home.server.friends
                    delegate: ColumnLayout {
                        id: friendRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            Layout.bottomMargin: 4
                            spacing: 12
                            Rectangle {
                                implicitWidth: 30
                                implicitHeight: 30
                                radius: 15
                                color: Tema.surface
                                border.color: Tema.line
                                Label {
                                    textFormat: Text.PlainText
                                    anchors.centerIn: parent
                                    text: home.initials(friendRow.modelData.name)
                                    font.family: Tema.body
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    color: Tema.text
                                }
                                // online: a small green light on the circle
                                Rectangle {
                                    visible: friendRow.modelData.online
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.margins: -1
                                    width: 10
                                    height: 10
                                    radius: 5
                                    color: Tema.ok
                                    border.width: 2
                                    border.color: Tema.bg
                                }
                            }
                            Label {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: friendRow.modelData.name
                                elide: Text.ElideRight
                                font.family: Tema.body
                                font.pixelSize: 16
                                font.weight: Font.DemiBold
                                color: Tema.text
                            }
                            Label {
                                textFormat: Text.PlainText
                                text: friendRow.modelData.online ? qsTr("çevrimiçi") : qsTr("çevrimdışı")
                                font.family: Tema.body
                                font.pixelSize: 13
                                color: Tema.muted
                            }
                            // Oviora: end-to-end encrypted messages
                            TextButton {
                                id: messageButton
                                readonly property int unread: (msgs.unread && friendRow.modelData && friendRow.modelData.name && msgs.unread[friendRow.modelData.name.toLowerCase()]) || 0
                                kind: unread > 0 ? "primary" : "quiet"
                                text: unread > 0 ? qsTr("Mesaj (%1)").arg(unread) : qsTr("Mesaj")
                                implicitHeight: 32
                                onClicked: home.openChat(friendRow.modelData.name)
                            }
                            TextButton {
                                kind: "quiet"
                                text: qsTr("Kaldır")
                                implicitHeight: 32
                                onClicked: home.showConfirmDialog(qsTr("Arkadaşlıktan çıkar"),
                                    qsTr("%1 arkadaşlıktan çıkar; birbirinizin PS5'lerine erişiminiz kalkar.").arg(friendRow.modelData.name),
                                    () => home.server.removeFriend(friendRow.modelData.name))
                            }
                        }
                        // Which of my PS5s this friend may use
                        Repeater {
                            model: home.server.consoles
                            delegate: Basic.Switch {
                                id: shareSwitch
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.leftMargin: 42
                                checked: modelData.shared.indexOf(friendRow.modelData.name) >= 0
                                text: qsTr("%1 kullanabilir").arg(modelData.name)
                                font.family: Tema.body
                                font.pixelSize: 14
                                padding: 4
                                onToggled: home.server.setShare(modelData.duid, friendRow.modelData.name, checked)
                                contentItem: Label {
                                    textFormat: Text.PlainText
                                    text: shareSwitch.text
                                    font: shareSwitch.font
                                    color: shareSwitch.checked ? Tema.text : Tema.muted
                                    leftPadding: shareSwitch.indicator.width + 10
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                                indicator: Rectangle {
                                    implicitWidth: 34
                                    implicitHeight: 20
                                    x: shareSwitch.leftPadding
                                    y: parent.height / 2 - height / 2
                                    radius: 10
                                    color: shareSwitch.checked ? Tema.accent : Tema.surface2
                                    border.color: shareSwitch.checked ? Tema.accent : Tema.lineStrong
                                    Rectangle {
                                        x: shareSwitch.checked ? parent.width - width - 3 : 3
                                        y: 3
                                        width: 14
                                        height: 14
                                        radius: 7
                                        color: shareSwitch.checked ? Tema.inkOnAccent : Tema.muted
                                        Behavior on x { NumberAnimation { duration: 120 } }
                                    }
                                }
                            }
                        }
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            Layout.leftMargin: 42
                            Layout.topMargin: 2
                            visible: friendRow.modelData.shares.length > 0
                            wrapMode: Text.WordWrap
                            text: qsTr("Seninle paylaşıyor: %1").arg(friendRow.modelData.shares.join(", "))
                            font.family: Tema.body
                            font.pixelSize: 13
                            color: Tema.muted
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.topMargin: 10
                            implicitHeight: 1
                            color: Tema.line
                        }
                    }
                }

                // Requests to me
                SectionLabel {
                    visible: home.server.incoming.length > 0
                    Layout.topMargin: 26
                    Layout.bottomMargin: 4
                    text: qsTr("Gelen istek")
                }
                Repeater {
                    model: home.server.incoming
                    delegate: RowLayout {
                        required property string modelData
                        Layout.fillWidth: true
                        spacing: 12
                        Rectangle {
                            implicitWidth: 30
                            implicitHeight: 30
                            radius: 15
                            color: Tema.surface
                            border.color: Tema.line
                            Label {
                                textFormat: Text.PlainText
                                anchors.centerIn: parent
                                text: home.initials(modelData)
                                font.family: Tema.body
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                color: Tema.text
                            }
                        }
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: modelData
                            elide: Text.ElideRight
                            font.family: Tema.body
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            color: Tema.text
                        }
                        TextButton { kind: "quiet"; tone: Tema.accentText; implicitHeight: 32; text: qsTr("Kabul et"); onClicked: home.server.acceptFriend(modelData) }
                        TextButton { kind: "quiet"; implicitHeight: 32; text: qsTr("Reddet"); onClicked: home.server.removeFriend(modelData) }
                    }
                }

                // My requests still waiting
                SectionLabel {
                    visible: home.server.outgoing.length > 0
                    Layout.topMargin: 26
                    Layout.bottomMargin: 4
                    text: qsTr("Gönderdiğin istek")
                }
                Repeater {
                    model: home.server.outgoing
                    delegate: RowLayout {
                        required property string modelData
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: qsTr("%1, kabul etmesi bekleniyor").arg(modelData)
                            elide: Text.ElideRight
                            font.family: Tema.body
                            font.pixelSize: 15
                            color: Tema.muted
                        }
                        TextButton { kind: "quiet"; implicitHeight: 32; text: qsTr("Geri al"); onClicked: home.server.removeFriend(modelData) }
                    }
                }
                Item { Layout.preferredHeight: 24 }
            }
        }
    }

    // ── Update state ─────────────────────────────────────────────────────
    RowLayout {
        id: updateBar
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            leftMargin: 56
            rightMargin: 40
            bottomMargin: 16
        }
        height: 40
        spacing: 16
        readonly property string state: home.server.updateState
        visible: home.signedIn

        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: updateBar.state == "downloading" || updateBar.state == "ready" || updateBar.state == "error"
                ? home.server.updateText
                : (home.server.installedVersion ? qsTr("Sürüm %1").arg(home.server.installedVersion) : "")
            font.family: Tema.body
            font.pixelSize: 13
            color: updateBar.state == "error" ? Tema.danger : Tema.faint
            elide: Text.ElideRight
        }
        TextButton {
            visible: updateBar.state == "ready"
            text: qsTr("Şimdi güncelle")
            onClicked: home.server.applyUpdate()
        }
    }

    // ── Required update: covers everything until the new Oviora is installed ──
    Rectangle {
        anchors.fill: parent
        visible: (home.signedIn && home.server.updateRequired) || home.updatePreview.length > 0
        color: Tema.bg
        // Takes every click and key so nothing underneath can be used
        MouseArea { anchors.fill: parent; hoverEnabled: true }
        Keys.onPressed: (event) => event.accepted = true

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 96, 560)
            spacing: 10
            OvioraMark {
                Layout.preferredWidth: 56
                Layout.preferredHeight: 56
                Layout.bottomMargin: 18
                travelling: home.server.updateState != "error"
            }
            Label {
                textFormat: Text.PlainText
                text: qsTr("Oviora güncelleniyor")
                font.family: Tema.display
                font.pixelSize: 40
                font.weight: Font.ExtraBold
                font.letterSpacing: -1.2
                color: Tema.text
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: home.server.updateState == "error"
                    ? home.server.updateText
                    : qsTr("Yeni sürüm çıktı; kurulana kadar bağlanılamaz. Bitince Oviora kendiliğinden yeniden açılır.")
                font.family: Tema.body
                font.pixelSize: 17
                lineHeight: 1.3
                color: home.server.updateState == "error" ? Tema.danger : Tema.muted
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 22
                height: 3
                radius: 1.5
                color: Tema.line
                visible: home.server.updateState != "error"
                Rectangle {
                    width: parent.width * Math.max(0.03, home.server.updateProgress)
                    height: parent.height
                    radius: 1.5
                    color: Tema.accent
                }
            }
            Label {
                textFormat: Text.PlainText
                // Every step shows where it is: checking, downloading x / y MB, ready (installing)
                visible: home.server.updateState != "error" && home.server.updateText.length > 0
                text: home.server.updateText
                font.family: Tema.body
                font.pixelSize: 14
                color: Tema.muted
            }
            // The release notes: their first line as the title, the rest in a card that scrolls,
            // so long notes no longer push the page off the window
            Rectangle {
                id: notesCard
                readonly property string notes: home.updatePreview.length > 0 ? home.updatePreview : home.server.updateNotes
                readonly property var lines: notes.split("\n")
                readonly property string heading: lines.length > 0 ? lines[0].trim() : ""
                readonly property string body: lines.slice(1).join("\n").trim()
                Layout.fillWidth: true
                Layout.topMargin: 14
                Layout.preferredHeight: Math.min(notesColumn.implicitHeight + 32,
                                                 Math.max(140, home.height - 420))
                visible: notes.length > 0
                radius: Tema.radiusPanel
                color: Tema.surface
                border.color: Tema.line
                Flickable {
                    id: notesFlick
                    anchors.fill: parent
                    anchors.margins: 16
                    clip: true
                    contentHeight: notesColumn.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: notesFlick.contentHeight > notesFlick.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }
                    ColumnLayout {
                        id: notesColumn
                        width: notesFlick.width - 10
                        spacing: 8
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: qsTr("Bu sürümde")
                            font.family: Tema.body
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            font.capitalization: Font.AllUppercase
                            font.letterSpacing: 0.6
                            color: Tema.accent
                        }
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            visible: notesCard.heading.length > 0
                            wrapMode: Text.WordWrap
                            text: notesCard.heading
                            font.family: Tema.body
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            color: Tema.text
                        }
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            visible: notesCard.body.length > 0
                            wrapMode: Text.WordWrap
                            text: notesCard.body
                            font.family: Tema.body
                            font.pixelSize: 14
                            lineHeight: 1.35
                            color: Tema.muted
                        }
                    }
                }
            }
            TextButton {
                Layout.topMargin: 16
                visible: home.server.updateState == "error"
                kind: "primary"
                text: qsTr("Tekrar dene")
                onClicked: home.server.checkUpdate()
            }
        }
    }

    // ── Oviora (new): confirm / info, toast and Sony's sign-in for the account ──
    Loader {
        id: confirmLoader
        active: false
        sourceComponent: Component {
            Item {
                property alias popup: confirmPopup
                Popup {
                    id: confirmPopup
        parent: Overlay.overlay
        x: Math.round((home.Window.width - width) / 2)
        y: Math.round((home.Window.height - height) / 2)
        width: Math.min(460, home.width - 64)
        modal: true
        dim: true
        padding: 24
        property var action: null
        property string title: ""
        property string text: ""
        function ask(t, x, a) { title = t; text = x; action = a; open() }
        function tell(t, x) { title = t; text = x; action = null; open() }
        background: Rectangle { radius: Tema.radiusPanel; color: Tema.surface; border.color: Tema.lineStrong }
        contentItem: ColumnLayout {
            spacing: 14
            Label { textFormat: Text.PlainText; text: confirmPopup.title; font.family: Tema.display; font.pixelSize: 22; color: Tema.text; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            Label { textFormat: Text.PlainText; text: confirmPopup.text; font.family: Tema.body; font.pixelSize: Tema.bodySize; color: Tema.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                TextButton { kind: "quiet"; visible: confirmPopup.action !== null; text: qsTr("Vazgeç"); onClicked: confirmPopup.close() }
                TextButton {
                    kind: "primary"
                    text: confirmPopup.action !== null ? qsTr("Evet") : qsTr("Tamam")
                    onClicked: { const a = confirmPopup.action; confirmPopup.close(); if (a) a(); }
                }
            }
        }
    }
            }
        }
    }

    Rectangle {
        id: toast
        parent: Overlay.overlay
        z: 1000
        visible: opacity > 0
        opacity: 0
        width: Math.min(420, home.width - 48)
        height: toastColumn.implicitHeight + 28
        x: home.Window.width - width - 24
        y: home.Window.height - height - 24
        radius: Tema.radiusPanel
        color: Tema.surface
        border.color: Tema.lineStrong
        Behavior on opacity { NumberAnimation { duration: Tema.fade } }
        function show(t, x) { toastTitle.text = t; toastText.text = x; opacity = 1; toastTimer.restart() }
        Timer { id: toastTimer; interval: 6000; onTriggered: toast.opacity = 0 }
        ColumnLayout {
            id: toastColumn
            anchors.fill: parent
            anchors.margins: 14
            spacing: 4
            Label { textFormat: Text.PlainText; id: toastTitle; font.family: Tema.body; font.weight: Font.DemiBold; font.pixelSize: Tema.bodySize; color: Tema.text; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            Label { textFormat: Text.PlainText; id: toastText; font.family: Tema.body; font.pixelSize: Tema.smallSize; color: Tema.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        }
    }
    Connections {
        target: platform
        function onMessage(title, text) { home.showInfoDialog(title, text) }
        function onToast(title, text) { home.showToast(title, text) }
        function onPsnLoginUrl(url) {
            psnLinkLoader.active = true;
            if (psnLinkLoader.item) {
                psnLinkLoader.item.popup.url = url;
                psnLinkLoader.item.popup.show();
            }
        }
    }

    // Sony's page inside the app (WebView2 child window over webArea); the redirect goes to the server
    Loader {
        id: psnLinkLoader
        active: false
        sourceComponent: Component {
            Item {
                property alias popup: psnLink
                Popup {
                    id: psnLink
        parent: Overlay.overlay
        x: Math.round((home.Window.width - width) / 2)
        y: Math.round((home.Window.height - height) / 2)
        width: Math.min(560, home.Window.width - 48)
        height: Math.min(760, home.Window.height - 48)
        modal: true
        dim: true
        padding: 20
        closePolicy: Popup.CloseOnEscape
        property string url: ""
        property bool sent: false
        function start() { url = ""; sent = false; open(); platform.requestPsnLoginUrl() }
        function show() {
            if (!opened) open();
            const p = webArea.mapToItem(null, 0, 0);
            psnWeb.open(url, p.x, p.y, webArea.width, webArea.height);
        }
        function place() {
            if (!opened || !url.length) return;
            const p = webArea.mapToItem(null, 0, 0);
            psnWeb.setRect(p.x, p.y, webArea.width, webArea.height);
        }
        onClosed: psnWeb.close()
        background: Rectangle { radius: Tema.radiusPanel; color: Tema.surface; border.color: Tema.lineStrong }
        Connections {
            target: psnWeb
            function onRedirected(url) {
                // Only its own sign-in: the console dialog's account lookup uses the same page
                if (!psnLink.opened || psnLink.sent) return;
                psnLink.sent = true;
                psnWeb.close();
                platform.linkPsn(url);
                psnLink.close();
                home.showToast(qsTr("PSN hesabı"), qsTr("Bağlanıyor…"));
            }
        }
        contentItem: ColumnLayout {
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Label { textFormat: Text.PlainText; text: qsTr("PSN hesabını bağla"); font.family: Tema.display; font.pixelSize: 22; color: Tema.text; Layout.fillWidth: true }
                TextButton { kind: "quiet"; text: qsTr("Kapat"); onClicked: psnLink.close() }
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.family: Tema.body
                font.pixelSize: Tema.smallSize
                color: psnWeb.state === "error" ? Tema.danger : Tema.muted
                text: !psnWeb.available ? qsTr("Bu bilgisayarda Microsoft Edge WebView2 yok; PSN girişi açılamıyor.")
                    : psnWeb.state === "error" ? psnWeb.errorText
                    : qsTr("Sony'nin giriş sayfası; giriş bitince hesabın kendiliğinden bağlanır.")
            }
            Item {
                id: webArea
                Layout.fillWidth: true
                Layout.fillHeight: true
                onWidthChanged: psnLink.place()
                onHeightChanged: psnLink.place()
                Label {
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    visible: psnWeb.state !== "ready"
                    text: qsTr("Yükleniyor…")
                    font.family: Tema.body
                    color: Tema.faint
                }
            }
        }
    }
            }
        }
    }
}
