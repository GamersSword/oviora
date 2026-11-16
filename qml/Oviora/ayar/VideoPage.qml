import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// Old Oviora's "Görüntü" page. Wired: the window at stream start (preferences.streamWindow:
// normal / maximized / fullscreen), double-click fullscreen (preferences.fullscreenDoubleClick)
// and the cursor hidden over the stream (preferences.hideCursor, hideCursorSeconds; Main.qml),
// V-Sync (oviora.displaySync), hardware decoding and fine colour sampling (oviora.ps5 decode /
// sampling). Kept but hidden until the new app has them: the custom window size, the picture
// presets and their custom / display dialogs, zero copy, frame mapping and mixing, the renderer.
SettingsPage {
    id: page

    // Old windowType, fullscreenDoubleClick, hideCursor. The old list's zoom and stretch are the
    // stream menu's picture fit here; a custom window size has no counterpart (the window follows
    // the picture's shape).
    SettingsCard {
        title: qsTr("Pencere")
        description: qsTr("Oyun ekranının bilgisayarında nasıl görüneceği.")

        SettingRow {
            title: qsTr("Görüntü düzeni")
            help: qsTr("Tam ekran çoğu kişi için en iyisi. Yakınlaştırma oranını yayın menüsünden ayarlarsın.")
            Dropdown {
                readonly property var ids: ["normal", "maximized", "fullscreen"]
                implicitWidth: 300
                model: [qsTr("Yayın boyutunda pencere"), qsTr("Büyütülmüş pencere"), qsTr("Tam ekran")]
                currentIndex: Math.max(0, ids.indexOf(oviora.preferences.streamWindow || "normal"))
                onActivated: (index) => oviora.setPreference("streamWindow", ids[index])
            }
        }
        SettingRow {
            visible: false
            title: qsTr("Pencere boyutu")
            help: qsTr("Genişlik × yükseklik, piksel olarak (en fazla 9999). Varsayılan 1920 × 1080.")
            RowLayout {
                spacing: 10
                InputField {
                    implicitWidth: 110
                    horizontalAlignment: TextInput.AlignHCenter
                    text: "1920"
                    inputMethodHints: Qt.ImhDigitsOnly
                }
                Label {
                    textFormat: Text.PlainText
                    text: "×"
                    font.pixelSize: 18
                    color: Tema.muted
                }
                InputField {
                    implicitWidth: 110
                    horizontalAlignment: TextInput.AlignHCenter
                    text: "1080"
                    inputMethodHints: Qt.ImhDigitsOnly
                }
            }
        }
        SettingRow {
            title: qsTr("Çift tıklayınca tam ekran")
            help: qsTr("Oyun ekranına çift tıklayarak tam ekrana geçip çıkarsın.")
            Toggle {
                checked: oviora.preferences.fullscreenDoubleClick !== false
                onToggled: oviora.setPreference("fullscreenDoubleClick", checked)
            }
        }
        SettingRow {
            title: qsTr("Fare imlecini gizle")
            help: qsTr("Oynarken fare imleci ekranda durmaz.")
            Toggle {
                checked: oviora.preferences.hideCursor === true
                onToggled: oviora.setPreference("hideCursor", checked)
            }
        }
        // New in this app: how long the cursor stays after the last movement
        SettingRow {
            visible: oviora.preferences.hideCursor === true
            title: qsTr("Gizlenme süresi")
            help: qsTr("Fare bu kadar süre kıpırdamazsa imleç oyun ekranından kalkar; fareyi oynatınca geri gelir.")
            ValueSlider {
                implicitWidth: 320
                valueWidth: 52
                from: 1
                to: 10
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: oviora.preferences.hideCursorSeconds !== undefined ? oviora.preferences.hideCursorSeconds : 2
                valueText: qsTr("%1 sn").arg(value.toFixed(0))
                onMoved: oviora.setPreference("hideCursorSeconds", Math.round(value))
            }
        }
    }

    SettingsCard {
        title: qsTr("Görüntü kalitesi")
        description: qsTr("Görüntünün ne kadar işleneceği. Daha yüksek kalite ekran kartını daha çok çalıştırır.")

        // Hidden: the new app's picture processing lives in its own pages (old: videoPreset)
        SettingRow {
            visible: false
            title: qsTr("Kalite")
            help: qsTr("Görüntü takılıyorsa Hızlı'yı dene. Büyütmeli seçenekler düşük çözünürlüğü daha keskin gösterir.")
            Dropdown {
                implicitWidth: 300
                popupMinWidth: 340
                model: [qsTr("Hızlı"), qsTr("Standart"), qsTr("Yüksek kalite"), qsTr("Yüksek kalite ve büyütme"),
                    qsTr("Yüksek kalite ve gelişmiş büyütme"), qsTr("Özel")]
                currentIndex: 1
            }
        }
        // Hidden: old custom placebo dialog
        SettingRow {
            visible: false
            title: qsTr("Özel görüntü işleme")
            help: qsTr("Büyütme, keskinlik ve renk ayarlarını tek tek seç.")
            PillButton {
                text: qsTr("Ayarları aç")
            }
        }
        // Hidden: old display / HDR dialog
        SettingRow {
            visible: false
            title: qsTr("Ekran ve HDR")
            help: qsTr("Ekranının parlaklığına ve renk aralığına göre ayar.")
            PillButton {
                text: qsTr("Ayarları aç")
            }
        }
        // oviora.displaySync: 0 tearing allowed, 1 V-Sync, 2 automatic. On = not tearing; turning it on picks V-Sync.
        SettingRow {
            title: qsTr("Dikey eşitleme (V-Sync)")
            help: qsTr("Görüntüdeki yırtılmaları önler, çok az gecikme ekler.")
            Toggle {
                checked: oviora.displaySync !== 0
                onToggled: oviora.displaySync = checked ? 1 : 0
            }
        }

        AdvancedSection {
            // oviora.ps5.decode: 0 automatic, 1 software, 2 hardware (PS5 streams; saved Oviora-wide)
            SettingRow {
                title: qsTr("Donanımla çözme")
                help: qsTr("Görüntüyü ekran kartının çözmesi. Otomatik çoğu bilgisayarda doğru seçer.")
                RowLayout {
                    spacing: 10
                    Dropdown {
                        implicitWidth: 240
                        model: [qsTr("Otomatik"), qsTr("Kapalı (işlemciyle çöz)"), qsTr("Ekran kartı")]
                        currentIndex: oviora.ps5.decode !== undefined ? oviora.ps5.decode : 0
                        onActivated: (index) => oviora.ps5Set("decode", index)
                    }
                }
            }
            // New in this app (no old row): oviora.ps5.sampling
            SettingRow {
                title: qsTr("İnce renk örnekleme")
                help: qsTr("Renk kenarlarını daha düzgün çizer; çok az ek iş yükü.")
                Toggle {
                    checked: oviora.ps5.sampling === 1
                    onToggled: oviora.ps5Set("sampling", checked ? 1 : 0)
                }
            }
            // Hidden: old zero copy, frame mapping, frame mixer, renderer, Vulkan deferred swap
            SettingRow {
                visible: false
                title: qsTr("Sıfır kopya")
                help: qsTr("Çözülen görüntü kopyalanmadan ekrana gider; bazı ekran kartlarında daha hızlıdır.")
                Toggle {
                    checked: false
                }
            }
            SettingRow {
                visible: false
                title: qsTr("Kare aktarımı")
                help: qsTr("Doğrudan en düşük gecikmeyi verir; sıra kare karıştırmaya izin verir. Değişince uygulama yeniden açılır.")
                Segmented {
                    model: [qsTr("Doğrudan"), qsTr("Sıra ile")]
                    currentIndex: 0
                }
            }
            SettingRow {
                visible: false
                title: qsTr("Kare karıştırma")
                help: qsTr("Ekranın yenileme hızı yayından farklıysa hareketi yumuşatır.")
                Dropdown {
                    implicitWidth: 220
                    model: [qsTr("Yok"), qsTr("Örnekleme"), "Hermite", qsTr("Doğrusal"), qsTr("Kübik")]
                    currentIndex: 0
                }
            }
            SettingRow {
                visible: false
                title: qsTr("Çizim altyapısı")
                help: qsTr("Vulkan önerilir. Görüntü hiç gelmiyorsa OpenGL'i dene. Değişince uygulama yeniden açılır.")
                Segmented {
                    model: ["Vulkan", "OpenGL"]
                    currentIndex: 0
                }
            }
            SettingRow {
                visible: false
                title: qsTr("Vulkan ertelenmiş takas")
                help: qsTr("Bazı sistemlerde takılmaları azaltır.")
                Toggle {
                    checked: false
                }
            }
        }
    }

    // Oviora: big NVIDIA runtimes come only to those who want them (not with every update)
    SettingsCard {
        title: qsTr("Ek bileşenler")
        description: qsTr("Büyük NVIDIA bileşenleri güncellemeyle gelmez; kullanmak istersen buradan indir. İndirdikten sonra Oviora'yı yeniden başlat.")
        Repeater {
            model: [
                { id: "vfg", title: qsTr("NVIDIA VFG kare üretimi"), help: qsTr("Görüntü ayarları → Kare üretimi → NVIDIA VFG için. Yaklaşık 255 MB, RTX ekran kartı gerekir.") },
                { id: "dlssnr", title: qsTr("NVIDIA DLSS Neural Rendering"), help: qsTr("Görüntü ayarları → Neural Rendering (NR) için. Yaklaşık 420 MB, RTX ekran kartı gerekir.") }
            ]
            delegate: SettingRow {
                required property var modelData
                readonly property var c: platform.components[modelData.id] || ({})
                title: modelData.title
                help: c.state === "working" || c.state === "error" || c.state === "done" ? c.text : (c.installed ? qsTr("Yüklü.") : modelData.help)
                PillButton {
                    kind: c.state === "done" ? "primary" : "secondary"
                    visible: platform.configured && platform.status === "active"
                    enabled: c.state !== "working" && (!c.installed || c.state === "done")
                    text: c.state === "done" ? qsTr("Yeniden başlat")
                        : c.state === "working" ? qsTr("İndiriliyor… %1").arg(Math.round((c.progress || 0) * 100) + "%")
                        : c.installed ? qsTr("Yüklü") : qsTr("İndir")
                    onClicked: c.state === "done" ? platform.restartApp() : platform.installComponent(modelData.id)
                }
            }
        }
    }
}
