import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// Old Oviora's "Genel" page. Wired: the interface language (oviora.preferences.language), what
// happens to the PS5 when the window closes during a stream or the PC goes to sleep
// (preferences.disconnectAction / suspendAction) and the controller combination of the stream
// menu. Kept but hidden until the new app has them: streamer mode, audio/video off for debugging.
SettingsPage {
    id: page

    // The buttons a shortcut can use, in the order the settings store them (0 = not used)
    readonly property var comboButtons: [qsTr("Kullanılmıyor"), qsTr("Çarpı"), qsTr("Daire"), qsTr("Kare"), qsTr("Üçgen"),
        qsTr("Yön sol"), qsTr("Yön sağ"), qsTr("Yön yukarı"), qsTr("Yön aşağı"), "L1", "R1", "L3", "R3",
        "Options", "Create", qsTr("Dokunmatik yüzey"), qsTr("PS düğmesi")]

    SettingsCard {
        title: qsTr("Dil")
        description: qsTr("Oviora'nın dili. Otomatik: Windows Türkçe ise Türkçe, Almanca ise Almanca, değilse İngilizce.")

        SettingRow {
            title: qsTr("Arayüz dili")
            Segmented {
                readonly property var ids: ["auto", "tr", "en", "de"]
                model: [qsTr("Otomatik"), "Türkçe", "English", "Deutsch"]
                currentIndex: Math.max(0, ids.indexOf(oviora.preferences.language || "auto"))
                onActivated: (index) => oviora.setPreference("language", ids[index])
            }
        }
    }

    // Old disconnectAction / suspendAction: "nothing" / "sleep" / "ask" (closing the window during a
    // PS5 stream, Main.qml) and "nothing" / "sleep" (the PC going to sleep, bridge/StreamSession.cpp)
    SettingsCard {
        title: qsTr("Oyunu bırakınca")
        description: qsTr("Yayından çıktığında ya da bilgisayarın uykuya geçtiğinde PS5'e ne olacağı.")

        SettingRow {
            title: qsTr("Yayından çıkınca")
            help: qsTr("Bana sor dersen her çıkışta konsolu uyutup uyutmayacağın sorulur.")
            Segmented {
                readonly property var ids: ["nothing", "sleep", "ask"]
                model: [qsTr("Bir şey yapma"), qsTr("Konsolu uyut"), qsTr("Bana sor")]
                currentIndex: Math.max(0, ids.indexOf(oviora.preferences.disconnectAction || "ask"))
                onActivated: (index) => oviora.setPreference("disconnectAction", ids[index])
            }
        }
        SettingRow {
            title: qsTr("Bilgisayar uykuya geçince")
            help: qsTr("Bilgisayarın uyku moduna girerken konsolu da uyutabilir.")
            Segmented {
                readonly property var ids: ["nothing", "sleep"]
                model: [qsTr("Bir şey yapma"), qsTr("Konsolu uyut")]
                currentIndex: Math.max(0, ids.indexOf(oviora.preferences.suspendAction || "nothing"))
                onActivated: (index) => oviora.setPreference("suspendAction", ids[index])
            }
        }
    }

    // The stream menu's controller combination (old: streamMenuEnabled, streamMenuShortcut1..4);
    // oviora.preferences.padMenuCombo / padMenuButtons, read by the bridge (bridge/GameInput.cpp)
    readonly property var menuButtons: oviora.preferences.padMenuButtons !== undefined ? oviora.preferences.padMenuButtons : [9, 10, 11, 12]
    SettingsCard {
        title: qsTr("Yayın menüsü")
        description: qsTr("Oyun sırasında ses, görüntü ve altyazı gibi ayarlara ulaştığın menü.")

        SettingRow {
            title: qsTr("Kumandayla açılsın")
            help: qsTr("Aşağıdaki tuşlara birlikte basınca menü açılır.")
            Toggle {
                checked: oviora.preferences.padMenuCombo !== false
                onToggled: oviora.setPreference("padMenuCombo", checked)
            }
        }
        SettingRow {
            visible: oviora.preferences.padMenuCombo !== false
            stacked: true
            title: qsTr("Tuş kombinasyonu")
            help: qsTr("En fazla dört tuş. Varsayılan: L1 + R1 + L3 + R3.")
            RowLayout {
                width: parent.width
                spacing: 10
                Repeater {
                    model: 4
                    delegate: Dropdown {
                        required property int index
                        Layout.fillWidth: true
                        Layout.preferredWidth: 100
                        model: page.comboButtons
                        currentIndex: page.menuButtons[index] || 0
                        onActivated: (i) => {
                            const list = [0, 1, 2, 3].map(n => page.menuButtons[n] || 0);
                            list[index] = i;
                            oviora.setPreference("padMenuButtons", list);
                        }
                    }
                }
            }
        }
    }

    // Streamer mode (preferences.streamerMode), Discord's "playing" line; audio/video switch-off stays hidden
    SettingsCard {
        title: qsTr("Gizlilik")
        description: qsTr("Ekranını başkalarına gösterirken işine yarar.")

        SettingRow {
            title: qsTr("Yayıncı modu")
            help: qsTr("Yayın açarken hesap adını, IP adreslerini ve arkadaş kodlarını gizler.")
            Toggle {
                checked: oviora.preferences.streamerMode === true
                onToggled: oviora.setPreference("streamerMode", checked)
            }
        }
        // preferences.discordPresence, on unless switched off (platform/DiscordPresence.cpp via main.cpp)
        SettingRow {
            title: qsTr("Discord'da oynadığını göster")
            help: qsTr("Yayın sürerken Discord profilinde oyunun adı ve kimin PS5'inde oynadığın görünür.")
            Toggle {
                checked: oviora.preferences.discordPresence !== false
                onToggled: oviora.setPreference("discordPresence", checked)
            }
        }

        AdvancedSection {
            visible: false
            SettingRow {
                title: qsTr("Ses ve görüntü")
                help: qsTr("Sorun ararken sesi ya da görüntüyü tamamen kapatmak için. Normalde ikisi de açık kalmalı.")
                Dropdown {
                    implicitWidth: 260
                    model: [qsTr("İkisi de açık"), qsTr("Ses kapalı"), qsTr("Görüntü kapalı"), qsTr("İkisi de kapalı")]
                    currentIndex: 0
                }
            }
        }
    }
}
