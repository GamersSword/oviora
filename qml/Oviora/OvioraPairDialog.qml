// The old Oviora's "Konsol kaydet" (RegistDialog), on the new app's PS5 pairing (oviora.ps5Set /
// ps5Pair): address, PSN account id and the 8-digit code the PS5 shows; progress below.
// Only PS5 (the new engine streams PS5 only); the console login PIN is asked when connecting.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import "ayar" as S

Rectangle {
    id: dialog
    color: Tema.bg
    visible: false
    // Keeps clicks and wheel off the page below
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => wheel.accepted = true }

    property bool started: false
    property int revisionBefore: 0
    // "Düzenle" on a registered console: same dialog; no code = address only, a code = new pairing
    readonly property bool editing: oviora.ps5.editing === true
    // The account id of the PSN account linked to this Oviora account (server), as the old "PSN ile giriş yap"
    readonly property string linkedAccount: typeof platform !== "undefined" && platform ? (platform.psnAccountId || "") : ""
    readonly property bool psnLinked: typeof platform !== "undefined" && platform && platform.psnLinked === true
    readonly property var savedAccounts: typeof platform !== "undefined" && platform && platform.psnAccounts ? platform.psnAccounts : []
    // Every PSN account linked to the Oviora account (several possible); a PS5 is registered with the one it is on
    readonly property var linkedAccounts: typeof platform !== "undefined" && platform && platform.psnLinks ? platform.psnLinks.filter(l => l.account_id && l.account_id.length > 0) : []
    function isLinkedId(id) {
        return id.length > 0 && (id === linkedAccount || linkedAccounts.some(l => l.account_id === id))
    }
    readonly property bool streamerMode: oviora.preferences.streamerMode === true
    property string sentAccountId: ""
    readonly property bool busy: oviora.ps5.busy === true
    readonly property bool paired: started && !busy && (oviora.ps5.pairRevision || 0) > revisionBefore
    readonly property bool canSend: !busy && hostField.text.trim().length > 0 && (editing && pin.text.length === 0 || accountId.text.trim().length > 0 && pin.acceptableInput)

    function hasSavedAccount(id) {
        for (let i = 0; i < savedAccounts.length; ++i) {
            if (savedAccounts[i].id === id)
                return true
        }
        return false
    }
    function open() {
        oviora.ps5Load()
        hostField.text = oviora.ps5.host || ""
        accountId.text = oviora.ps5.account || (editing ? "" : linkedAccount)
        pin.text = ""
        // Ad: the name shown for this console (\"\" = the one the console announces)
        editKey = editing ? (oviora.ps5.profile || "") : ""
        // Only a name the user gave; the console's own name shows as the hint
        nameField.text = editKey.length ? ((((oviora.preferences || {}).consoleNames) || {})[editKey] || "") : ""
        ownName = editKey.length ? oviora.lanName(editKey) : ""
        sentAccountId = ""
        started = false
        visible = true
        hostField.forceActiveFocus()
    }
    function close() { visible = false; oviora.ps5EndEdit() }
    function startLookup() { lookup.start() }
    function send() {
        if (!canSend) return
        sentAccountId = accountId.text.trim()
        oviora.ps5Set("host", hostField.text.trim())
        oviora.ps5Set("account", sentAccountId)
        revisionBefore = oviora.ps5.pairRevision || 0
        started = true
        if (editing) oviora.lanRename(editKey, nameField.text)
        if (editing && pin.text.length === 0) oviora.ps5SaveEdit()
        else oviora.ps5Pair(pin.text.trim())
    }
    property string editKey: ""
    property string ownName: ""
    onPairedChanged: if (paired) {
        // A new registration: the name typed in goes to the console just saved (now selected)
        if (!editing && nameField.text.trim().length > 0 && (oviora.ps5.profile || "").length > 0) oviora.lanRename(oviora.ps5.profile, nameField.text)
        if (typeof platform !== "undefined" && platform && sentAccountId.length > 0 && !hasSavedAccount(sentAccountId))
            platform.savePsnAccount(sentAccountId, "")
        closeTimer.start()
    }

    // Sony's sign-in page for any PSN account; the redirect gives only that account's id
    Popup {
        id: lookup
        parent: Overlay.overlay
        x: Math.round((dialog.width - width) / 2)
        y: Math.round((dialog.height - height) / 2)
        width: Math.min(560, dialog.width - 48)
        height: Math.min(760, dialog.height - 48)
        modal: true
        dim: true
        padding: 20
        closePolicy: Popup.CloseOnEscape
        property bool sent: false
        property string error: ""
        function start() {
            if (typeof psnWeb === "undefined" || typeof psnLookup === "undefined") return
            sent = false
            error = ""
            open()
            Qt.callLater(show)
        }
        function show() {
            const p = webArea.mapToItem(null, 0, 0)
            psnWeb.open(psnLookup.loginUrl(), p.x, p.y, webArea.width, webArea.height)
        }
        function place() {
            if (!opened || sent) return
            const p = webArea.mapToItem(null, 0, 0)
            psnWeb.setRect(p.x, p.y, webArea.width, webArea.height)
        }
        onClosed: if (!sent) psnWeb.close()
        background: Rectangle { radius: Tema.radiusPanel; color: Tema.surface; border.color: Tema.lineStrong }
        Connections {
            target: typeof psnWeb !== "undefined" ? psnWeb : null
            function onRedirected(url) {
                if (!lookup.opened || lookup.sent) return
                lookup.sent = true
                psnWeb.close()
                psnLookup.finish(url)
            }
        }
        Connections {
            target: typeof psnLookup !== "undefined" ? psnLookup : null
            function onFound(id, onlineId) {
                accountId.text = id
                if (typeof platform !== "undefined" && platform)
                    platform.savePsnAccount(id, onlineId || "")
                lookup.close()
            }
            function onFailed(text) { lookup.error = text; lookup.sent = false; lookup.show() }
        }
        contentItem: ColumnLayout {
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Label { textFormat: Text.PlainText; text: qsTr("PSN ile giriş yap"); font.family: Tema.display; font.pixelSize: 22; color: Tema.text; Layout.fillWidth: true }
                S.PillButton { kind: "quiet"; compact: true; text: qsTr("Kapat"); onClicked: lookup.close() }
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.family: Tema.body
                font.pixelSize: Tema.smallSize
                color: lookup.error.length || psnWeb.state === "error" ? Tema.danger : Tema.muted
                text: !psnWeb.available ? qsTr("Bu bilgisayarda Microsoft Edge WebView2 yok; PSN girişi açılamıyor.")
                    : lookup.error.length ? lookup.error
                    : psnWeb.state === "error" ? psnWeb.errorText
                    : psnLookup.busy ? qsTr("Hesap kimliği alınıyor…")
                    : qsTr("Konsolda kayıt için kullanacağın PSN hesabıyla giriş yap. Yalnız hesap kimliği alınır; Oviora'ya bağlı hesabın değişmez.")
            }
            Item {
                id: webArea
                Layout.fillWidth: true
                Layout.fillHeight: true
                onWidthChanged: lookup.place()
                onHeightChanged: lookup.place()
                Label {
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    visible: psnWeb.state !== "ready"
                    text: psnLookup.busy ? qsTr("Hesap kimliği alınıyor…") : qsTr("Yükleniyor…")
                    font.family: Tema.body
                    color: Tema.faint
                }
            }
        }
    }
    onLinkedAccountChanged: if (visible && accountId.text.trim().length === 0) accountId.text = linkedAccount
    Timer { id: closeTimer; interval: 1800; onTriggered: dialog.close() }
    Keys.onEscapePressed: dialog.close()

    // ── Header: title, what it is, the action and a way out (as the old DialogView) ──
    Item {
        id: header
        anchors { top: parent.top; left: parent.left; right: parent.right; leftMargin: 40; rightMargin: 24 }
        height: 108
        Label {
            textFormat: Text.PlainText
            id: title
            anchors { top: parent.top; left: parent.left; topMargin: 28 }
            text: dialog.editing ? qsTr("Konsolu düzenle") : qsTr("Konsol kaydet")
            font.family: Tema.display
            font.pixelSize: 34
            font.weight: Font.ExtraBold
            font.letterSpacing: -1
            color: Tema.text
        }
        Label {
            textFormat: Text.PlainText
            anchors { top: title.bottom; left: parent.left; topMargin: 4 }
            text: dialog.editing ? qsTr("Yalnız adresi değiştiriyorsan kodu boş bırak; hesabı değiştirmek için yeni kod gir") : qsTr("Bir kez kaydedersin; sonra ana ekranda görünür")
            font.family: Tema.body
            font.pixelSize: 16
            color: Tema.muted
        }
        RowLayout {
            anchors { top: parent.top; right: parent.right; topMargin: 26 }
            spacing: 10
            // Editing: remove this registration; the first press asks, the second deletes
            S.PillButton {
                id: deleteButton
                property bool armed: false
                visible: dialog.editing && dialog.editKey.length > 0
                kind: "danger"
                text: armed ? qsTr("Silmek için tekrar bas") : qsTr("Konsolu sil")
                enabled: !dialog.busy
                onClicked: {
                    if (!armed) { armed = true; disarm.restart(); return }
                    armed = false
                    const key = dialog.editKey
                    dialog.close()
                    oviora.lanForget(key)
                }
                onVisibleChanged: armed = false
                Timer { id: disarm; interval: 3000; onTriggered: deleteButton.armed = false }
            }
            S.PillButton {
                kind: "primary"
                text: qsTr("Kaydet")
                enabled: dialog.canSend
                onClicked: dialog.send()
            }
            AbstractButton {
                id: closeButton
                implicitWidth: 44
                implicitHeight: 44
                hoverEnabled: true
                onClicked: dialog.close()
                background: Rectangle { radius: 22; color: closeButton.hovered ? Tema.surface2 : "transparent" }
                contentItem: Label {
                    textFormat: Text.PlainText
                    text: "✕"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 16
                    color: closeButton.hovered ? Tema.text : Tema.muted
                }
            }
        }
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Tema.line }
    }

    S.SettingsPage {
        anchors { top: header.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; leftMargin: 24; rightMargin: 12 }
        maxContentWidth: 820

        S.SettingsCard {
            title: qsTr("Konsol")
            description: qsTr("Konsol açık ve bu bilgisayarla aynı ağda olmalı.")
            S.SettingRow {
                title: qsTr("Ad")
                help: qsTr("Ana ekranda ve listelerde görünen ad. Boş bırakırsan konsolun kendi adı kullanılır.")
                S.InputField {
                    id: nameField
                    implicitWidth: 320
                    maximumLength: 40
                    placeholderText: dialog.ownName.length && dialog.ownName !== "PS5" ? dialog.ownName : qsTr("Örneğin Salondaki PS5")
                }
            }
            S.SettingRow {
                title: qsTr("Adres")
                help: qsTr("Konsolun IP adresi.")
                S.InputField {
                    id: hostField
                    implicitWidth: 320
                    placeholderText: qsTr("192.168.1.20")
                }
            }
        }

        S.SettingsCard {
            title: qsTr("PSN hesabı")
            description: qsTr("Konsolda oturum açtığın PSN hesabı. Kayıt bu hesaba bağlanır.")
            S.SettingRow {
                stacked: true
                title: qsTr("Hesap kimliği")
                help: dialog.linkedAccount.length > 0 && accountId.text.trim() === dialog.linkedAccount ? qsTr("Oviora'ya bağlı PSN hesabından alındı.") : dialog.linkedAccount.length > 0 ? qsTr("base64 biçiminde bir kimlik. Bağlı PSN hesabını kullanabilir ya da kullanıcı adınla arayabilirsin.") : dialog.psnLinked ? qsTr("base64 biçiminde bir kimlik. Bilmiyorsan kullanıcı adınla ara.") : qsTr("base64 biçiminde bir kimlik. Bilmiyorsan PSN ile giriş yap ya da kullanıcı adınla ara.")
                RowLayout {
                    width: parent.width
                    spacing: 10
                    S.InputField {
                        id: accountId
                        Layout.fillWidth: true
                        placeholderText: qsTr("Hesap kimliği")
                    }
                    // Any account on the console (e.g. a second one for the stream while the
                    // DualSense uses the main one on the PS5): only its id, nothing is linked
                    S.PillButton {
                        kind: dialog.linkedAccount.length > 0 ? "quiet" : "primary"
                        text: dialog.linkedAccount.length > 0 ? qsTr("Başka hesapla giriş yap") : qsTr("PSN ile giriş yap")
                        onClicked: lookup.start()
                    }
                    // Linked, but another id is typed in (e.g. editing): back to the linked account
                    S.PillButton {
                        kind: "quiet"
                        visible: dialog.linkedAccount.length > 0 && !dialog.isLinkedId(accountId.text.trim())
                        text: qsTr("Bağlı hesabımı kullan")
                        onClicked: accountId.text = dialog.linkedAccount
                    }
                    S.PillButton {
                        kind: "quiet"
                        text: qsTr("Kullanıcı adıyla bul")
                        onClicked: oviora.ps5OpenAccountChecker()
                    }
                }
            }
            // Several PSN accounts linked: pick the one the console is signed in with
            S.SettingRow {
                stacked: true
                visible: dialog.linkedAccounts.length > 1
                title: qsTr("Bağlı PSN hesapların")
                Flow {
                    width: parent.width
                    spacing: 8
                    Repeater {
                        model: dialog.linkedAccounts
                        delegate: S.PillButton {
                            required property var modelData
                            compact: true
                            kind: accountId.text.trim() === modelData.account_id ? "primary" : "secondary"
                            text: dialog.streamerMode || !modelData.name ? "PSN " + modelData.n : modelData.name
                            onClicked: accountId.text = modelData.account_id
                        }
                    }
                }
            }
            S.SettingRow {
                stacked: true
                visible: dialog.savedAccounts.length > 0
                title: qsTr("Kayıtlı PSN hesapları")
                Flow {
                    width: parent.width
                    spacing: 8
                    Repeater {
                        model: dialog.savedAccounts
                        delegate: RowLayout {
                            spacing: 4
                            readonly property bool selected: accountId.text.trim() === modelData.id
                            readonly property bool isLinked: dialog.isLinkedId(modelData.id)
                            readonly property string baseName: dialog.streamerMode
                                ? qsTr("PSN hesabı %1").arg(index + 1)
                                : (modelData.name && modelData.name.length > 0 ? modelData.name : isLinked ? qsTr("Bağlı hesap") : qsTr("PSN hesabı %1").arg(index + 1))
                            S.PillButton {
                                compact: true
                                kind: selected ? "primary" : "secondary"
                                text: isLinked && baseName !== qsTr("Bağlı hesap") ? baseName + " (" + qsTr("bağlı") + ")" : baseName
                                onClicked: accountId.text = modelData.id
                            }
                            AbstractButton {
                                id: removeBtn
                                visible: !isLinked
                                implicitWidth: 28
                                implicitHeight: 28
                                hoverEnabled: true
                                onClicked: if (typeof platform !== "undefined" && platform) platform.removePsnAccount(modelData.id)
                                background: Rectangle {
                                    radius: 14
                                    color: removeBtn.hovered ? Tema.surface2 : "transparent"
                                }
                                contentItem: Label {
                                    textFormat: Text.PlainText
                                    text: "✕"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    font.pixelSize: 12
                                    color: removeBtn.hovered ? Tema.danger : Tema.muted
                                }
                            }
                        }
                    }
                }
            }
        }

        S.SettingsCard {
            title: qsTr("Kodlar")
            description: dialog.editing ? qsTr("Hesabı değiştirmek için: konsolda yeni hesapla oturum aç, Ayarlar > Sistem > Uzaktan Oynatma > Cihazı Bağla'yı aç ve çıkan kodu gir. Eski kayıt bunun yerine geçer.") : qsTr("Konsolda Ayarlar > Sistem > Uzaktan Oynatma > Cihazı Bağla'yı aç; ekranda bir kod görünür.")
            S.SettingRow {
                title: qsTr("Uzaktan Oynatma kodu")
                help: qsTr("Konsolun gösterdiği 8 haneli kod. Birkaç dakika geçerlidir.")
                S.InputField {
                    id: pin
                    implicitWidth: 320
                    placeholderText: qsTr("8 hane")
                    font.letterSpacing: text.length ? 3 : 0
                    validator: RegularExpressionValidator { regularExpression: /[0-9]{8}/ }
                    onAccepted: dialog.send()
                }
            }
        }

        // The job's progress (the old LogDialog's place)
        S.SettingsCard {
            visible: dialog.started
            title: dialog.busy ? qsTr("Konsola kaydediliyor…") : dialog.paired ? (dialog.editing ? qsTr("Değişiklik kaydedildi.") : qsTr("Kaydedildi. Konsol ana ekranda.")) : qsTr("Kaydedilemedi. Kodu ve adresi kontrol et.")
            accent: dialog.paired ? Tema.ok : dialog.busy ? Tema.accent : Tema.danger
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: oviora.ps5.status || ""
                font.family: Tema.body
                font.pixelSize: Tema.smallSize
                color: Tema.muted
            }
        }
    }
}
