import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// Oviora account: who is signed in, the linked PSN accounts that share the PS5s, about (version, Arfep)
SettingsPage {
    id: page
    signal signOutRequested()
    signal psnLinkRequested()
    // Oyun bilgisi: Sony's sign-in for that linked PSN account (link: its psnLinks id, name: as shown)
    signal psnWideRequested(int link, string name)
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

    // Linked PSN accounts (up to psnLinksMax); each has its own PS5s, removed with it
    readonly property var psnLinks: platform.psnLinks || []
    readonly property int psnLinksMax: platform.psnLinksMax || 1
    readonly property bool streamerMode: oviora.preferences.streamerMode === true
    function psnLinkName(link) {
        return page.streamerMode || !link.name ? "PSN " + (link.n || 1) : link.name
    }

    SettingsCard {
        title: qsTr("PSN hesapları")
        description: page.psnLinks.length > 0
            ? qsTr("Bağlı. PS5'lerin ana ekranda; arkadaşlarına açtıkların internet üzerinden onlara da görünür. Giriş bilgin sunucuda şifreli durur.")
            : qsTr("Bağlı değil. Kendi PS5'ini eklemek ve arkadaşlarınla paylaşmak için PSN hesabınla bir kez giriş yap.")
        Repeater {
            model: page.psnLinks
            delegate: SettingRow {
                required property var modelData
                readonly property string linkName: page.psnLinkName(modelData)
                readonly property int count: modelData.consoles || 0
                title: modelData.primary && page.psnLinks.length > 1 ? qsTr("%1 (ana hesap)").arg(linkName) : linkName
                help: count > 0 ? qsTr("%1 PS5").arg(count) : qsTr("Bu hesapta PS5 yok")
                // Oyun bilgisi (opt-in per account): "ok", "expired" or ""; "none" = a server without it
                readonly property string wide: modelData.wide === undefined || modelData.wide === null ? "none" : String(modelData.wide)
                RowLayout {
                    spacing: 10
                    Label {
                        visible: wide === "ok" || wide === "expired"
                        textFormat: Text.PlainText
                        text: wide === "ok" ? qsTr("Oyun bilgisi açık") : qsTr("Oyun bilgisi için yeniden giriş gerekli")
                        color: wide === "ok" ? Tema.ok : Tema.warm
                        font.family: Tema.body
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignRight
                        Layout.maximumWidth: 280
                    }
                    PillButton {
                        visible: wide !== "none"
                        compact: true
                        kind: wide === "ok" ? "quiet" : "secondary"
                        text: wide === "ok" ? qsTr("Kapat") : wide === "expired" ? qsTr("Yeniden giriş") : qsTr("Oyun bilgisini aç")
                        enabled: !platform.busy
                        onClicked: {
                            if (wide === "ok")
                                platform.wideUnlinkPsn(modelData.id);
                            else if (wide === "expired")
                                page.psnWideRequested(modelData.id, linkName);
                            else
                                page.confirmRequested(qsTr("Oyun bilgisi"),
                                    qsTr("Arkadaşların PS5'te ne oynadığını görsün diye Oviora, Sony'ye bu PSN hesabıyla giriş yapar ve çevrimiçi durumunu okur. Giriş bilgin sunucuda güçlü şifrelemeyle saklanır ve yalnız bunun için kullanılır. İstediğin zaman kapatabilirsin."),
                                    () => page.psnWideRequested(modelData.id, linkName));
                        }
                    }
                    PillButton {
                        kind: "danger"
                        compact: wide !== "none"
                        text: qsTr("Kaldır")
                        onClicked: page.confirmRequested(qsTr("PSN hesabını kaldır"),
                            qsTr("%1 kaldırılsın mı? Bu hesabın PS5'leri ve arkadaşlarınla paylaşımları da kaldırılır; o PS5'lerde oynayan varsa çıkar.").arg(linkName),
                            () => platform.unlinkPsn(modelData.id))
                    }
                }
            }
        }
        SettingRow {
            visible: page.psnLinks.length === 0
            title: qsTr("PSN hesabını bağla")
            help: qsTr("Bir kez giriş yapman yeter.")
            PillButton {
                kind: "primary"
                text: qsTr("Bağla")
                onClicked: page.psnLinkRequested()
            }
        }
        SettingRow {
            visible: page.psnLinks.length > 0
            title: qsTr("Başka bir PSN hesabı")
            help: page.psnLinks.length >= page.psnLinksMax
                ? qsTr("En fazla %1 PSN hesabı").arg(page.psnLinksMax)
                : qsTr("Başka bir PSN hesabıyla giriş yap; o hesabın PS5'leri de buraya gelir.")
            PillButton {
                text: qsTr("PSN hesabı ekle")
                enabled: page.psnLinks.length < page.psnLinksMax && !platform.busy
                onClicked: page.psnLinkRequested()
            }
        }
        SettingRow {
            visible: page.psnLinks.length > 0
            title: qsTr("PS5 listesi")
            help: qsTr("Yeni bir PS5 aldıysan ya da adını değiştirdiysen listeyi yenile.")
            PillButton {
                text: qsTr("Yenile")
                enabled: !platform.busy
                onClicked: platform.refreshConsoles()
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
