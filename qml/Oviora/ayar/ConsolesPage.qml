import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// Old Oviora's "Konsollar" page. Wired: registering a PS5 (oviora.openPs5Dialog), the registered
// PS5s (oviora.lanConsoles, registered ones), their auto connect (preferences.autoConnect, one
// console; bridge/StreamSession.cpp connects once it is found after the start) and deleting them
// (oviora.lanForget), the search of this network (oviora.lanSearch, new in this app) and the
// consoles hidden from the home (oviora.hiddenConsoles / lanUnhide).
SettingsPage {
    id: page
    signal confirmRequested(string title, string text, var action)

    readonly property var registered: oviora.lanConsoles.filter(c => c.registered)

    SettingsCard {
        title: qsTr("Konsol ekle")
        description: qsTr("Ev ağındaki bir PS5'i bu bilgisayara bir kez kaydedersin; sonra ana ekranda görünür.")
        SettingRow {
            title: qsTr("Yeni konsol kaydet")
            help: qsTr("Konsolun açık ve aynı ağda olmalı. PS5'te Ayarlar > Sistem > Uzaktan Oynatma > Cihazı Bağla'dan kodu alırsın.")
            PillButton {
                kind: "primary"
                text: qsTr("Konsol kaydet")
                onClicked: oviora.openPs5Dialog()
            }
        }
    }

    SettingsCard {
        title: qsTr("Kayıtlı konsollar")
        description: qsTr("Otomatik bağlan açıksa Oviora açılınca o konsola kendiliğinden bağlanır.")

        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            Layout.topMargin: 12
            Layout.bottomMargin: 6
            visible: page.registered.length === 0
            text: qsTr("Henüz kayıtlı konsol yok.")
            font.family: Tema.body
            font.pixelSize: 15
            color: Tema.muted
        }

        Repeater {
            model: page.registered
            delegate: SettingRow {
                id: hostRow
                required property var modelData
                stacked: false
                title: modelData.name
                help: "PS5" + (modelData.address ? "  ·  " + (oviora.preferences.streamerMode === true ? qsTr("adres gizli") : modelData.address) : "")
                RowLayout {
                    spacing: 14
                    Label {
                        textFormat: Text.PlainText
                        text: qsTr("Otomatik bağlan")
                        font.family: Tema.body
                        font.pixelSize: 14
                        color: Tema.muted
                    }
                    Toggle {
                        checked: hostRow.modelData.autoConnect === true
                        onToggled: oviora.setPreference("autoConnect", checked ? hostRow.modelData.key : "")
                    }
                    // Address, or another PSN account (a new code from the console replaces the pairing)
                    PillButton {
                        compact: true
                        kind: "quiet"
                        text: qsTr("Düzenle")
                        onClicked: oviora.lanEdit(hostRow.modelData.key)
                    }
                    PillButton {
                        compact: true
                        kind: "danger"
                        text: qsTr("Sil")
                        onClicked: page.confirmRequested(qsTr("Konsolu sil"),
                            qsTr("%1 bu bilgisayardan silinsin mi? Tekrar bağlanmak için yeniden kaydetmen gerekir.").arg(hostRow.modelData.name),
                            () => oviora.lanForget(hostRow.modelData.key))
                    }
                }
            }
        }
    }

    // New in this app: the home's "Bu ağda" list searches this network every few seconds
    SettingsCard {
        title: qsTr("Bu ağ")
        description: qsTr("Ana ekrandaki \"Bu ağda\" listesi.")
        SettingRow {
            title: qsTr("Bu ağdaki PS5'leri ara")
            help: qsTr("Açıkken PS5'ler birkaç saniyede bir aranır; oyun sırasında arama durur.")
            Toggle {
                checked: oviora.lanSearch
                onToggled: oviora.lanSearch = checked
            }
        }
    }

    // The consoles hidden from the home's "Bu ağda" list (old: Chiaki.hiddenHosts, unhideHost)
    SettingsCard {
        title: qsTr("Gizlenen konsollar")
        description: qsTr("Ana ekrandan gizlediğin konsollar. Geri getirmek için Göster'e bas.")

        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            Layout.topMargin: 12
            Layout.bottomMargin: 6
            visible: oviora.hiddenConsoles.length === 0
            text: qsTr("Gizlenen konsol yok.")
            font.family: Tema.body
            font.pixelSize: 15
            color: Tema.muted
        }

        Repeater {
            model: oviora.hiddenConsoles
            delegate: SettingRow {
                id: hiddenRow
                required property var modelData
                title: modelData.name || "PS5"
                help: modelData.id
                PillButton {
                    compact: true
                    text: qsTr("Göster")
                    onClicked: page.confirmRequested(qsTr("Konsolu göster"),
                        qsTr("%1 ana ekranda yeniden görünsün mü?").arg(hiddenRow.modelData.name || "PS5"),
                        () => oviora.lanUnhide(hiddenRow.modelData.id))
                }
            }
        }
    }
}
