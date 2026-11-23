import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// Old Oviora's "Altyazı" page (settings/SubtitlePage.qml): reads the game's subtitle off the PS5 picture
// and writes the Turkish over it. Wired to the context property "subtitles" (src/platform/subtitle).
// Not ported: the old "Okuma yöntemi" choice (Tesseract); reading is always PaddleOCR.
SettingsPage {
    id: page
    signal confirmRequested(string title, string text, var action)
    readonly property var sub: subtitles
    readonly property var cfg: subtitles.config
    readonly property var ai: subtitles.localAi
    readonly property bool usesLocal: cfg.engine == 2 || cfg.engine == 4
    readonly property bool gpuGood: ai.gpu.indexOf("NVIDIA") >= 0 || ai.gpu.indexOf("AMD") >= 0

    component ShortcutHint: RowLayout {
        property string keys
        property string label
        spacing: 10
        KeyCap {
            small: true
            text: parent.keys
        }
        Label {
            textFormat: Text.PlainText
            text: parent.label
            font.family: Tema.body
            font.pixelSize: 13
            color: Tema.text
        }
    }

    component Requirement: RowLayout {
        property string name
        property string value
        spacing: 12
        Layout.fillWidth: true
        Label {
            textFormat: Text.PlainText
            Layout.preferredWidth: 104
            Layout.alignment: Qt.AlignTop
            text: parent.name
            font.family: Tema.body
            font.pixelSize: 13
            color: Tema.muted
        }
        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: parent.value
            wrapMode: Text.WordWrap
            font.family: Tema.body
            font.pixelSize: 13
            color: Tema.text
        }
    }

    Notice {
        width: parent.width
        visible: !page.sub.available
        tone: "error"
        text: qsTr("Altyazı okuma dosyaları bu kurulumda bulunamadı (runtime/altyazi/ocr). Oviora'yı yeniden kurmayı dene.")
    }

    SettingsCard {
        title: qsTr("Türkçe altyazı")
        description: qsTr("Oyundaki İngilizce altyazıyı okur, Türkçesini tam üstüne yazar. Oyunun kendi ayarlarından altyazıyı açmayı unutma.")
        enabled: page.sub.available

        SettingRow {
            title: qsTr("Altyazıyı çevir")
            help: page.sub.status ? qsTr("Durum: %1").arg(page.sub.status) : qsTr("Oynarken Ctrl+T ile de açıp kapatabilirsin.")
            Toggle {
                checked: page.cfg.enabled
                onToggled: page.cfg.enabled = checked
            }
        }
        SettingRow {
            stacked: true
            title: qsTr("Oynarken kısayollar")
            help: qsTr("Altyazı yanlış yerde okunuyorsa bölgeyi fareyle yeniden çiz.")
            GridLayout {
                width: parent.width
                columns: width > 560 ? 2 : 1
                columnSpacing: 24
                rowSpacing: 10
                ShortcutHint { keys: "Ctrl+T"; label: qsTr("Çeviriyi aç ya da kapat") }
                ShortcutHint { keys: "Ctrl+B"; label: qsTr("Altyazı kutusunu fareyle çiz") }
                ShortcutHint { keys: "Ctrl+R"; label: qsTr("Okunan bölgeyi göster ve düzenle") }
                ShortcutHint { keys: "Ctrl+D"; label: qsTr("Hata kaydını aç ya da kapat") }
                ShortcutHint { keys: "Ctrl+E"; label: qsTr("Ekrandaki çeviriyi düzelt") }
            }
        }
    }

    SettingsCard {
        title: qsTr("Çeviri")
        description: qsTr("Okunan yazının nasıl Türkçeye çevrileceği.")
        enabled: page.sub.available

        SettingRow {
            title: qsTr("Ortak çeviri hafızası")
            help: qsTr("Çevrilen satırlar Oviora sunucusunda saklanır; aynı satır başkasına hazır gelir, kötü çeviriyi Ctrl+E ile düzeltebilirsin. Yalnız oyundaki yazı gider.")
            Toggle {
                checked: page.cfg.sharedMemory
                onToggled: page.cfg.sharedMemory = checked
            }
        }

        SettingRow {
            title: qsTr("Çeviren")
            help: page.cfg.engine === 4
                ? qsTr("İkisi aynı anda çevirir, önce gelen gösterilir. En hızlısı budur.")
                : page.cfg.engine === 1 ? qsTr("DeepL için kendi API anahtarın gerekir.")
                : page.cfg.engine === 2 ? qsTr("Yalnızca bu bilgisayardaki yapay zeka çevirir; internet gerekmez.")
                : qsTr("Anahtar gerekmez; internet bağlantısı yeterli.")
            Dropdown {
                implicitWidth: 320
                popupMinWidth: 360
                model: [qsTr("Google (anahtarsız)"), qsTr("DeepL (API anahtarı ile)"), qsTr("Yerel yapay zeka"), qsTr("Yerel yapay zeka ve Google birlikte")]
                // engine 3 is the internal MyMemory fallback, so the race (engine 4) sits at index 3
                currentIndex: page.cfg.engine === 4 ? 3 : page.cfg.engine
                onActivated: (index) => page.cfg.engine = index === 3 ? 4 : index
            }
        }
        SettingRow {
            visible: page.cfg.engine == 1
            title: qsTr("DeepL API anahtarı")
            help: qsTr("Ücretsiz anahtarlar :fx ile biter.")
            InputField {
                implicitWidth: 320
                echoMode: TextInput.PasswordEchoOnEdit
                placeholderText: qsTr("Anahtarı yapıştır")
                text: page.cfg.deeplKey
                onEditingFinished: page.cfg.deeplKey = text.trim()
            }
        }
    }

    // ── The local translation AI: optional download, with what it needs from the PC ──
    SettingsCard {
        id: aiCard
        title: qsTr("Yerel çeviri yapay zekası")
        description: qsTr("İsteğe bağlı. Çeviriyi bu bilgisayarda yapar ve Google ile yarıştırır; genelde daha hızlı ve tutarlı olur.")
        enabled: page.sub.available

        GridLayout {
            Layout.fillWidth: true
            Layout.topMargin: 14
            Layout.bottomMargin: 6
            columns: aiCard.width > 760 ? 2 : 1
            columnSpacing: 20
            rowSpacing: 16

            // left: what is installed, and the button
            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: 10
                spacing: 12

                RowLayout {
                    spacing: 10
                    Rectangle {
                        implicitWidth: 10
                        implicitHeight: 10
                        radius: 5
                        color: page.ai.state === "installed" || page.ai.state === "system" ? Tema.ok
                            : page.ai.state === "error" ? Tema.danger
                            : page.ai.state === "working" ? Tema.accent : Tema.faint
                    }
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: page.ai.state === "installed" || page.ai.state === "system" ? qsTr("Kurulu")
                            : page.ai.state === "working" ? qsTr("İndiriliyor")
                            : page.ai.state === "error" ? qsTr("Kurulamadı") : qsTr("Kurulu değil")
                        font.family: Tema.display
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                        color: Tema.text
                    }
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: page.ai.status
                    wrapMode: Text.WordWrap
                    font.family: Tema.body
                    font.pixelSize: 14
                    color: page.ai.state === "error" ? Tema.danger : Tema.text
                }
                // progress
                Item {
                    Layout.fillWidth: true
                    implicitHeight: 8
                    visible: page.ai.state === "working"
                    Rectangle {
                        anchors.fill: parent
                        radius: 4
                        color: Tema.line
                    }
                    Rectangle {
                        id: progressFill
                        height: parent.height
                        radius: 4
                        color: Tema.accent
                        width: page.ai.progress < 0 ? parent.width * 0.3 : parent.width * Math.max(0, Math.min(1, page.ai.progress))
                        x: page.ai.progress < 0 ? sweep.x : 0
                        Behavior on width { enabled: page.ai.progress >= 0; NumberAnimation { duration: 200 } }
                    }
                    Item {
                        id: sweep
                        property real span: parent.width * 0.7
                        SequentialAnimation on x {
                            running: page.ai.state === "working" && page.ai.progress < 0 && sweep.visible
                            loops: Animation.Infinite
                            NumberAnimation { from: 0; to: sweep.span; duration: 900; easing.type: Easing.InOutSine }
                            NumberAnimation { from: sweep.span; to: 0; duration: 900; easing.type: Easing.InOutSine }
                        }
                    }
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: page.ai.state === "missing" || page.ai.state === "error"
                    text: qsTr("İndirilecek: Ollama ve ekran kartı dosyaları (%1, yaklaşık 0,1–1 GB) ile çeviri modeli (3,3 GB). Yarıda kalırsa kaldığı yerden devam eder.").arg(page.ai.gpu)
                    wrapMode: Text.WordWrap
                    font.family: Tema.body
                    font.pixelSize: 13
                    lineHeight: 1.1
                    color: Tema.muted
                }
                RowLayout {
                    spacing: 10
                    PillButton {
                        kind: "primary"
                        text: page.ai.state === "error" ? qsTr("Tekrar dene") : qsTr("İndir ve kur")
                        visible: page.ai.state === "missing" || page.ai.state === "error"
                        onClicked: page.ai.install()
                    }
                    PillButton {
                        text: qsTr("Durdur")
                        visible: page.ai.state === "working"
                        onClicked: page.ai.cancel()
                    }
                    PillButton {
                        kind: "danger"
                        text: qsTr("Kaldır ve yer aç")
                        visible: page.ai.state === "installed"
                        onClicked: page.confirmRequested(qsTr("Yerel çeviri yapay zekası"),
                            qsTr("Ollama ve çeviri modeli bu klasörden silinsin mi? Altyazılar Google ile çevrilmeye devam eder; istediğinde tekrar indirebilirsin."),
                            () => { page.cfg.engine = 0; page.ai.uninstall(); })
                    }
                }
            }

            // right: estimated requirements, and what this PC has
            Rectangle {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: 10
                implicitHeight: reqCol.implicitHeight + 32
                // a recessed note beside the rows, not another card
                radius: Tema.radiusInput
                color: Tema.bg
                ColumnLayout {
                    id: reqCol
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                        margins: 16
                        leftMargin: 18
                    }
                    spacing: 9
                    Label {
                        textFormat: Text.PlainText
                        text: qsTr("Tahmini gereksinim")
                        font.family: Tema.display
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: Tema.text
                    }
                    Requirement { name: qsTr("Ekran kartı"); value: qsTr("NVIDIA ya da AMD") }
                    Requirement { name: qsTr("Ekran belleği"); value: qsTr("En az 6 GB (4 GB'ta yavaşlar)") }
                    Requirement { name: qsTr("Bellek (RAM)"); value: qsTr("8 GB") }
                    Requirement { name: qsTr("İndirme"); value: qsTr("Yaklaşık 4–5 GB") }
                    Requirement { name: qsTr("Disk alanı"); value: qsTr("Yaklaşık 6 GB") }
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Ekran kartı yoksa çeviri çok yavaş olur. Ölçülen: NVIDIA RTX 4060 ile satır başına yaklaşık 0,25 saniye; genel sürücüyle (Vulkan) yaklaşık 3 saniye.")
                        font.family: Tema.body
                        font.pixelSize: 12
                        lineHeight: 1.1
                        color: Tema.muted
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: 2
                        implicitHeight: 1
                        color: Tema.line
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Rectangle {
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 5
                            implicitWidth: 8
                            implicitHeight: 8
                            radius: 4
                            color: page.gpuGood ? Tema.ok : Tema.warm
                        }
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: page.gpuGood
                                ? qsTr("Bu bilgisayarda bulunan: %1. Uygun görünüyor.").arg(page.ai.gpu)
                                : qsTr("Bu bilgisayarda NVIDIA ya da AMD ekran kartı bulunamadı; çeviri genel sürücüyle yavaş çalışır (satır başına birkaç saniye).")
                            font.family: Tema.body
                            font.pixelSize: 13
                            color: page.gpuGood ? Tema.ok : Tema.warm
                        }
                    }
                }
            }
        }

        AdvancedSection {
            visible: page.usesLocal
            title: qsTr("Model ayarları")
            SettingRow {
                title: qsTr("Model adresi")
                help: qsTr("Varsayılan, bu bilgisayardaki Ollama.")
                InputField {
                    implicitWidth: 340
                    text: page.cfg.llmUrl
                    onEditingFinished: page.cfg.llmUrl = text.trim()
                }
            }
            SettingRow {
                title: qsTr("Model adı")
                help: qsTr("Varsayılan translategemma:4b.")
                InputField {
                    implicitWidth: 340
                    text: page.cfg.llmModel
                    onEditingFinished: page.cfg.llmModel = text.trim()
                }
            }
        }
    }

    SettingsCard {
        title: qsTr("Okuma")
        description: qsTr("Altyazının ekrandan nasıl okunacağı. Yanlış ya da eksik okuyorsa buraya bak.")
        enabled: page.sub.available

        SettingRow {
            title: qsTr("Yalnızca kenarlıklı yazı")
            help: qsTr("Koyu kenarlıklı altyazıları okur, arka plandaki yazıları eler. Oyunun altyazısı kenarlıksızsa kapat.")
            Toggle {
                checked: page.cfg.requireOutline
                onToggled: page.cfg.requireOutline = checked
            }
        }
        SettingRow {
            title: qsTr("Yalnızca ortalanmış satırlar")
            help: qsTr("Oyundaki telefon ve menü yazılarını eler.")
            Toggle {
                checked: page.cfg.centeredOnly
                onToggled: page.cfg.centeredOnly = checked
            }
        }

        AdvancedSection {
            SettingRow {
                title: qsTr("Yazı parlaklığı eşiği")
                help: qsTr("Bundan parlak noktalar yazı sayılır. Yazı kaçıyorsa düşür, gürültü okunuyorsa artır. Varsayılan 180.")
                ValueSlider {
                    implicitWidth: 300
                    valueWidth: 44
                    from: 50
                    to: 250
                    stepSize: 5
                    value: page.cfg.threshold
                    valueText: value.toFixed(0)
                    onMoved: page.cfg.threshold = value
                }
            }
            SettingRow {
                title: qsTr("Okuma sıklığı")
                help: qsTr("Ekranın ne kadar sık okunacağı. Daha kısa süre daha çabuk tepki verir ama işlemciyi yorar. Varsayılan 150 ms.")
                ValueSlider {
                    implicitWidth: 300
                    valueWidth: 70
                    from: 100
                    to: 1000
                    stepSize: 50
                    value: page.cfg.intervalMs
                    valueText: value.toFixed(0) + " ms"
                    onMoved: page.cfg.intervalMs = value
                }
            }
            SettingRow {
                title: qsTr("Hedef dil")
                help: qsTr("Çevrilecek dilin kodu. Varsayılan tr.")
                InputField {
                    implicitWidth: 120
                    horizontalAlignment: TextInput.AlignHCenter
                    text: page.cfg.targetLang
                    onEditingFinished: page.cfg.targetLang = text.trim().toLowerCase()
                }
            }
            SettingRow {
                title: qsTr("Okunan bölge")
                help: qsTr("Soldan, üstten, genişlik ve yükseklik; ekranın yüzdesi olarak. Değiştirmek için oynarken Ctrl+B.")
                Label {
                    textFormat: Text.PlainText
                    text: [page.cfg.regionX, page.cfg.regionY, page.cfg.regionW, page.cfg.regionH]
                        .map(v => "%" + (v * 100).toFixed(0)).join("  ·  ")
                    font.family: Tema.display
                    font.pixelSize: 15
                    color: Tema.text
                }
            }
            SettingRow {
                title: qsTr("Hata kaydı")
                help: qsTr("Okunan görüntüleri logs/altyazi klasörüne kaydeder; yalnızca sorun ararken aç.")
                Toggle {
                    checked: page.cfg.debugDump
                    onToggled: page.cfg.debugDump = checked
                }
            }
        }
    }

    SettingsCard {
        title: qsTr("Görünüm")
        description: qsTr("Türkçe altyazının ekranda nasıl duracağı.")
        enabled: page.sub.available

        SettingRow {
            title: qsTr("Yazı boyutu")
            help: qsTr("Oyunun kendi altyazısına göre. Varsayılan %100.")
            ValueSlider {
                implicitWidth: 300
                valueWidth: 56
                from: 0.5
                to: 2
                stepSize: 0.1
                value: page.cfg.fontRel
                valueText: "%" + (value * 100).toFixed(0)
                onMoved: page.cfg.fontRel = value
            }
        }
        SettingRow {
            title: qsTr("Arka plan koyuluğu")
            help: qsTr("Türkçe yazının arkasındaki kutu. Varsayılan %92.")
            ValueSlider {
                implicitWidth: 300
                valueWidth: 56
                from: 0
                to: 100
                stepSize: 5
                value: page.cfg.boxOpacity
                valueText: "%" + value.toFixed(0)
                onMoved: page.cfg.boxOpacity = value
            }
        }
        SettingRow {
            title: qsTr("İngilizce altyazıyı kapat")
            help: qsTr("Türkçe yazı, oyunun kendi altyazısının üstünü örter.")
            Toggle {
                checked: page.cfg.coverOriginal
                onToggled: page.cfg.coverOriginal = checked
            }
        }
        SettingRow {
            title: qsTr("İngilizcesini de göster")
            help: qsTr("Türkçenin altında aslını küçük yazar.")
            Toggle {
                checked: page.cfg.showSource
                onToggled: page.cfg.showSource = checked
            }
        }
        SettingRow {
            title: qsTr("Okunan bölgeyi göster")
            help: qsTr("Oyunda okunan alanın çerçevesini gösterir; düzenlemek için. Kısayolu Ctrl+R.")
            Toggle {
                checked: page.cfg.showRegion
                onToggled: page.cfg.showRegion = checked
            }
        }
        SettingRow {
            title: qsTr("Altyazı ayarlarını sıfırla")
            help: qsTr("Bu sayfadaki her şey ilk haline döner.")
            PillButton {
                kind: "quiet"
                text: qsTr("Varsayılana döndür")
                onClicked: page.sub.resetDefaults()
            }
        }
    }
}
