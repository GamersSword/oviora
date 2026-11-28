import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// Old Oviora's "Yayın" page: a home-network and an internet set for the PS5 (oviora.ps5 / ps5Set,
// saved Oviora-wide). LAN connects use the home set, PSN / internet connects the internet one:
//   quality / remoteQuality 0..3 = 720p30 / 720p60 / 1080p30 / 1080p60 (resolution and frame rate rows),
//   codec / remoteCodec 0..2 = H.264 / H.265 / H.265 HDR,
//   bitrateMbps / remoteBitrateMbps = whole Mbps 2..100 (typed or slid; the old step index bitrate is kept in step),
//   bitrateAuto / remoteBitrateAuto = the old "Otomatik": 10 Mbps at 720p, 15 at 1080p (internet:
//   8 Mbps), back on whenever the resolution changes.
//   The internet set's "Uzak bağlantı" defaults: 1080p, 30 fps, H.265, automatic bit rate.
//   adaptive = Uyarlanabilir kalite ([Stream] adaptive, both sets; off unless turned on, also
//   over the internet).
//   smoothing = Akıcılık tamponu ([Stream] smoothing): Otomatik (on for internet streams) or Kapalı.
// Kept but hidden: the PS5/PS4 choice, the after-stream quality hint.
SettingsPage {
    id: page
    property bool ps5: true

    readonly property var bitrates: [5, 10, 15, 20, 30, 50, 80, 100]

    component QualityCard: SettingsCard {
        id: qc
        property bool remote: false
        // The keys of this card's set in oviora.ps5
        function key(name) { return remote ? "remote" + name.charAt(0).toUpperCase() + name.slice(1) : name }
        function stored(name, fallback) { const v = oviora.ps5[key(name)]; return v !== undefined ? v : fallback }
        readonly property int quality: stored("quality", remote ? 1 : 3)
        readonly property int resolution: quality >= 2 ? 1 : 0     // 0: 720p, 1: 1080p
        readonly property int fps: quality % 2                     // 0: 30, 1: 60
        readonly property bool automatic: stored("bitrateAuto", true) === true
        // The old app's automatic rate: 10 Mbps at 720p, 15 at 1080p; over the internet 8 Mbps at any resolution
        readonly property int autoBitrate: remote ? 8 : resolution ? 15 : 10
        // Whole Mbps, 2-100 (the slider and the typed number); automatic shows the automatic rate
        readonly property int mbps: automatic ? autoBitrate : stored("bitrateMbps", remote ? 10 : 15)
        readonly property string defaultResolution: remote ? "720p" : "1080p"

        SettingRow {
            title: qsTr("Çözünürlük")
            help: qsTr("Varsayılan %1. Bağlantın zayıfsa düşür.").arg(qc.defaultResolution)
            Segmented {
                segmentWidth: 70
                model: ["720p", "1080p"]
                currentIndex: qc.resolution
                onActivated: (index) => {
                    oviora.ps5Set(qc.key("quality"), index * 2 + qc.fps);
                    // As the old app: a new resolution goes back to its automatic bit rate
                    oviora.ps5Set(qc.key("bitrateAuto"), true);
                }
            }
        }
        SettingRow {
            title: qsTr("Kare hızı")
            help: qsTr("60 fps daha akıcıdır, 30 fps daha az bağlantı ister.")
            Segmented {
                segmentWidth: 80
                model: ["30 fps", "60 fps"]
                currentIndex: qc.fps
                onActivated: (index) => oviora.ps5Set(qc.key("quality"), qc.resolution * 2 + index)
            }
        }
        SettingRow {
            title: qsTr("Bit hızı")
            help: !qc.automatic
                ? qsTr("Saniyede gönderilen görüntü verisi. Bu çözünürlük için önerilen %1 Mbps.").arg(qc.autoBitrate)
                : qc.remote ? qsTr("Şu an otomatik: internet için %1 Mbps. Çözünürlüğü değiştirince yeniden otomatiğe döner.").arg(qc.autoBitrate)
                : qsTr("Şu an otomatik: çözünürlüğe göre %1 Mbps. Çözünürlüğü değiştirince yeniden otomatiğe döner.").arg(qc.autoBitrate)
            RowLayout {
                spacing: 8
                ValueSlider {
                    implicitWidth: 240
                    valueWidth: 0
                    from: 2
                    to: 100
                    stepSize: 1
                    value: qc.mbps
                    valueText: ""
                    onMoved: oviora.ps5Set(qc.key("bitrateMbps"), Math.round(value))
                }
                // Any whole number from 2 to 100 typed in (Enter or leaving the box saves it)
                InputField {
                    id: mbpsField
                    implicitWidth: 64
                    horizontalAlignment: TextInput.AlignHCenter
                    leftPadding: 8
                    rightPadding: 8
                    text: String(qc.mbps)
                    validator: IntValidator { bottom: 1; top: 100 }
                    invalid: acceptableInput && (Number(text) < 2 || Number(text) > 100)
                    function save() {
                        const n = Number(text)
                        if (n >= 2 && n <= 100 && n !== qc.mbps) oviora.ps5Set(qc.key("bitrateMbps"), n)
                        else text = String(qc.mbps)
                    }
                    onAccepted: save()
                    onActiveFocusChanged: if (!activeFocus) save()
                }
                Label {
                    textFormat: Text.PlainText
                    text: "Mbps"
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.muted
                }
                PillButton {
                    compact: true
                    kind: "quiet"
                    visible: !qc.automatic
                    text: qsTr("Otomatik")
                    onClicked: oviora.ps5Set(qc.key("bitrateAuto"), true)
                }
            }
        }
        SettingRow {
            title: qsTr("Sıkıştırma")
            help: qsTr("H.265 aynı bit hızında daha net görüntü verir. HDR'yi yalnızca HDR ekranda seç.")
            Segmented {
                model: ["H.264", "H.265", "H.265 HDR"]
                currentIndex: qc.stored("codec", qc.remote ? 1 : 0)
                onActivated: (index) => oviora.ps5Set(qc.key("codec"), index)
            }
        }
    }

    // Hidden: the new app streams from PS5 only
    SettingsCard {
        visible: false
        title: qsTr("Konsol")
        description: qsTr("PS5 ve PS4 için ayrı ayarlar tutulur. Değiştirmek istediğini seç.")
        SettingRow {
            title: qsTr("Ayarladığın konsol")
            Segmented {
                segmentWidth: 80
                model: ["PS5", "PS4"]
                currentIndex: 0
            }
        }
    }

    // Old adaptiveQuality: the lib offers the console one lower resolution too (adaptive_resolution;
    // 1080p -> 720p, 720p -> 540p). On a weak line the PS5 switches profile mid-stream; the decoder
    // reopens at the new size and the engine rebuilds its graph (tested on a PS5 over the LAN).
    SettingsCard {
        title: qsTr("Uyarlanabilir kalite")
        description: qsTr("Deneme aşamasında. Açıkken PS5'e bir alt çözünürlük de önerilir; bağlantı zayıflayınca görüntü donmak yerine kısa süre bulanıklaşabilir.")
        SettingRow {
            title: qsTr("PS5 gerekince çözünürlüğü düşürsün")
            help: qsTr("Bir sonraki yayında geçerli olur. Bağlantıda sorun görürsen kapat ve Sorun bildir ile haber ver.")
            Toggle {
                checked: oviora.ps5.adaptive === true
                onToggled: oviora.ps5Set("adaptive", checked)
            }
        }
    }

    // Internet streams: decoded frames wait a frame or two (33..66 ms at 30 fps) and leave on an
    // even cadence, so a jittery line does not stutter on screen (RemotePlaySessionSource).
    SettingsCard {
        title: qsTr("Akıcılık tamponu")
        description: qsTr("İnternet yayınında görüntüyü 1-2 kare bekletip eşit aralıkla gösterir; uzak ve dalgalı bağlantıda takılmayı azaltır. Ev ağında kullanılmaz.")
        SettingRow {
            title: qsTr("Tampon")
            help: qsTr("Otomatik: hat sakinken 1 kare, dalgalanınca 2 kare bekler. Kapalı: her kare geldiği an gösterilir.")
            Segmented {
                segmentWidth: 100
                model: [qsTr("Otomatik"), qsTr("Kapalı")]
                currentIndex: oviora.ps5.smoothing === false ? 1 : 0
                onActivated: (index) => oviora.ps5Set("smoothing", index === 0)
            }
        }
    }

    // Hidden: no after-stream hint yet (old: yayinOnerisi)
    SettingsCard {
        visible: false
        title: qsTr("Yayın sonu")
        SettingRow {
            title: qsTr("Kötü geçen yayından sonra kalite öner")
            help: qsTr("Yayında çok paket kaybı ya da takılma olduysa, kapanınca çözünürlüğü veya kare hızını düşürmeyi önerir.")
            Toggle {
                checked: false
            }
        }
    }

    // LAN connects (this network)
    QualityCard {
        remote: false
        title: qsTr("Ev ağında")
        description: qsTr("Konsol ile bilgisayar aynı modemdeyken. Kablolu bağlantıda en yüksek ayarlar rahat çalışır.")
    }

    // PSN connects (bridge connectPsn)
    QualityCard {
        remote: true
        title: qsTr("İnternet üzerinden")
        description: qsTr("Evden uzaktayken ya da bir arkadaşının PS5'inde. Görüntü takılırsa önce bit hızını düşür.")
    }
}
