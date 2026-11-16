import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// Oviora account: who is signed in, the PSN link that shares the PS5s, about (version, Arfep)
SettingsPage {
    id: page
    signal signOutRequested()
    signal psnLinkRequested()
    signal reportRequested()
    signal deleteAccountRequested()
    signal confirmRequested(string title, string text, var action)

    SettingsCard {
        title: platform.user
        description: qsTr("Oviora hesabınla giriş yaptın. Arkadaşların seni bu adla bulur.")
        SettingRow {
            title: qsTr("Hesaptan çık")
            help: qsTr("Bu bilgisayardaki giriş kapanır; ayarların yerinde kalır.")
            PillButton {
                text: qsTr("Çıkış yap")
                enabled: platform.sessionState == "idle"
                onClicked: page.signOutRequested()
            }
        }
    }

    SettingsCard {
        title: qsTr("PSN hesabı")
        description: platform.psnLinked
            ? qsTr("Bağlı. PS5'lerin ana ekranda; arkadaşlarına açtıkların internet üzerinden onlara da görünür. Giriş bilgin sunucuda şifreli durur.")
            : qsTr("Bağlı değil. Kendi PS5'ini eklemek ve arkadaşlarınla paylaşmak için PSN hesabınla bir kez giriş yap.")
        SettingRow {
            visible: !platform.psnLinked
            title: qsTr("PSN hesabını bağla")
            help: qsTr("Bir kez giriş yapman yeter.")
            PillButton {
                kind: "primary"
                text: qsTr("Bağla")
                onClicked: page.psnLinkRequested()
            }
        }
        SettingRow {
            visible: platform.psnLinked
            title: qsTr("PS5 listesi")
            help: qsTr("Yeni bir PS5 aldıysan ya da adını değiştirdiysen listeyi yenile.")
            PillButton {
                text: qsTr("Yenile")
                onClicked: platform.refreshConsoles()
            }
        }
        SettingRow {
            visible: platform.psnLinked
            title: qsTr("PSN hesabını kaldır")
            help: qsTr("PSN girişin sunucudan silinir; PS5'lerine kimse bağlanamaz.")
            PillButton {
                kind: "danger"
                text: qsTr("Kaldır")
                onClicked: page.confirmRequested(qsTr("PSN hesabını kaldır"),
                    qsTr("PSN girişin sunucudan silinir; PS5'lerine kimse bağlanamaz, oynayan varsa çıkar."),
                    () => platform.unlinkPsn())
            }
        }
    }

    SettingsCard {
        title: qsTr("Bildirimler")
        description: qsTr("Oviora açıkken olanları haber verir: önündeyse pencerenin köşesinde, değilse Windows bildirimi olarak.")
        Repeater {
            model: [
                { type: "friend_request", title: qsTr("Arkadaşlık istekleri"), help: qsTr("Biri seni eklediğinde ya da isteğini kabul ettiğinde.") },
                { type: "share_granted", title: qsTr("Yeni paylaşımlar"), help: qsTr("Bir arkadaşın PS5'ini seninle paylaştığında.") },
                { type: "session_started", title: qsTr("PS5'imde oturum"), help: qsTr("Biri senin PS5'inde oynamaya başladığında.") },
                { type: "console_free", title: qsTr("PS5 boşaldı"), help: qsTr("Sırada beklediğin PS5 boşaldığında.") },
                { type: "kicked", title: qsTr("Çıkarılma"), help: qsTr("Bir oturumdan çıkarıldığında.") }
            ]
            delegate: SettingRow {
                required property var modelData
                title: modelData.title
                help: modelData.help
                Toggle {
                    checked: platform.notifyEnabled(modelData.type)
                    onToggled: {
                        platform.setNotifyEnabled(modelData.type, checked);
                        if (modelData.type === "friend_request")
                            platform.setNotifyEnabled("friend_accepted", checked);
                    }
                }
            }
        }
    }

    SettingsCard {
        title: qsTr("Sorun bildir")
        description: qsTr("Bir şey ters gittiyse bize yaz; son oturum kayıtları da eklenir (şifre ve giriş bilgileri silinerek).")
        SettingRow {
            title: qsTr("Sorun bildir")
            help: qsTr("Notunu yaz, türünü seç ve gönder.")
            PillButton {
                text: qsTr("Sorun bildir")
                onClicked: page.reportRequested()
            }
        }
    }

    AboutCard {
        updateText: platform.updateText || qsTr("Güncellemeler kendiliğinden gelir.")
        SettingRow {
            title: qsTr("Yasal")
            stacked: true
            Flow {
                width: parent.width
                spacing: 8
                PillButton {
                    compact: true
                    kind: "quiet"
                    text: qsTr("Gizlilik politikası")
                    onClicked: Qt.openUrlExternally(platform.legalUrl("gizlilik.php"))
                }
                PillButton {
                    compact: true
                    kind: "quiet"
                    text: qsTr("Kullanım koşulları")
                    onClicked: Qt.openUrlExternally(platform.legalUrl("kosullar.php"))
                }
                PillButton {
                    compact: true
                    kind: "quiet"
                    text: qsTr("Künye")
                    onClicked: Qt.openUrlExternally(platform.legalUrl("kunye.php"))
                }
            }
        }
    }

    SettingsCard {
        title: qsTr("Hesabı sil")
        Notice {
            tone: "error"
            text: qsTr("Oviora hesabın, arkadaşların, paylaşımların, PSN bağlantın, mesajların ve sorun bildirimlerin kalıcı olarak silinir. Bu işlem geri alınamaz. Bilgisayarındaki PS5 kayıtları (ev ağı) silinmez.")
            PillButton {
                kind: "danger"
                text: qsTr("Hesabı sil")
                enabled: platform.sessionState == "idle"
                onClicked: page.deleteAccountRequested()
            }
        }
    }
}
