// Oviora's settings: the old Oviora's settings panel (SettingsDialog.qml), ported as it was —
// sections on the left, the section's title on the right, its page below — and rewired to the
// new app (`oviora` for the player and PS5, `platform` for the Oviora server).
//
// Section ids (test runs: --section <id>; old page index in brackets):
//   account [10]  Hesap         (only when signed in)
//   general [0]   Genel
//   video   [1]   Görüntü
//   stream  [2]   Yayın
//   audio   [3]   Ses ve ağ
//   consoles[4]   Konsollar
//   keys    [5]   Tuşlar
//   controllers [6] Kumandalar
//   remote  [7]   Uzaktan       hidden: this PC's own PSN token, replaced by the Oviora server
//   config  [8]   Yapılandırma  hidden: no setting profiles
//   subtitle[9]   Altyazı
// Older ids still accepted: "controller" → controllers, "about" → account. A hidden id is ignored.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Basic as B
import QtQuick.Shapes
import "ayar" as S

Rectangle {
    id: settings
    color: Tema.bg
    focus: true

    signal closeRequested()
    signal psnLinkRequested()
    signal psnWideRequested(int link, string name)

    readonly property bool signedIn: platform.configured && platform.status === "active"

    // One section's page, made when it is first shown and kept after that
    component PageSlot: Loader {
        required property int page
        readonly property bool wanted: bar.currentIndex === page
        active: false
        onWantedChanged: if (wanted) active = true
        Component.onCompleted: if (wanted) active = true
    }

    // The page (an ayar/SettingsPage) of the section shown now
    function activeSettingsFlick() {
        const slot = pages.children[bar.currentIndex];
        return slot && slot.item ? slot.item : null;
    }
    function focusCurrentTabFirstItem() {
        const page = activeSettingsFlick();
        if (!page)
            return;
        // A page made just now is shown a moment later; until then it has no focus chain
        if (!page.visible) {
            focusRetry.restart();
            return;
        }
        page.contentY = 0;
        const item = page.firstFocusable();
        if (item)
            item.forceActiveFocus(Qt.TabFocusReason);
        else
            page.forceActiveFocus(Qt.TabFocusReason);
    }
    Timer {
        id: focusRetry
        interval: 16
        onTriggered: settings.focusCurrentTabFirstItem()
    }
    function ensureActiveFocusVisible() {
        const page = activeSettingsFlick();
        const window = settings.Window.window;
        if (!page || !window || !window.activeFocusItem)
            return;
        let p = window.activeFocusItem;
        while (p && p !== page)
            p = p.parent;
        if (p === page)
            page.ensureVisible(window.activeFocusItem);
    }
    function openReport() {
        showSection("account");
        report.open();
    }
    // Picks a section by its id (see the list above)
    function showSection(id) {
        const alias = { controller: "controllers", about: "account" };
        const wanted = alias[id] || id;
        // only the sections the menu shows; a hidden one stays on the current section
        for (let i = 0; i < bar.sections.length; i++)
            if (bar.sections[i][3] === wanted) {
                bar.picked = true;
                bar.currentIndex = bar.sections[i][0];
                return true;
            }
        return false;
    }

    // Opens on the account once signed in, unless a section was picked already
    onSignedInChanged: {
        if (!bar.picked)
            bar.currentIndex = signedIn ? 10 : 0;
        else if (!signedIn && bar.currentIndex === 10)
            bar.currentIndex = 0;
    }
    // Test runs: --section <id> opens that section (main.cpp test switches)
    Component.onCompleted: Qt.callLater(() => {
        const t = typeof vyTest !== "undefined" ? vyTest.section : undefined
        if (t !== undefined && t !== "")
            showSection(String(t))
    })

    Keys.onEscapePressed: settings.closeRequested()
    Keys.onPressed: (event) => {
        if (event.modifiers)
            return;
        switch (event.key) {
        case Qt.Key_PageUp:
            bar.decrementCurrentIndex();
            event.accepted = true;
            break;
        case Qt.Key_PageDown:
            bar.incrementCurrentIndex();
            event.accepted = true;
            break;
        case Qt.Key_Up:
        case Qt.Key_Down:
        {
            // Nothing focused on the page yet: the arrows start at its first control
            const page = activeSettingsFlick();
            if (!page)
                return;
            const item = page.firstFocusable();
            if (item) {
                item.forceActiveFocus(Qt.TabFocusReason);
                event.accepted = true;
            }
            break;
        }
        }
    }

    // ── Left: the sections ──
    Item {
        id: bar
        // Page index in the StackLayout below; the menu shows them in its own order
        property int currentIndex: settings.signedIn ? 10 : 0
        property bool picked: false
        // Menu order: page index, title, one-line description, id, shown
        readonly property var allSections: [
            // --test-psn-links: the account page with made-up PSN accounts even when signed out
            [10, qsTr("Hesap"), qsTr("Oviora hesabın, PSN bağlantın ve sürüm"), "account", settings.signedIn || platform.testPsnLinks === true],
            [0, qsTr("Genel"), qsTr("Bağlantı kesilince ve uyku modunda ne olacağı, yayın menüsü kısayolları"), "general", true],
            [1, qsTr("Görüntü"), qsTr("Kod çözücü, pencere düzeni ve dikey eşitleme"), "video", true],
            [2, qsTr("Yayın"), qsTr("Çözünürlük, kare hızı, bit hızı ve kodlayıcı; ev ağı ve internet için ayrı"), "stream", true],
            [3, qsTr("Ses ve ağ"), qsTr("Ses aygıtları, mikrofon ve Wi-Fi ayarları"), "audio", true],
            [4, qsTr("Konsollar"), qsTr("Bu bilgisayara kayıtlı ve gizlenmiş PS5'ler"), "consoles", true],
            [5, qsTr("Tuşlar"), qsTr("Klavyeyle oynarken tuş eşleşmeleri"), "keys", true],
            [6, qsTr("Kumandalar"), qsTr("Kumanda eşleşmeleri ve titreşim"), "controllers", true],
            // Hidden: replaced by the Oviora server (old page: settings/RemotePage.qml)
            [7, qsTr("Uzaktan"), qsTr("Bu bilgisayarın kendi PSN bağlantısı ve bağlantı denemeleri"), "remote", false],
            // Hidden: no setting profiles yet (old page: settings/ConfigPage.qml)
            [8, qsTr("Yapılandırma"), qsTr("Ayar profilleri, dışa ve içe aktarma"), "config", false],
            // Altyazı: the in-game subtitle translation (old page: settings/SubtitlePage.qml)
            [9, qsTr("Altyazı"), qsTr("Türkçe altyazı, çeviri ve yerel yapay zeka"), "subtitle", true]
        ]
        readonly property var sections: allSections.filter(s => s[4])
        readonly property int position: {
            for (let i = 0; i < sections.length; i++)
                if (sections[i][0] === currentIndex)
                    return i;
            return 0;
        }
        function incrementCurrentIndex() {
            picked = true;
            currentIndex = sections[Math.min(sections.length - 1, position + 1)][0];
        }
        function decrementCurrentIndex() {
            picked = true;
            currentIndex = sections[Math.max(0, position - 1)][0];
        }

        anchors {
            top: parent.top
            left: parent.left
            bottom: parent.bottom
        }
        width: 280

        // the sections sit on the panel itself; a hairline parts them from the page
        Rectangle {
            anchors {
                top: parent.top
                bottom: parent.bottom
                right: parent.right
            }
            width: 1
            color: Tema.line
        }

        Label {
            textFormat: Text.PlainText
            id: navTitle
            anchors {
                top: parent.top
                left: parent.left
                topMargin: 30
                leftMargin: 30
            }
            text: qsTr("Ayarlar")
            font.family: Tema.display
            font.pixelSize: 20
            font.weight: Font.DemiBold
            font.letterSpacing: -0.3
            color: Tema.text
        }

        Column {
            anchors {
                top: navTitle.bottom
                left: parent.left
                right: parent.right
                topMargin: 22
                leftMargin: 14
                rightMargin: 14
            }
            spacing: 2
            Repeater {
                model: bar.sections
                delegate: AbstractButton {
                    id: navItem
                    required property var modelData
                    readonly property bool current: bar.currentIndex === modelData[0]
                    width: parent.width
                    height: 42
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: true
                    onClicked: {
                        bar.picked = true;
                        bar.currentIndex = modelData[0];
                    }
                    background: Rectangle {
                        radius: 10
                        color: navItem.current ? Tema.accentSoft : (navItem.hovered ? Tema.surface2 : "transparent")
                    }
                    contentItem: Label {
                        textFormat: Text.PlainText
                        leftPadding: 16
                        verticalAlignment: Text.AlignVCenter
                        text: navItem.modelData[1]
                        font.family: Tema.body
                        font.pixelSize: 16
                        font.weight: navItem.current ? Font.DemiBold : Font.Normal
                        color: navItem.current ? Tema.text : Tema.muted
                    }
                }
            }
        }

        Label {
            textFormat: Text.PlainText
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
                margins: 30
            }
            wrapMode: Text.WordWrap
            text: qsTr("Bölümler arasında PageUp ve PageDown, ayarlar arasında ok tuşlarıyla gezinebilirsin.")
            font.family: Tema.body
            font.pixelSize: 13
            lineHeight: 1.25
            color: Tema.faint
        }
    }

    // ── Right: the section's title, what it is about, and a way out ──
    Item {
        id: sectionHeader
        anchors {
            top: parent.top
            left: bar.right
            right: parent.right
            leftMargin: 40
            rightMargin: 24
        }
        height: 108
        Label {
            textFormat: Text.PlainText
            id: sectionTitle
            anchors {
                top: parent.top
                left: parent.left
                topMargin: 28
            }
            text: bar.sections.length ? bar.sections[bar.position][1] : ""
            font.family: Tema.display
            font.pixelSize: 34
            font.weight: Font.ExtraBold
            font.letterSpacing: -1
            color: Tema.text
        }
        Label {
            textFormat: Text.PlainText
            anchors {
                top: sectionTitle.bottom
                left: parent.left
                right: closeButton.left
                topMargin: 4
            }
            elide: Text.ElideRight
            text: bar.sections.length ? bar.sections[bar.position][2] : ""
            font.family: Tema.body
            font.pixelSize: 16
            color: Tema.muted
        }
        AbstractButton {
            id: closeButton
            anchors {
                top: parent.top
                right: parent.right
                topMargin: 22
            }
            width: 44
            height: 44
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            onClicked: settings.closeRequested()
            background: Rectangle {
                radius: 22
                color: closeButton.hovered ? Tema.surface2 : "transparent"
            }
            // a drawn cross in the one icon style (1.8 px, round ends)
            contentItem: Shape {
                ShapePath {
                    strokeColor: closeButton.hovered ? Tema.text : Tema.muted
                    strokeWidth: 1.8
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    PathMove { x: 16; y: 16 }
                    PathLine { x: 28; y: 28 }
                    PathMove { x: 28; y: 16 }
                    PathLine { x: 16; y: 28 }
                }
            }
        }
        Rectangle {
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
            }
            height: 1
            color: Tema.line
        }
    }

    StackLayout {
        id: pages
        anchors {
            top: sectionHeader.bottom
            left: bar.right
            right: parent.right
            bottom: parent.bottom
            leftMargin: 24
            rightMargin: 12
        }
        currentIndex: bar.currentIndex
        onCurrentIndexChanged: {
            // the page may only now be made: focus once it is there
            Qt.callLater(settings.focusCurrentTabFirstItem)
            Qt.callLater(settings.ensureActiveFocusVisible)
        }

        // A page is made the first time it is opened, so the panel opens without a pause
        PageSlot { page: 0; sourceComponent: Component { S.GeneralPage {} } }
        PageSlot { page: 1; sourceComponent: Component { S.VideoPage {} } }
        PageSlot { page: 2; sourceComponent: Component { S.StreamPage {} } }
        PageSlot { page: 3; sourceComponent: Component { S.AudioNetworkPage {} } }
        PageSlot {
            page: 4
            sourceComponent: Component {
                S.ConsolesPage {
                    onConfirmRequested: (title, text, action) => confirm.ask(title, text, action)
                }
            }
        }
        // Tuşlar: keyboard play and its keys (old: settings/KeysPage.qml)
        PageSlot {
            page: 5
            sourceComponent: Component {
                S.KeysPage {
                    onConfirmRequested: (title, text, action) => confirm.ask(title, text, action)
                }
            }
        }
        // Kumandalar: the old mapping dialog simplified (ayar/PadMappingDialog.qml)
        PageSlot { page: 6; sourceComponent: Component { S.ControllersPage {} } }
        // Uzaktan, Yapılandırma: hidden sections (old: RemotePage, ConfigPage)
        PageSlot { page: 7 }
        PageSlot { page: 8 }
        // Altyazı: the in-game subtitle translation (old: SubtitlePage)
        PageSlot {
            page: 9
            sourceComponent: Component {
                S.SubtitlePage {
                    onConfirmRequested: (title, text, action) => confirm.ask(title, text, action)
                }
            }
        }
        PageSlot {
            page: 10
            sourceComponent: Component {
                S.AccountPage {
                    onSignOutRequested: confirm.ask(qsTr("Hesaptan çık"), qsTr("Bu bilgisayardaki giriş kapansın mı?"), () => platform.logout())
                    onPsnLinkRequested: settings.psnLinkRequested()
                    onPsnWideRequested: (link, name) => settings.psnWideRequested(link, name)
                    onReportRequested: report.open()
                    onDeleteAccountRequested: deleteAccountPopup.ask()
                    onConfirmRequested: (title, text, action) => confirm.ask(title, text, action)
                }
            }
        }
    }

    Connections {
        target: settings.Window.window
        function onActiveFocusItemChanged() {
            settings.ensureActiveFocusVisible()
        }
    }

    // ── Confirm and problem report (the old app's confirm and report dialogs) ──
    Popup {
        id: confirm
        parent: Overlay.overlay
        x: Math.round((settings.Window.width - width) / 2)
        y: Math.round((settings.Window.height - height) / 2)
        width: Math.min(460, settings.width - 64)
        modal: true
        dim: true
        padding: 24
        property var action: null
        property string title: ""
        property string text: ""
        function ask(t, x, a) { title = t; text = x; action = a; open() }
        background: Rectangle { radius: Tema.radiusPanel; color: Tema.surface; border.color: Tema.lineStrong }
        contentItem: ColumnLayout {
            spacing: 14
            Label { textFormat: Text.PlainText; text: confirm.title; font.family: Tema.display; font.pixelSize: 22; color: Tema.text; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            Label { textFormat: Text.PlainText; text: confirm.text; font.family: Tema.body; font.pixelSize: Tema.bodySize; color: Tema.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                S.PillButton { text: qsTr("Vazgeç"); onClicked: confirm.close() }
                S.PillButton { kind: "primary"; text: qsTr("Evet"); onClicked: { const a = confirm.action; confirm.close(); if (a) a(); } }
            }
        }
    }
    Popup {
        id: deleteAccountPopup
        parent: Overlay.overlay
        x: Math.round((settings.Window.width - width) / 2)
        y: Math.round((settings.Window.height - height) / 2)
        width: Math.min(460, settings.width - 64)
        modal: true
        dim: true
        closePolicy: sending ? Popup.NoAutoClose : (Popup.CloseOnEscape | Popup.CloseOnPressOutside)
        padding: 24
        property bool sending: false
        property string errorText: ""
        function ask() {
            sending = false
            errorText = ""
            deletePassField.text = ""
            open()
            deletePassField.forceActiveFocus()
        }
        function submit() {
            if (sending || !deletePassField.text.length)
                return
            sending = true
            errorText = ""
            platform.deleteAccount(deletePassField.text)
        }
        onClosed: {
            sending = false
            errorText = ""
            deletePassField.text = ""
        }
        background: Rectangle { radius: Tema.radiusPanel; color: Tema.surface; border.color: Tema.lineStrong }
        Connections {
            target: platform
            function onAccountDeleted() {
                deleteAccountPopup.sending = false
                deleteAccountPopup.close()
            }
            function onAccountDeleteFailed(text) {
                deleteAccountPopup.sending = false
                deleteAccountPopup.errorText = text
            }
        }
        contentItem: ColumnLayout {
            spacing: 14
            Label {
                textFormat: Text.PlainText
                text: qsTr("Hesabı sil")
                font.family: Tema.display
                font.pixelSize: 22
                color: Tema.text
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                textFormat: Text.PlainText
                text: qsTr("Onaylamak için Oviora hesap şifreni gir. Bu işlem geri alınamaz.")
                font.family: Tema.body
                font.pixelSize: Tema.bodySize
                color: Tema.muted
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            S.InputField {
                id: deletePassField
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: qsTr("Şifre")
                enabled: !deleteAccountPopup.sending
                invalid: deleteAccountPopup.errorText.length > 0
                onAccepted: deleteAccountPopup.submit()
            }
            Label {
                textFormat: Text.PlainText
                visible: deleteAccountPopup.errorText.length > 0
                text: deleteAccountPopup.errorText
                font.family: Tema.body
                font.pixelSize: Tema.smallSize
                color: Tema.danger
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                S.PillButton {
                    text: qsTr("Vazgeç")
                    enabled: !deleteAccountPopup.sending
                    onClicked: deleteAccountPopup.close()
                }
                S.PillButton {
                    kind: "danger"
                    text: deleteAccountPopup.sending ? qsTr("Siliniyor…") : qsTr("Hesabımı kalıcı olarak sil")
                    enabled: !deleteAccountPopup.sending && deletePassField.text.length > 0
                    onClicked: deleteAccountPopup.submit()
                }
            }
        }
    }
    Popup {
        id: report
        parent: Overlay.overlay
        x: Math.round((settings.Window.width - width) / 2)
        y: Math.round((settings.Window.height - height) / 2)
        width: Math.min(520, settings.width - 64)
        modal: true
        dim: true
        padding: 24
        background: Rectangle { radius: Tema.radiusPanel; color: Tema.surface; border.color: Tema.lineStrong }
        Connections {
            target: platform
            function onReportDone(ok, text) { report.close(); confirm.ask(ok ? qsTr("Gönderildi") : qsTr("Gönderilemedi"), text, null) }
        }
        contentItem: ColumnLayout {
            spacing: 12
            Label { textFormat: Text.PlainText; text: qsTr("Sorun bildir"); font.family: Tema.display; font.pixelSize: 22; color: Tema.text }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Ne oldu? Son oturum kayıtları da eklenir; şifre ve giriş bilgileri silinir.")
                font.family: Tema.body
                font.pixelSize: Tema.smallSize
                color: Tema.muted
            }
            B.TextArea {
                id: note
                Layout.fillWidth: true
                Layout.preferredHeight: 140
                wrapMode: TextEdit.Wrap
                color: Tema.text
                font.family: Tema.body
                background: Rectangle { radius: Tema.radiusInput; color: Tema.surface2; border.color: Tema.line }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                S.PillButton { text: qsTr("Vazgeç"); onClicked: report.close() }
                S.PillButton {
                    kind: "primary"
                    enabled: !platform.reportSending
                    text: platform.reportSending ? qsTr("Gönderiliyor…") : qsTr("Gönder")
                    onClicked: platform.sendReport(note.text, "diger")
                }
            }
        }
    }
}
