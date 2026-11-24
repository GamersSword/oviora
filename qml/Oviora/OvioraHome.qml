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
    // An animated switch to the home (and the opening's end): the sections rise in one after another
    signal enter()
    signal requestSettings()
    signal requestManualHost()
    signal connectionStarted(var retryFn)
    function showConfirmDialog(title, text, action) {
        confirmLoader.active = true;
        if (confirmLoader.item) confirmLoader.item.popup.ask(title, text, action);
    }
    // The same question whose "Evet" only opens after a few seconds (giving a friend your PS5); cancel runs onCancel
    function showSlowConfirmDialog(title, text, action, onCancel) {
        confirmLoader.active = true;
        if (confirmLoader.item) confirmLoader.item.popup.ask(title, text, action, 3, onCancel);
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
        if (psnLinkLoader.item) psnLinkLoader.item.popup.start(0, "");
    }
    // Oyun bilgisi: the same Sony sign-in for one linked PSN account; what Sony gives is read and sent in C++
    function showPsnWideDialog(link, name) {
        if (!(link > 0)) return;
        psnLinkLoader.active = true;
        if (psnLinkLoader.item) psnLinkLoader.item.popup.start(link, name);
    }
    // End-to-end encrypted messages (MessageCenter, the context property "messages")
    readonly property var msgs: messages
    // The stream's picture is shown (Main.qml): incoming messages go to its card, not this toast
    property bool streamShown: false

    readonly property var server: platform
    readonly property bool streamerMode: oviora.preferences.streamerMode === true
    // --test-register: the register form even when signed in (screenshots of the form)
    readonly property bool testRegister: (Qt.application.arguments || []).indexOf("--test-register") >= 0
    // --test-psn-links: the signed-in home with made-up PSN accounts and PS5s even when signed out
    // --test-groups: the same with made-up friends, groups and a group conversation
    readonly property bool signedIn: (server.status == "active" || server.testPsnLinks === true || server.testGroups === true) && !testRegister
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
        if (shareConsolesLoader.item) shareConsolesLoader.item.popup.close();
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

    // Diagnostics for "the list sometimes scrolls down by itself at start": a move of either column that no
    // drag, wheel or scroll bar made goes to the log with the sizes and the focused item
    component ScrollWatch: Connections {
        required property ScrollView view
        required property string name
        target: view.contentItem
        function onContentYChanged() {
            const f = view.contentItem;
            if (f.moving || f.dragging || (view.ScrollBar.vertical && view.ScrollBar.vertical.pressed))
                return;
            console.log("home-scroll " + name + " y=" + Math.round(f.contentY) + " h=" + Math.round(f.contentHeight)
                + " view=" + Math.round(f.height) + " focus=" + (view.Window.activeFocusItem ? view.Window.activeFocusItem.toString() : "-"));
        }
    }
    ScrollWatch { view: consoleScroll; name: "consoles" }
    ScrollWatch { view: friendScroll; name: "friends" }

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

    // The PSN account my own PS5 belongs to ("kardesim_ps"), shown after its name; "" when unknown
    // and in streamer mode (names stay hidden there)
    function psnOf(c) {
        if (!c || !c.mine || streamerMode)
            return "";
        let name = c.psn_name;
        if (!name) {
            const own = myConsole(c.duid);
            name = own ? own.psn_name : "";
        }
        return (name || "").trim();
    }

    // My console with this duid, from the account view
    function myConsole(duid) {
        for (const c of (server.consoles || []))
            if (c && c.duid == duid)
                return c;
        return null;
    }

    // The rows of one physical PS5 together. The server sends a row per (PS5, PSN account); rows with
    // the same `ps5` key and the same owner are one PS5 (mine: all mine, a friend's: per host). A row
    // without the key (an older server) is a PS5 of its own. Groups in the order of their first row:
    // [{ key, rows: [row, ...], indexes: [index in list, ...] }]
    function ps5Groups(list) {
        const groups = [];
        const at = {};
        const rows = list || [];
        for (let i = 0; i < rows.length; i++) {
            const c = rows[i];
            if (!c)
                continue;
            const key = c.ps5 ? (c.mine ? "me" : "f:" + (c.host || "")) + "/" + c.ps5 : "row:" + i;
            if (at[key] === undefined) {
                at[key] = groups.length;
                groups.push({ key: key, rows: [], indexes: [] });
            }
            groups[at[key]].rows.push(c);
            groups[at[key]].indexes.push(i);
        }
        return groups;
    }
    // A PS5 card's title: the name most of its rows have, else the first row's
    function ps5Title(rows) {
        const count = {};
        let best = rows.length ? rows[0].name : "", most = 1;
        for (const c of rows) {
            count[c.name] = (count[c.name] || 0) + 1;
            if (count[c.name] > most) {
                most = count[c.name];
                best = c.name;
            }
        }
        return best;
    }
    // A PSN row's state words: "bağlısın", "mehmet oynuyor", "boşta"
    function psnStateWord(c) {
        if (server.sessionDuid == c.duid)
            return server.sessionState == "asking" ? qsTr("bağlanılıyor…") : qsTr("bağlısın");
        if (c.busy.length > 0)
            return qsTr("%1 oynuyor").arg(c.busy);
        return qsTr("boşta");
    }
    // A PSN row's (or PS5 card's) state line: whose it is, the state words, what it runs
    function psnStateLine(c, state, app) {
        const line = c.mine ? qsTr("Senin konsolun, %1").arg(state) : qsTr("%1 paylaşıyor, %2").arg(c.host).arg(state);
        // What the PS5 runs, as its owner's app last reported it (me.available[].app)
        return app && app.name ? line + " · " + qsTr("Şu an açık: %1").arg(app.name) : line;
    }

    // A friend row's line about my PS5s: "Paylaşılan: Salon PS5" (one or two named), "3 PS5 paylaşılıyor",
    // "PS5 paylaşılmıyor"; it reads the live account view, so it changes when the server answers
    function shareSummary(friend) {
        // Counted per physical PS5: its rows on several PSN accounts are one
        const names = [];
        for (const g of ps5Groups(server.consoles)) {
            const rows = g.rows.filter(c => (c.shared || []).indexOf(friend) >= 0);
            if (rows.length > 0)
                names.push(ps5Title(rows));
        }
        if (names.length === 0)
            return qsTr("PS5 paylaşılmıyor");
        if (names.length <= 2)
            return qsTr("Paylaşılan: %1").arg(names.join(", "));
        return qsTr("%1 PS5 paylaşılıyor").arg(names.length);
    }

    // Two letters for a name's circle
    function initials(name) {
        const s = (name || "").trim();
        return s.length ? s.substring(0, Math.min(2, s.length)).toLocaleUpperCase(Qt.locale("tr_TR")) : "?";
    }

    // A friend's or group member's presence (OvioraServer.presenceText): "Oynuyor: GTA VI · Ahmet'in
    // PS5'inde · 25 dk", "Çevrimdışı · 2 sa önce"; streamer mode leaves the PS5's owner unnamed.
    // presenceTick reads the times again every minute between the account's refreshes.
    property int presenceTick: 0
    Timer { interval: 60000; running: home.signedIn; repeat: true; onTriggered: home.presenceTick++ }
    function presenceLine(p) { presenceTick; return p ? server.presenceText(p, streamerMode) : ""; }
    // playing or online: the green light
    function presenceOn(p) { return !!p && (p.state === "playing" || p.state === "online"); }
    function presencePlaying(p) { return !!p && p.state === "playing"; }
    // "5 kişi · 2 kişi oynuyor" under a group's name
    function groupSummary(g) {
        const n = (g && g.members) ? g.members.length : 0;
        return g && g.playing > 0 ? qsTr("%1 kişi").arg(n) + " · " + qsTr("%1 kişi oynuyor").arg(g.playing) : qsTr("%1 kişi").arg(n);
    }
    function groupUnreadOf(id) { return (msgs.groupUnread && msgs.groupUnread[String(id)]) || 0; }

    // ── Pieces ───────────────────────────────────────────────────────────
    // kind: "primary" (purple fill, the main action of a row), "secondary" (outlined), "quiet" (text only)
    component TextButton: AbstractButton {
        id: tb
        property string kind: "secondary"
        property bool large: false
        // the sub-rows of a PS5 card: lower, smaller words
        property bool compact: false
        property color tone: kind === "quiet" ? Tema.muted : Tema.text
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        implicitHeight: large ? 52 : (compact ? 32 : 40)
        implicitWidth: label.implicitWidth + (kind === "quiet" ? (compact ? 16 : 20) : (large ? 56 : (compact ? 28 : 36)))
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
            font.pixelSize: tb.large ? 17 : (tb.compact ? 14 : 15)
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
        // small grey words right after the name (the PSN account of my own PS5), "" for none
        property string nameNote
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
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: cr.nameNote.length === 0
                    Layout.maximumWidth: implicitWidth
                    Layout.minimumWidth: Math.min(implicitWidth, 80)
                    Layout.alignment: Qt.AlignBaseline
                    text: cr.name
                    elide: Text.ElideRight
                    font.family: Tema.body
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    color: Tema.text
                }
                Label {
                    textFormat: Text.PlainText
                    visible: cr.nameNote.length > 0
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignBaseline
                    text: cr.nameNote
                    elide: Text.ElideRight
                    font.family: Tema.body
                    font.pixelSize: 12
                    color: Tema.faint
                }
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

    // The actions of one PSN row (c: a row of home.server.available), on a plain row and on a PS5 card's
    // sub-row (compact; the card has its own "Çıkar": kick false)
    component PsnActions: RowLayout {
        id: pa
        property var c: ({})
        property bool compact: false
        property bool kick: true
        readonly property var mine: c.mine ? home.myConsole(c.duid) : null
        readonly property bool here: home.server.sessionDuid == c.duid
        readonly property bool taken: (c.busy || "").length > 0
        // Someone plays on a friend's PS5: wait in line (servers without a queue: just "Dolu")
        readonly property bool canQueue: taken && !here && !c.mine && c.queued !== undefined
        readonly property bool queued: !!c.queued
        spacing: compact ? 4 : 8

        // Someone else on my PS5: I can always take it back
        TextButton {
            visible: pa.kick && !!pa.mine && pa.taken && pa.mine.sid.length > 0
            compact: pa.compact
            kind: "quiet"
            tone: Tema.warm
            text: qsTr("Çıkar")
            onClicked: home.showConfirmDialog(qsTr("Çıkar"),
                qsTr("%1 oturumu birkaç saniye içinde kapanır ve sen tekrar açana kadar bu PS5'e bağlanamaz.").arg(pa.c.busy),
                () => home.server.kick(pa.mine.sid))
        }
        TextButton {
            visible: pa.canQueue
            compact: pa.compact
            kind: pa.queued ? "quiet" : "secondary"
            tone: pa.queued ? Tema.accentText : Tema.text
            text: pa.queued
                ? (pa.c.queue_pos > 0 ? qsTr("Sıradasın (%1.)").arg(pa.c.queue_pos) : qsTr("Sıradasın"))
                : (pa.c.queue_len > 0 ? qsTr("Sıraya gir (%1 kişi)").arg(pa.c.queue_len) : qsTr("Sıraya gir"))
            onClicked: {
                if (pa.queued)
                    home.showConfirmDialog(qsTr("Sıradan çık"),
                        qsTr("%1 için sıradan çıkılsın mı?").arg(pa.c.name),
                        () => home.server.setQueue(pa.c.duid, false));
                else
                    home.server.setQueue(pa.c.duid, true);
            }
        }
        // My own PS5: its name in Oviora (friends see it too)
        TextButton {
            compact: pa.compact
            kind: "quiet"
            visible: !!pa.c.mine
            enabled: !home.server.busy
            text: qsTr("Adını değiştir")
            onClicked: home.showConsoleRename(pa.c.duid, pa.c.name)
        }
        TextButton {
            visible: !pa.canQueue
            compact: pa.compact
            kind: pa.taken ? "secondary" : "primary"
            enabled: !pa.taken && home.server.sessionState == "idle"
            text: pa.taken ? qsTr("Dolu") : ((pa.c.turn > 0) ? qsTr("Bağlan (sıra sende)") : qsTr("Bağlan"))
            onClicked: {
                const h = pa.c.host;
                const d = pa.c.duid;
                home.connectionStarted(() => home.server.connectTo(h, d));
                home.server.connectTo(h, d);
            }
        }
    }

    // A PS5 on one PSN account: the plain console row
    component PsnRow: ConsoleRow {
        id: pr
        property var c: ({})
        readonly property bool here: home.server.sessionDuid == c.duid
        readonly property bool taken: (c.busy || "").length > 0
        tone: taken && !here ? Tema.warm : Tema.ok
        name: c.name || ""
        nameNote: home.psnOf(c)
        stateText: home.psnStateLine(c, home.psnStateWord(c), c.app)
        PsnActions { c: pr.c }
    }

    // One physical PS5 on several PSN accounts (a row each): the PS5 once, with the state its rows share
    // (busy is per PS5), and under it a compact line per row: its name, its PSN account, its actions
    component Ps5Card: Item {
        id: pc
        property var rows: []
        readonly property var first: rows.length ? rows[0] : ({})
        // The row this PC is connected over, else the first: the state line follows it
        readonly property var lead: {
            for (const c of rows)
                if (home.server.sessionDuid == c.duid)
                    return c;
            return first;
        }
        readonly property bool here: home.server.sessionDuid == lead.duid
        readonly property bool taken: (lead.busy || "").length > 0
        readonly property var app: {
            for (const c of rows)
                if (c.app && c.app.name)
                    return c.app;
            return null;
        }
        // The session on my PS5 (the same on all its rows): one "Çıkar" for the card
        readonly property var live: {
            if (!first.mine || !taken)
                return null;
            for (const c of rows) {
                const m = home.myConsole(c.duid);
                if (m && m.sid && m.sid.length > 0)
                    return m;
            }
            return null;
        }
        Layout.fillWidth: true
        implicitHeight: pcHead.height + pcSubs.implicitHeight + 12

        Item {
            id: pcHead
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
            }
            height: 70
            KonsolSimge {
                id: pcGlyph
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: 4
                width: 64
                height: 18
                tone: pc.taken && !pc.here ? Tema.warm : Tema.ok
            }
            ColumnLayout {
                anchors {
                    left: pcGlyph.right
                    right: pcHeadActions.left
                    verticalCenter: parent.verticalCenter
                    verticalCenterOffset: 4
                    leftMargin: 18
                    rightMargin: 16
                }
                spacing: 3
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        Layout.maximumWidth: implicitWidth
                        Layout.minimumWidth: Math.min(implicitWidth, 80)
                        Layout.alignment: Qt.AlignBaseline
                        text: home.ps5Title(pc.rows)
                        elide: Text.ElideRight
                        font.family: Tema.body
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                        color: Tema.text
                    }
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignBaseline
                        text: qsTr("%1 PSN hesabı").arg(pc.rows.length)
                        elide: Text.ElideRight
                        font.family: Tema.body
                        font.pixelSize: 12
                        color: Tema.faint
                    }
                }
                StateLine {
                    Layout.fillWidth: true
                    tone: pc.taken && !pc.here ? Tema.warm : Tema.ok
                    text: home.psnStateLine(pc.lead, home.psnStateWord(pc.lead), pc.app)
                }
            }
            RowLayout {
                id: pcHeadActions
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: 4
                spacing: 8
                // Someone else on my PS5: I can always take it back
                TextButton {
                    visible: !!pc.live
                    kind: "quiet"
                    tone: Tema.warm
                    text: qsTr("Çıkar")
                    onClicked: home.showConfirmDialog(qsTr("Çıkar"),
                        qsTr("%1 oturumu birkaç saniye içinde kapanır ve sen tekrar açana kadar bu PS5'e bağlanamaz.").arg(pc.lead.busy),
                        () => home.server.kick(pc.live.sid))
                }
            }
        }
        ColumnLayout {
            id: pcSubs
            anchors {
                left: parent.left
                right: parent.right
                top: pcHead.bottom
                leftMargin: 82
            }
            spacing: 0
            Repeater {
                model: pc.rows
                delegate: Item {
                    id: sub
                    required property var modelData
                    readonly property string psn: home.psnOf(modelData)
                    readonly property bool here: home.server.sessionDuid == modelData.duid
                    Layout.fillWidth: true
                    implicitHeight: 44
                    Rectangle {
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                        }
                        height: 1
                        color: Tema.line
                        opacity: 0.7
                    }
                    RowLayout {
                        anchors {
                            left: parent.left
                            right: parent.right
                            verticalCenter: parent.verticalCenter
                        }
                        spacing: 10
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            Layout.maximumWidth: implicitWidth
                            Layout.minimumWidth: Math.min(implicitWidth, 60)
                            Layout.alignment: Qt.AlignBaseline
                            text: sub.modelData.name
                            elide: Text.ElideRight
                            font.family: Tema.body
                            font.pixelSize: 15
                            font.weight: Font.Medium
                            color: Tema.text
                        }
                        Label {
                            textFormat: Text.PlainText
                            visible: sub.psn.length > 0
                            Layout.fillWidth: true
                            Layout.maximumWidth: implicitWidth
                            Layout.minimumWidth: Math.min(implicitWidth, 40)
                            Layout.alignment: Qt.AlignBaseline
                            text: sub.psn
                            elide: Text.ElideRight
                            font.family: Tema.body
                            font.pixelSize: 12
                            color: Tema.faint
                        }
                        // The row this PC is connected over
                        Label {
                            textFormat: Text.PlainText
                            visible: sub.here
                            Layout.alignment: Qt.AlignBaseline
                            text: home.psnStateWord(sub.modelData)
                            font.family: Tema.body
                            font.pixelSize: 12
                            color: Tema.ok
                        }
                        Item { Layout.fillWidth: true }
                        PsnActions {
                            c: sub.modelData
                            compact: true
                            kick: false
                        }
                    }
                }
            }
        }
        // Parted from the rows around it by a hairline, as a plain row
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

    // ── "Canlı yayınlar": friends' and group members' broadcasts on the air (izlet.live). "İzle" plays one in this
    // app's own player, the way a PS5 plays (izlet.watch: no browser) ──
    property bool liveOpen: false
    function openLive() {
        liveOpen = true
        izlet.refreshLive()
    }
    function closeLive() { liveOpen = false }
    Rectangle {
        id: livePage
        visible: home.liveOpen
        z: 50
        color: Tema.bg
        anchors {
            top: topBar.bottom
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            topMargin: 18
        }
        // The list stays fresh while the page is open
        Timer {
            interval: 5000
            repeat: true
            running: home.liveOpen
            onTriggered: izlet.refreshLive()
        }
        Shortcut {
            sequence: "Escape"
            enabled: home.liveOpen
            onActivated: home.closeLive()
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 56
            anchors.rightMargin: 40
            anchors.bottomMargin: 24
            spacing: 14
            RowLayout {
                Layout.fillWidth: true
                spacing: 14
                TextButton {
                    kind: "secondary"
                    compact: true
                    text: qsTr("‹ Geri")
                    onClicked: home.closeLive()
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: qsTr("Canlı yayınlar")
                    font.family: Tema.display
                    font.pixelSize: 26
                    font.weight: Font.DemiBold
                    color: Tema.text
                }
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                Layout.maximumWidth: 760
                wrapMode: Text.WordWrap
                text: qsTr("Arkadaşların ve grup üyelerin oynarken \"İzlet\"i açarsa burada görünür. İzlediğin yayın, PS5 oynar gibi bu uygulamanın oynatıcısında açılır.")
                font.family: Tema.body
                font.pixelSize: 15
                lineHeight: 1.3
                color: Tema.muted
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.maximumWidth: 900
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    width: parent.width
                    spacing: 0
                    EmptyNote {
                        visible: izlet.live.length === 0
                        text: qsTr("Şu an açık yayın yok.")
                    }
                    Repeater {
                        model: izlet.live
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 74
                            color: "transparent"
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Tema.line }
                            RowLayout {
                                anchors.fill: parent
                                spacing: 14
                                Rectangle {
                                    radius: 4
                                    color: Tema.danger
                                    implicitWidth: pageLiveTag.implicitWidth + 12
                                    implicitHeight: pageLiveTag.implicitHeight + 4
                                    Label {
                                        id: pageLiveTag
                                        anchors.centerIn: parent
                                        textFormat: Text.PlainText
                                        text: qsTr("CANLI")
                                        font.family: Tema.body
                                        font.pixelSize: 12
                                        font.weight: Font.Bold
                                        color: "white"
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Label {
                                        textFormat: Text.PlainText
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        text: home.streamerMode ? qsTr("Bir arkadaşın") : modelData.owner
                                        font.family: Tema.body
                                        font.pixelSize: 18
                                        font.weight: Font.DemiBold
                                        color: Tema.text
                                    }
                                    Label {
                                        textFormat: Text.PlainText
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        text: (home.streamerMode ? "PS5" : modelData.console) + " · " + qsTr("%1 izleyici").arg(modelData.viewers)
                                            + (modelData.codec === "h265" ? " · H.265" : "")
                                        font.family: Tema.body
                                        font.pixelSize: 14
                                        color: Tema.muted
                                    }
                                }
                                TextButton {
                                    kind: "primary"
                                    enabled: izlet.canWatch && !oviora.hasSource
                                    text: qsTr("İzle")
                                    onClicked: { home.closeLive(); izlet.watch(modelData.yid) }
                                }
                            }
                        }
                    }
                }
            }
        }
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
        // Friends' live broadcasts, watched on a page of this window (a dot while someone is on the air)
        TextButton {
            visible: home.signedIn && izlet.canWatch
            kind: "quiet"
            text: izlet.live.length > 0 ? qsTr("● Canlı yayınlar (%1)").arg(izlet.live.length) : qsTr("Canlı yayınlar")
            tone: izlet.live.length > 0 ? Tema.danger : Tema.muted
            onClicked: home.liveOpen ? home.closeLive() : home.openLive()
        }
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
    // A group's conversation in the same window (the group's page closes first)
    function openGroupChat(id) {
        if (groupLoader.item) groupLoader.item.popup.close();
        msgs.openGroupChat(id);
        chatLoader.active = true;
        if (chatLoader.item) chatLoader.item.popup.open();
    }
    // Testing: --test-chat <friend> opens that conversation, #<id> a group's (only opens it);
    // --dialog group / groupcreate: the first group's page, the "Grup kur" window
    Timer {
        interval: 9000
        running: typeof vyTest !== "undefined" && (!!vyTest.chat || vyTest.dialog === "group" || vyTest.dialog === "groupcreate")
        onTriggered: {
            if (vyTest.dialog === "group" && home.server.groups.length > 0) home.openGroup(home.server.groups[0].id);
            else if (vyTest.dialog === "groupcreate") home.openGroupCreate();
            else if (String(vyTest.chat).indexOf("#") === 0) home.openGroupChat(parseInt(String(vyTest.chat).substring(1)));
            else if (vyTest.chat) home.openChat(vyTest.chat);
        }
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
        onOpened: chatView.focusField()
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
                    text: msgs.chatTitle
                    elide: Text.ElideRight
                    font.family: Tema.display
                    font.pixelSize: 26
                    font.weight: Font.ExtraBold
                    color: Tema.text
                }
                // A group: its members and tools; the safety code is compared in each member's own chat
                TextButton {
                    kind: "quiet"
                    visible: msgs.chatIsGroup
                    text: qsTr("Üyeler")
                    onClicked: home.openGroup(msgs.chatGroup)
                }
                TextButton {
                    kind: "quiet"
                    visible: !msgs.chatIsGroup
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
                text: msgs.chatIsGroup
                    ? qsTr("Uçtan uca şifreli: mesajları yalnız grubun üyeleri okuyabilir, Oviora sunucusu bile göremez.")
                    : qsTr("Uçtan uca şifreli: mesajları yalnız sen ve %1 okuyabilir, Oviora sunucusu bile göremez.").arg(msgs.chatWith)
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
                    text: msgs.chatIsGroup
                        ? qsTr("Grupta %1 için güvenlik anahtarı değişti. Yeni bir cihaz kurmuş olabilir; emin olmak için güvenlik kodunu onunla birebir sohbetinizde karşılaştır.").arg(msgs.chatKeyChangedNames.join(", "))
                        : qsTr("%1 için güvenlik anahtarı değişti. Yeni bir cihaz kurmuş olabilir; emin olmak için güvenlik kodunu onunla karşılaştır.").arg(msgs.chatWith)
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
            // The bubbles and the line to write in (shared with the stream menu's panel)
            ChatView {
                id: chatView
                visible: !chatPopup.showCode
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }
            }
        }
    }
    Connections {
        target: msgs
        // Over a stream the main window shows its own card on the picture (Main.qml messageToast)
        function onIncoming(from, preview, group, groupName) {
            if (home.streamShown) return;
            home.showToast(group ? groupName + " · " + from : qsTr("%1 mesaj gönderdi").arg(from), preview);
        }
        function onFailed(text) { home.showToast(qsTr("Mesaj gönderilemedi"), text); }
    }

    // ── A friend group's page: the members and what they do now; the owner renames, adds, removes and
    // deletes; anyone can leave. Everything it shows comes from OvioraServer.groups ──
    property int groupPageId: 0
    function openGroup(id) {
        groupPageId = id;
        groupLoader.active = true;
        if (groupLoader.item) groupLoader.item.popup.open();
    }
    function openGroupCreate() {
        groupCreateLoader.active = true;
        if (groupCreateLoader.item) groupCreateLoader.item.popup.start();
    }
    Connections {
        target: home.server
        // A group I just set up: straight to its page
        function onGroupCreated(id) { if (id) home.openGroup(id); }
    }
    Loader {
        id: groupLoader
        active: false
        sourceComponent: Component {
            Item {
                property alias popup: groupPopup
                Popup {
                    id: groupPopup
                    parent: Overlay.overlay
                    x: Math.round((home.Window.width - width) / 2)
                    y: Math.round((home.Window.height - height) / 2)
                    width: Math.min(560, home.width - 64)
                    height: Math.min(implicitHeight, home.Window.height - 64)
                    modal: true
                    dim: true
                    padding: 24
                    // Read again with every new groups view
                    readonly property var group: { home.server.groups; return home.server.group(home.groupPageId); }
                    readonly property var members: group.members || []
                    readonly property bool mine: !!group.mine
                    readonly property bool full: members.length >= home.server.groupMembersMax
                    // "": the members; "rename": the new name; "add": friends to add
                    property string mode: ""
                    // My friends who are not in the group yet
                    readonly property var candidates: {
                        const inGroup = members.map(m => (m.name || "").toLowerCase());
                        return (home.server.friends || []).filter(f => f && f.name && inGroup.indexOf(f.name.toLowerCase()) < 0);
                    }
                    onClosed: mode = ""
                    // Left, deleted or removed from it: nothing left to show
                    onGroupChanged: if (opened && group.id === undefined) close()
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
                                text: groupPopup.group.name || ""
                                elide: Text.ElideRight
                                font.family: Tema.display
                                font.pixelSize: 26
                                font.weight: Font.ExtraBold
                                color: Tema.text
                            }
                            TextButton {
                                kind: "quiet"
                                text: qsTr("Kapat")
                                onClicked: groupPopup.close()
                            }
                        }
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: (groupPopup.mine ? qsTr("Grubu sen yönetiyorsun") : qsTr("Yönetici: %1").arg(groupPopup.group.owner || ""))
                                + " · " + qsTr("%1/%2 kişi").arg(groupPopup.members.length).arg(home.server.groupMembersMax)
                            font.family: Tema.body
                            font.pixelSize: 13
                            color: Tema.faint
                        }
                        // The owner's tools
                        RowLayout {
                            visible: groupPopup.mine && groupPopup.mode === ""
                            spacing: 4
                            Layout.leftMargin: -10
                            TextButton {
                                kind: "quiet"
                                tone: Tema.accentText
                                implicitHeight: 32
                                text: qsTr("Yeniden adlandır")
                                onClicked: {
                                    renameField.text = groupPopup.group.name || "";
                                    groupPopup.mode = "rename";
                                    renameField.forceActiveFocus();
                                }
                            }
                            TextButton {
                                kind: "quiet"
                                tone: Tema.accentText
                                implicitHeight: 32
                                enabled: !groupPopup.full
                                text: qsTr("Üye ekle")
                                onClicked: groupPopup.mode = "add"
                            }
                        }
                        // A new name
                        RowLayout {
                            visible: groupPopup.mode === "rename"
                            Layout.fillWidth: true
                            spacing: 8
                            Field {
                                id: renameField
                                placeholderText: qsTr("Grubun adı")
                                maximumLength: 40
                                font.pixelSize: 15
                                topPadding: 9
                                bottomPadding: 9
                                leftPadding: 14
                                onAccepted: renameSave.clicked()
                            }
                            TextButton {
                                id: renameSave
                                kind: "primary"
                                text: qsTr("Değiştir")
                                enabled: renameField.text.trim().length > 0
                                onClicked: {
                                    if (!enabled) return;
                                    home.server.groupRename(groupPopup.group.id, renameField.text);
                                    groupPopup.mode = "";
                                }
                            }
                            TextButton { kind: "quiet"; text: qsTr("Vazgeç"); onClicked: groupPopup.mode = "" }
                        }
                        // Friends to add (only the owner's own friends can be added)
                        ColumnLayout {
                            visible: groupPopup.mode === "add"
                            Layout.fillWidth: true
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                SectionLabel { text: qsTr("Eklenecek arkadaş") }
                                Item { Layout.fillWidth: true }
                                TextButton { kind: "quiet"; implicitHeight: 32; text: qsTr("Bitti"); onClicked: groupPopup.mode = "" }
                            }
                            EmptyNote {
                                visible: groupPopup.candidates.length === 0
                                text: qsTr("Bütün arkadaşların zaten bu grupta.")
                            }
                            Repeater {
                                model: groupPopup.candidates
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 12
                                    Label {
                                        textFormat: Text.PlainText
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        elide: Text.ElideRight
                                        font.family: Tema.body
                                        font.pixelSize: 15
                                        font.weight: Font.DemiBold
                                        color: Tema.text
                                    }
                                    TextButton {
                                        kind: "quiet"
                                        tone: Tema.accentText
                                        implicitHeight: 32
                                        enabled: !groupPopup.full
                                        text: qsTr("Ekle")
                                        onClicked: home.server.groupAdd(groupPopup.group.id, modelData.name)
                                    }
                                }
                            }
                        }
                        // The members, in the order they joined
                        ListView {
                            id: memberList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredHeight: Math.min(contentHeight, 340)
                            Layout.topMargin: 4
                            clip: true
                            spacing: 0
                            model: groupPopup.members
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar { policy: memberList.contentHeight > memberList.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }
                            delegate: Item {
                                id: memberRow
                                required property var modelData
                                width: ListView.view.width
                                implicitHeight: 60
                                Rectangle {
                                    anchors { left: parent.left; right: parent.right; top: parent.top }
                                    height: 1
                                    color: Tema.line
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    spacing: 12
                                    Rectangle {
                                        implicitWidth: 30
                                        implicitHeight: 30
                                        radius: 15
                                        color: Tema.surface2
                                        border.color: Tema.line
                                        Label {
                                            textFormat: Text.PlainText
                                            anchors.centerIn: parent
                                            text: home.initials(memberRow.modelData.name)
                                            font.family: Tema.body
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                            color: Tema.text
                                        }
                                        Rectangle {
                                            anchors.right: parent.right
                                            anchors.bottom: parent.bottom
                                            anchors.margins: -1
                                            width: 10
                                            height: 10
                                            radius: 5
                                            color: home.presenceOn(memberRow.modelData.presence) ? Tema.ok : Tema.faint
                                            border.width: 2
                                            border.color: Tema.surface
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Label {
                                                textFormat: Text.PlainText
                                                Layout.maximumWidth: memberRow.width - 200
                                                text: memberRow.modelData.me ? qsTr("%1 (sen)").arg(memberRow.modelData.name) : memberRow.modelData.name
                                                elide: Text.ElideRight
                                                font.family: Tema.body
                                                font.pixelSize: 15
                                                font.weight: Font.DemiBold
                                                color: Tema.text
                                            }
                                            Label {
                                                id: roleTag
                                                textFormat: Text.PlainText
                                                visible: memberRow.modelData.role === "owner"
                                                text: qsTr("yönetici")
                                                font.family: Tema.body
                                                font.pixelSize: 12
                                                font.weight: Font.DemiBold
                                                color: Tema.accentText
                                            }
                                            Item { Layout.fillWidth: true }
                                        }
                                        Label {
                                            textFormat: Text.PlainText
                                            Layout.fillWidth: true
                                            text: home.presenceLine(memberRow.modelData.presence)
                                            elide: Text.ElideRight
                                            font.family: Tema.body
                                            font.pixelSize: 13
                                            color: home.presencePlaying(memberRow.modelData.presence) ? Tema.ok : Tema.muted
                                        }
                                    }
                                    TextButton {
                                        visible: groupPopup.mine && !memberRow.modelData.me
                                        kind: "quiet"
                                        implicitHeight: 32
                                        text: qsTr("Çıkar")
                                        onClicked: home.showConfirmDialog(qsTr("Gruptan çıkar"),
                                            qsTr("%1, %2 grubundan çıkarılsın mı?").arg(memberRow.modelData.name).arg(groupPopup.group.name),
                                            () => home.server.groupRemove(groupPopup.group.id, memberRow.modelData.name))
                                    }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 6
                            spacing: 8
                            TextButton {
                                kind: "primary"
                                text: home.groupUnreadOf(groupPopup.group.id) > 0
                                    ? qsTr("Mesaj (%1)").arg(home.groupUnreadOf(groupPopup.group.id)) : qsTr("Mesaj yaz")
                                onClicked: home.openGroupChat(groupPopup.group.id)
                            }
                            Item { Layout.fillWidth: true }
                            TextButton {
                                visible: groupPopup.mine
                                kind: "quiet"
                                tone: Tema.danger
                                text: qsTr("Grubu sil")
                                onClicked: home.showConfirmDialog(qsTr("Grubu sil"),
                                    qsTr("%1 grubu herkes için silinsin mi? Gruba gönderilen ve henüz alınmamış mesajlar da silinir.").arg(groupPopup.group.name),
                                    () => home.server.groupDelete(groupPopup.group.id))
                            }
                            TextButton {
                                kind: "quiet"
                                text: qsTr("Gruptan ayrıl")
                                onClicked: home.showConfirmDialog(qsTr("Gruptan ayrıl"),
                                    groupPopup.mine && groupPopup.members.length > 1
                                        ? qsTr("%1 grubundan ayrılmak istiyor musun? Yönetim, gruba en önce katılmış üyeye geçer.").arg(groupPopup.group.name)
                                        : qsTr("%1 grubundan ayrılmak istiyor musun? Grubun mesajları artık sana gelmez.").arg(groupPopup.group.name),
                                    () => home.server.groupRemove(groupPopup.group.id, home.server.user))
                            }
                        }
                    }
                }
            }
        }
    }

    // ── "Grup kur": a name and the friends to add ──
    Loader {
        id: groupCreateLoader
        active: false
        sourceComponent: Component {
            Item {
                property alias popup: groupCreatePopup
                Popup {
                    id: groupCreatePopup
                    parent: Overlay.overlay
                    x: Math.round((home.Window.width - width) / 2)
                    y: Math.round((home.Window.height - height) / 2)
                    width: Math.min(500, home.width - 64)
                    height: Math.min(implicitHeight, home.Window.height - 64)
                    modal: true
                    dim: true
                    padding: 24
                    // The friends ticked, by name
                    property var picked: []
                    readonly property var friends: home.server.friends || []
                    function start() {
                        groupNameField.text = "";
                        picked = [];
                        open();
                        groupNameField.forceActiveFocus();
                    }
                    function toggle(name, on) {
                        picked = on ? picked.concat([name]) : picked.filter(n => n !== name);
                    }
                    readonly property bool tooMany: picked.length + 1 > home.server.groupMembersMax
                    background: Rectangle {
                        radius: Tema.radiusPanel
                        color: Tema.surface
                        border.color: Tema.lineStrong
                    }
                    contentItem: ColumnLayout {
                        spacing: 12
                        Label {
                            textFormat: Text.PlainText
                            text: qsTr("Grup kur")
                            font.family: Tema.display
                            font.pixelSize: 26
                            font.weight: Font.ExtraBold
                            color: Tema.text
                        }
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Gruba bir ad ver ve eklemek istediğin arkadaşlarını seç. Grubu sen yönetirsin; sonradan da üye ekleyebilirsin.")
                            font.family: Tema.body
                            font.pixelSize: 14
                            lineHeight: 1.2
                            color: Tema.muted
                        }
                        Field {
                            id: groupNameField
                            placeholderText: qsTr("Grubun adı")
                            maximumLength: 40
                            font.pixelSize: 15
                            topPadding: 10
                            bottomPadding: 10
                            leftPadding: 14
                        }
                        SectionLabel {
                            Layout.topMargin: 4
                            text: qsTr("Arkadaşlar")
                        }
                        EmptyNote {
                            visible: groupCreatePopup.friends.length === 0
                            Layout.topMargin: 0
                            text: qsTr("Henüz arkadaşın yok. Grubu yalnız kurup arkadaş ekledikçe onları da ekleyebilirsin.")
                        }
                        ListView {
                            id: pickList
                            visible: count > 0
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredHeight: Math.min(contentHeight, 300)
                            clip: true
                            model: groupCreatePopup.friends
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar { policy: pickList.contentHeight > pickList.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }
                            delegate: AbstractButton {
                                id: pickRow
                                required property var modelData
                                readonly property bool on: groupCreatePopup.picked.indexOf(modelData.name) >= 0
                                width: ListView.view.width
                                implicitHeight: 44
                                focusPolicy: Qt.NoFocus
                                hoverEnabled: true
                                onClicked: groupCreatePopup.toggle(modelData.name, !on)
                                background: Rectangle {
                                    radius: 10
                                    color: pickRow.down ? Tema.line : (pickRow.hovered ? Tema.surface2 : "transparent")
                                }
                                contentItem: RowLayout {
                                    spacing: 12
                                    Item { implicitWidth: 2 }
                                    // A tick box in the brand colour
                                    Rectangle {
                                        implicitWidth: 20
                                        implicitHeight: 20
                                        radius: 6
                                        color: pickRow.on ? Tema.accent : "transparent"
                                        border.color: pickRow.on ? Tema.accent : Tema.lineStrong
                                        border.width: 1.5
                                        Rectangle {
                                            visible: pickRow.on
                                            anchors.centerIn: parent
                                            width: 8
                                            height: 8
                                            radius: 2
                                            color: Tema.inkOnAccent
                                        }
                                    }
                                    Label {
                                        textFormat: Text.PlainText
                                        text: pickRow.modelData.name
                                        elide: Text.ElideRight
                                        font.family: Tema.body
                                        font.pixelSize: 15
                                        font.weight: Font.DemiBold
                                        color: Tema.text
                                    }
                                    Label {
                                        textFormat: Text.PlainText
                                        Layout.fillWidth: true
                                        text: home.presenceLine(pickRow.modelData.presence)
                                        elide: Text.ElideRight
                                        font.family: Tema.body
                                        font.pixelSize: 13
                                        color: Tema.faint
                                    }
                                }
                            }
                        }
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: groupCreatePopup.tooMany
                                ? qsTr("Bir grupta en çok %1 kişi olabilir (sen dahil).").arg(home.server.groupMembersMax)
                                : qsTr("%1 arkadaş seçildi").arg(groupCreatePopup.picked.length)
                            font.family: Tema.body
                            font.pixelSize: 13
                            color: groupCreatePopup.tooMany ? Tema.danger : Tema.faint
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignRight
                            spacing: 8
                            TextButton { kind: "quiet"; text: qsTr("Vazgeç"); onClicked: groupCreatePopup.close() }
                            TextButton {
                                kind: "primary"
                                text: qsTr("Kur")
                                enabled: groupNameField.text.trim().length > 0 && !groupCreatePopup.tooMany
                                onClicked: {
                                    home.server.groupCreate(groupNameField.text, groupCreatePopup.picked);
                                    groupCreatePopup.close();
                                }
                            }
                        }
                    }
                }
            }
        }
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
        id: signInColumn
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
            id: consoleScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.maximumWidth: 900
            contentWidth: availableWidth
            clip: true
            // The scroll bar gets its own lane (it used to lie over the Bağlan buttons)
            rightPadding: 14

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

                // ── Friends on the air (İzlet): watched in a window of its own (izlet.live, izlet.watch) ──
                ColumnLayout {
                    visible: izlet.live.length > 0
                    Layout.fillWidth: true
                    Layout.bottomMargin: 26
                    spacing: 8
                    Connections {
                        target: izlet
                        function onWatchFailed(text) { home.showToast(qsTr("İzle"), text) }
                    }
                    SectionLabel {
                        text: qsTr("Canlı yayınlar")
                        info: qsTr("Arkadaşların ve grup üyelerin oynadıklarını izletiyor. \"İzle\" yayını bu uygulamanın oynatıcısında açar.")
                    }
                    Repeater {
                        model: izlet.live
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.topMargin: 2
                            spacing: 12
                            Rectangle {
                                radius: 4
                                color: Tema.danger
                                implicitWidth: liveTag.implicitWidth + 12
                                implicitHeight: liveTag.implicitHeight + 4
                                Label {
                                    id: liveTag
                                    anchors.centerIn: parent
                                    textFormat: Text.PlainText
                                    text: qsTr("CANLI")
                                    font.family: Tema.body
                                    font.pixelSize: 12
                                    font.weight: Font.Bold
                                    color: "white"
                                }
                            }
                            Label {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: home.streamerMode ? qsTr("Bir arkadaşın") : modelData.owner + " · " + modelData.console
                                font.family: Tema.body
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                                color: Tema.text
                            }
                            Label {
                                textFormat: Text.PlainText
                                text: qsTr("%1 izleyici").arg(modelData.viewers)
                                font.family: Tema.body
                                font.pixelSize: 14
                                color: Tema.muted
                            }
                            TextButton {
                                kind: "primary"
                                compact: true
                                enabled: izlet.canWatch
                                text: qsTr("İzle")
                                onClicked: izlet.watch(modelData.yid)
                            }
                        }
                    }
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
                        visible: home.server.psnLinks.length > 0 && home.server.psnLinks.length < home.server.psnLinksMax
                        enabled: !home.server.busy
                        text: qsTr("PSN hesabı ekle")
                        onClicked: home.showPsnLinkDialog()
                    }
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

                // One entry per physical PS5 (home.ps5Groups): the plain row when it is on one PSN account,
                // a PS5 card with a sub-row per account when on several
                Repeater {
                    model: home.ps5Groups(home.server.available)
                    delegate: Loader {
                        id: psnEntry
                        required property var modelData
                        Layout.fillWidth: true
                        sourceComponent: modelData.rows.length > 1 ? ps5CardComponent : psnRowComponent
                        Component {
                            id: psnRowComponent
                            PsnRow { c: psnEntry.modelData.rows[0] }
                        }
                        Component {
                            id: ps5CardComponent
                            Ps5Card { rows: psnEntry.modelData.rows }
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
            id: friendScroll
            Layout.preferredWidth: 360
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            // The scroll bar gets its own lane on the right (it used to lie over the Kaldır buttons)
            rightPadding: 14

            ColumnLayout {
                width: parent.width
                spacing: 0

                // ── Friend groups: a row per group, its messages and its page (members, owner's tools) ──
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 14
                    SectionLabel {
                        text: qsTr("Gruplar")
                        info: qsTr("Arkadaşlarınla bir grup kur; grupta herkes uçtan uca şifreli olarak birlikte yazışır. Grubu kuran kişi yönetir, herkes istediğinde ayrılabilir.")
                    }
                    Item { Layout.fillWidth: true }
                    TextButton {
                        kind: "quiet"
                        text: qsTr("Grup kur")
                        enabled: home.server.groups.length < home.server.groupsMax
                        onClicked: home.openGroupCreate()
                    }
                }
                EmptyNote {
                    visible: home.server.groups.length == 0
                    text: qsTr("Henüz bir grubun yok.")
                }
                Repeater {
                    model: home.server.groups
                    delegate: ColumnLayout {
                        id: groupRow
                        required property var modelData
                        readonly property int unread: home.groupUnreadOf(modelData.id)
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            Layout.bottomMargin: 4
                            spacing: 12
                            // A rounded square, not a circle: a group, not a person
                            Rectangle {
                                implicitWidth: 30
                                implicitHeight: 30
                                radius: 9
                                color: Tema.surface
                                border.color: groupRow.modelData.playing > 0 ? Tema.ok : Tema.line
                                Label {
                                    textFormat: Text.PlainText
                                    anchors.centerIn: parent
                                    text: home.initials(groupRow.modelData.name)
                                    font.family: Tema.body
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    color: Tema.text
                                }
                            }
                            // Name with the summary under it, beside the buttons (they used to overlap the summary)
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: groupRow.modelData.name
                                    elide: Text.ElideRight
                                    font.family: Tema.body
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                    color: Tema.text
                                }
                                Label {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: home.groupSummary(groupRow.modelData)
                                    font.family: Tema.body
                                    font.pixelSize: 13
                                    color: groupRow.modelData.playing > 0 ? Tema.ok : Tema.muted
                                }
                            }
                            TextButton {
                                kind: groupRow.unread > 0 ? "primary" : "quiet"
                                text: groupRow.unread > 0 ? qsTr("Mesaj (%1)").arg(groupRow.unread) : qsTr("Mesaj")
                                implicitHeight: 32
                                onClicked: home.openGroupChat(groupRow.modelData.id)
                            }
                            TextButton {
                                kind: "quiet"
                                text: qsTr("Üyeler")
                                implicitHeight: 32
                                onClicked: home.openGroup(groupRow.modelData.id)
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.topMargin: 10
                            implicitHeight: 1
                            color: Tema.line
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 26
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
                                // playing or online: a green light on the circle, grey when offline
                                Rectangle {
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.margins: -1
                                    width: 10
                                    height: 10
                                    radius: 5
                                    color: home.presenceOn(friendRow.modelData.presence) ? Tema.ok : Tema.faint
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
                        // What the friend does now: "Oynuyor: GTA VI · Ahmet'in PS5'inde · 25 dk", "Çevrimdışı · 2 sa önce"
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            Layout.leftMargin: 42
                            Layout.topMargin: 2
                            Layout.bottomMargin: 4
                            wrapMode: Text.WordWrap
                            text: home.presenceLine(friendRow.modelData.presence)
                            font.family: Tema.body
                            font.pixelSize: 13
                            color: home.presencePlaying(friendRow.modelData.presence) ? Tema.ok : Tema.muted
                        }
                        // Which of my PS5s this friend may use: one line, the switches in "PS5 paylaş"
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 42
                            spacing: 6
                            visible: home.server.consoles.length > 0
                            Label {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: home.shareSummary(friendRow.modelData.name)
                                elide: Text.ElideRight
                                font.family: Tema.body
                                font.pixelSize: 13
                                color: Tema.muted
                            }
                            TextButton {
                                kind: "quiet"
                                text: qsTr("PS5 paylaş")
                                implicitHeight: 28
                                onClicked: home.openShareConsoles(friendRow.modelData.name)
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
        // Over any other window that asks (the "PS5 paylaş" list opens the share question over itself)
        z: 100
        x: Math.round((home.Window.width - width) / 2)
        y: Math.round((home.Window.height - height) / 2)
        width: Math.min(460, home.width - 64)
        modal: true
        dim: true
        padding: 24
        property var action: null
        property var cancelAction: null
        property string title: ""
        property string text: ""
        property int wait: 0   // seconds before "Evet" can be pressed
        function ask(t, x, a, waitSeconds, onCancel) {
            title = t; text = x; action = a; cancelAction = onCancel || null; wait = waitSeconds || 0; open()
        }
        function tell(t, x) { title = t; text = x; action = null; cancelAction = null; wait = 0; open() }
        Timer { interval: 1000; repeat: true; running: confirmPopup.opened && confirmPopup.wait > 0; onTriggered: confirmPopup.wait-- }
        // Closed any way but "Evet" (Vazgeç, Esc, a click outside): the cancel action
        property bool accepted: false
        onAboutToShow: accepted = false
        onClosed: { const c = cancelAction; cancelAction = null; if (!accepted && c) c(); }
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
                    enabled: confirmPopup.wait <= 0
                    text: confirmPopup.action === null ? qsTr("Tamam")
                        : confirmPopup.wait > 0 ? qsTr("Evet (%1)").arg(confirmPopup.wait) : qsTr("Evet")
                    onClicked: { const a = confirmPopup.action; confirmPopup.accepted = true; confirmPopup.close(); if (a) a(); }
                }
            }
        }
    }
            }
        }
    }

    // "Adını değiştir" on my own PS5: a new name for it in Oviora; empty brings PSN's own name back
    function showConsoleRename(duid, name) {
        consoleRenameLoader.active = true;
        if (consoleRenameLoader.item) consoleRenameLoader.item.popup.start(duid, name);
    }
    // Testing: --dialog consolerename opens it for the first own PS5 of the list (with --test-psn-links a
    // made-up one, whose "Kaydet" sends nothing)
    Timer {
        interval: 6000
        running: typeof vyTest !== "undefined" && vyTest.dialog === "consolerename"
        onTriggered: {
            for (const c of (home.server.available || []))
                if (c && c.mine) {
                    home.showConsoleRename(c.duid, c.name);
                    return;
                }
        }
    }
    Loader {
        id: consoleRenameLoader
        active: false
        sourceComponent: Component {
            Item {
                property alias popup: consoleRename
                Popup {
                    id: consoleRename
        parent: Overlay.overlay
        x: Math.round((home.Window.width - width) / 2)
        y: Math.round((home.Window.height - height) / 2)
        width: Math.min(460, home.width - 64)
        modal: true
        dim: true
        padding: 24
        property string duid: ""
        function start(d, name) {
            duid = d;
            consoleNameField.text = name || "";
            open();
            consoleNameField.forceActiveFocus();
            consoleNameField.selectAll();
        }
        function save() {
            home.server.renameConsole(duid, consoleNameField.text.trim());
            close();
        }
        background: Rectangle { radius: Tema.radiusPanel; color: Tema.surface; border.color: Tema.lineStrong }
        contentItem: ColumnLayout {
            spacing: 14
            Label { textFormat: Text.PlainText; text: qsTr("Konsolun adı"); font.family: Tema.display; font.pixelSize: 22; color: Tema.text; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            Field {
                id: consoleNameField
                placeholderText: qsTr("PS5'in adı")
                maximumLength: 40
                onAccepted: consoleRename.save()
            }
            Label { textFormat: Text.PlainText; text: qsTr("Boş bırakırsan PSN'deki adı geri gelir."); font.family: Tema.body; font.pixelSize: Tema.smallSize; color: Tema.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                TextButton { kind: "quiet"; text: qsTr("Vazgeç"); onClicked: consoleRename.close() }
                TextButton { kind: "primary"; text: qsTr("Kaydet"); onClicked: consoleRename.save() }
            }
        }
    }
            }
        }
    }

    // ── "PS5 paylaş": my PS5s for one friend, a switch each (searchable when there are many) ──
    function openShareConsoles(friend) {
        shareConsolesLoader.active = true;
        if (shareConsolesLoader.item) shareConsolesLoader.item.popup.start(friend);
    }
    // Testing: --dialog shareconsoles opens it for the first friend (with --test-groups --test-psn-links made-up
    // friends and PS5s, whose switches send nothing)
    Timer {
        interval: 6000
        running: typeof vyTest !== "undefined" && vyTest.dialog === "shareconsoles"
        onTriggered: {
            const f = home.server.friends || [];
            if (f.length > 0 && home.server.consoles.length > 0)
                home.openShareConsoles(f[0].name);
        }
    }
    // Testing: --dialog psnlink shows the "PSN hesabını bağla" popup only (no sign-in address is asked for,
    // so no page loads)
    Timer {
        interval: 6000
        running: typeof vyTest !== "undefined" && vyTest.dialog === "psnlink"
        onTriggered: {
            psnLinkLoader.active = true;
            if (psnLinkLoader.item) psnLinkLoader.item.popup.open();
        }
    }
    Loader {
        id: shareConsolesLoader
        active: false
        sourceComponent: Component {
            Item {
                property alias popup: shareConsoles
                Popup {
                    id: shareConsoles
                    parent: Overlay.overlay
                    x: Math.round((home.Window.width - width) / 2)
                    y: Math.round((home.Window.height - height) / 2)
                    width: Math.min(480, home.width - 64)
                    height: Math.min(implicitHeight, home.Window.height - 64)
                    modal: true
                    dim: true
                    padding: 24
                    property string friend: ""
                    onClosed: friend = ""
                    readonly property var consoles: home.server.consoles || []
                    // Many PS5s: a search field over the list
                    readonly property bool many: consoles.length > 6
                    // The consoles shown, as { i: index into home.server.consoles, head, sub }. A new list whenever the account
                    // view or target friend changes, so the switches are made again from the server's word: a failed share goes
                    // back off.
                    readonly property var rows: {
                        if (!friend.length)
                            return [];
                        const tr = Qt.locale("tr_TR");
                        const q = many ? shareSearch.text.trim().toLocaleLowerCase(tr) : "";
                        const keep = {};
                        for (let i = 0; i < consoles.length; i++) {
                            const c = consoles[i];
                            if (!c)
                                continue;
                            if (q.length && (c.name || "").toLocaleLowerCase(tr).indexOf(q) < 0
                                    && psnName(c).toLocaleLowerCase(tr).indexOf(q) < 0)
                                continue;
                            keep[i] = true;
                        }
                        // The rows of one PS5 (one per PSN account) together; over them a small header
                        const out = [];
                        for (const g of home.ps5Groups(consoles)) {
                            const shown = g.indexes.filter(i => keep[i]);
                            for (let k = 0; k < shown.length; k++)
                                out.push({ i: shown[k], sub: g.rows.length > 1,
                                           head: k === 0 && g.rows.length > 1 ? home.ps5Title(g.rows) + " · " + qsTr("%1 PSN hesabı").arg(g.rows.length) : "" });
                        }
                        return out;
                    }
                    // The PSN account the PS5 is on, as on the home rows ("" in streamer mode)
                    function psnName(c) {
                        return c && !home.streamerMode ? (c.psn_name || "").trim() : "";
                    }
                    function start(f) {
                        friend = f;
                        shareSearch.text = "";
                        open();
                        if (many)
                            shareSearch.forceActiveFocus();
                    }
                    background: Rectangle {
                        radius: Tema.radiusPanel
                        color: Tema.surface
                        border.color: Tema.lineStrong
                    }
                    contentItem: ColumnLayout {
                        spacing: 12
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: qsTr("%1 ile paylaş").arg(shareConsoles.friend)
                            elide: Text.ElideRight
                            font.family: Tema.display
                            font.pixelSize: 22
                            color: Tema.text
                        }
                        Field {
                            id: shareSearch
                            visible: shareConsoles.many
                            placeholderText: qsTr("PS5 ara")
                            font.pixelSize: 15
                            topPadding: 9
                            bottomPadding: 9
                            leftPadding: 14
                        }
                        EmptyNote {
                            visible: shareConsoles.rows.length === 0
                            Layout.topMargin: 0
                            text: qsTr("Bu adda bir PS5 yok.")
                        }
                        ScrollView {
                            id: shareScroll
                            visible: shareConsoles.rows.length > 0
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredHeight: Math.min(shareList.implicitHeight, 440)
                            Layout.minimumHeight: Math.min(shareList.implicitHeight, 120)
                            contentWidth: availableWidth
                            clip: true
                            // The scroll bar gets its own lane on the right, off the switches
                            rightPadding: 14

                            ColumnLayout {
                                id: shareList
                                width: parent.width
                                spacing: 0
                                Repeater {
                                    model: shareConsoles.rows
                                    delegate: ColumnLayout {
                                        id: shareEntry
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: 0
                                        // The PS5 over its rows when it is on several PSN accounts
                                        Label {
                                            textFormat: Text.PlainText
                                            visible: shareEntry.modelData.head.length > 0
                                            Layout.fillWidth: true
                                            Layout.topMargin: 8
                                            Layout.leftMargin: 2
                                            text: shareEntry.modelData.head
                                            elide: Text.ElideRight
                                            font.family: Tema.body
                                            font.pixelSize: 12
                                            font.weight: Font.DemiBold
                                            color: Tema.muted
                                        }
                                        Basic.Switch {
                                            id: shareSwitch
                                            readonly property int modelData: shareEntry.modelData.i
                                            // The live entry (home.server.consoles): its "shared" changes when the server answers
                                            readonly property var con: shareConsoles.consoles[modelData]
                                            readonly property string psn: shareConsoles.psnName(con)
                                            Layout.fillWidth: true
                                            checked: !!con && (con.shared || []).indexOf(shareConsoles.friend) >= 0
                                            text: con ? con.name : ""
                                            font.family: Tema.body
                                            font.pixelSize: 15
                                            topPadding: 8
                                            bottomPadding: 8
                                            leftPadding: shareEntry.modelData.sub ? 14 : 2
                                            // Giving a friend the PS5 asks first (over this window), and "Evet" waits
                                            // 3 s; taking it back does not ask
                                            onToggled: {
                                                if (!con)
                                                    return;
                                                if (!checked) {
                                                    home.server.setShare(con.duid, shareConsoles.friend, false);
                                                    return;
                                                }
                                                checked = false;
                                                const sw = shareSwitch, duid = con.duid, ps5 = con.name, friend = shareConsoles.friend;
                                                home.showSlowConfirmDialog(qsTr("PS5'ini paylaş"),
                                                    qsTr("%1, %2 konsoluna internet üzerinden bağlanıp oynayabilecek; o oynarken sen bağlanamazsın. Paylaşmak istediğine emin misin?").arg(friend).arg(ps5),
                                                    () => { if (sw) sw.checked = true; home.server.setShare(duid, friend, true); },
                                                    () => { if (sw) sw.checked = false; });
                                            }
                                            contentItem: RowLayout {
                                                spacing: 10
                                                Item { implicitWidth: shareSwitch.indicator.width }
                                                Label {
                                                    textFormat: Text.PlainText
                                                    Layout.fillWidth: shareSwitch.psn.length === 0
                                                    Layout.maximumWidth: implicitWidth
                                                    Layout.minimumWidth: Math.min(implicitWidth, 80)
                                                    Layout.alignment: Qt.AlignBaseline
                                                    text: shareSwitch.text
                                                    elide: Text.ElideRight
                                                    font: shareSwitch.font
                                                    color: shareSwitch.checked ? Tema.text : Tema.muted
                                                }
                                                Label {
                                                    textFormat: Text.PlainText
                                                    visible: shareSwitch.psn.length > 0
                                                    Layout.fillWidth: true
                                                    Layout.alignment: Qt.AlignBaseline
                                                    text: shareSwitch.psn
                                                    elide: Text.ElideRight
                                                    font.family: Tema.body
                                                    font.pixelSize: 12
                                                    color: Tema.faint
                                                }
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
                                }
                            }
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignRight
                            spacing: 8
                            TextButton { kind: "primary"; text: qsTr("Kapat"); onClicked: shareConsoles.close() }
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
        // Oyun bilgisi: the linked PSN account (> 0) this sign-in is for, and its name as shown
        property int wideLink: 0
        property string wideName: ""
        // "Oyun bilgisini de aç" (an ordinary link only): the same sign-in also turns Oyun bilgisi on; after
        // the redirect the page stays, hidden, and C++ reads Sony's sign-in for the linked account
        property bool alsoWide: true
        function start(link, name) {
            url = ""; sent = false; wideLink = link > 0 ? link : 0; wideName = name || ""; alsoWide = true;
            open(); platform.requestPsnLoginUrl()
        }
        function show() {
            if (!opened) open();
            const p = webArea.mapToItem(null, 0, 0);
            psnWeb.open(url, p.x, p.y, webArea.width, webArea.height);
            psnWeb.setWideLink(wideLink);
            psnWeb.setLinkWide(wideLink <= 0 && alsoWide);
        }
        onAlsoWideChanged: if (opened && !sent) psnWeb.setLinkWide(wideLink <= 0 && alsoWide)
        // The browser is a native window placed by numbers: when the popup opens during a page change (from
        // Ayarlar the home page slides in), the first numbers are off. Kept in step while the popup is up.
        Timer {
            interval: 150
            repeat: true
            running: psnLink.opened && psnLink.url.length > 0
            onTriggered: psnLink.place()
        }
        function place() {
            if (!opened || !url.length) return;
            const p = webArea.mapToItem(null, 0, 0);
            psnWeb.setRect(p.x, p.y, webArea.width, webArea.height);
        }
        // A page held for "Oyun bilgisini de aç" closes itself once it has read (or failed to read) Sony's sign-in
        onClosed: if (!psnWeb.holdingForWide()) psnWeb.close()
        background: Rectangle { radius: Tema.radiusPanel; color: Tema.surface; border.color: Tema.lineStrong }
        Connections {
            target: psnWeb
            function onRedirected(url) {
                // Only its own sign-in: the console dialog's account lookup uses the same page
                if (!psnLink.opened || psnLink.sent || psnLink.wideLink > 0) return;
                psnLink.sent = true;
                if (!psnWeb.holdingForWide()) psnWeb.close();
                platform.linkPsn(url);
                psnLink.close();
                home.showToast(qsTr("PSN hesabı"), qsTr("Bağlanıyor…"));
            }
            // Oyun bilgisi: the page has already closed itself and deleted what it kept
            function onWideFinished(sentOk, error) {
                if (!psnLink.opened || psnLink.sent || psnLink.wideLink <= 0) return;
                psnLink.sent = true;
                psnLink.close();
                if (sentOk)
                    home.showToast(qsTr("Oyun bilgisi"), qsTr("Açılıyor…"));
                else
                    home.showInfoDialog(qsTr("Oyun bilgisi açılamadı"), error);
            }
        }
        contentItem: ColumnLayout {
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Label { textFormat: Text.PlainText; text: psnLink.wideLink > 0 ? qsTr("Oyun bilgisi") : platform.psnLinked ? qsTr("PSN hesabı ekle") : qsTr("PSN hesabını bağla"); font.family: Tema.display; font.pixelSize: 22; color: Tema.text; Layout.fillWidth: true }
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
                    : psnLink.wideLink > 0 ? qsTr("%1 hesabıyla Sony'ye giriş yap; giriş bitince oyun bilgisi kendiliğinden açılır. Başka bir hesapla girersen açılmaz.").arg(psnLink.wideName)
                    : platform.psnLinked ? qsTr("Eklemek istediğin PSN hesabıyla giriş yap; bağlı hesapların yerinde kalır. Zaten bağlı bir hesapla girersen yalnız girişi yenilenir.")
                    : qsTr("Sony'nin giriş sayfası; giriş bitince hesabın kendiliğinden bağlanır.")
            }
            // Oyun bilgisi in the same sign-in (not for the Oyun bilgisi sign-in of a linked account)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                visible: psnLink.wideLink <= 0
                Basic.CheckBox {
                    id: alsoWideBox
                    Layout.fillWidth: true
                    checked: psnLink.alsoWide
                    onToggled: psnLink.alsoWide = checked
                    padding: 0
                    spacing: 10
                    indicator: Rectangle {
                        x: alsoWideBox.leftPadding
                        y: alsoWideBox.topPadding + (alsoWideBox.availableHeight - height) / 2
                        implicitWidth: 18
                        implicitHeight: 18
                        radius: 4
                        color: alsoWideBox.checked ? Tema.accent : (alsoWideBox.hovered ? Tema.line : Tema.surface2)
                        border.width: alsoWideBox.checked ? 0 : 1
                        border.color: alsoWideBox.visualFocus ? Tema.accent : Tema.lineStrong
                        Text {
                            anchors.centerIn: parent
                            visible: alsoWideBox.checked
                            text: "✓"
                            color: Tema.inkOnAccent
                            font.pixelSize: 13
                            font.bold: true
                        }
                    }
                    contentItem: Label {
                        textFormat: Text.PlainText
                        leftPadding: alsoWideBox.indicator.width + alsoWideBox.spacing
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Oyun bilgisini de aç")
                        font.family: Tema.body
                        font.pixelSize: Tema.bodySize
                        color: Tema.text
                        wrapMode: Text.WordWrap
                    }
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    leftPadding: 28
                    wrapMode: Text.WordWrap
                    font.family: Tema.body
                    font.pixelSize: Tema.smallSize
                    color: Tema.muted
                    text: qsTr("Arkadaşların PS5'te ne oynadığını görür. Giriş bilgin sunucuda güçlü şifrelemeyle saklanır; istediğin zaman Ayarlar > Hesap'tan kapatabilirsin.")
                }
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

    VRise { target: topBar; d: 0; page: home }
    VRise { target: signInColumn; d: 2; page: home }
    VRise { target: consoleScroll; d: 2; page: home }
    VRise { target: friendScroll; d: 4; page: home }
}
