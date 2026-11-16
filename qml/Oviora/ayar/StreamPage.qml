import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// Old Oviora's "Yayın" page: a home-network and an internet set for the PS5 (oviora.ps5 / ps5Set,
// saved Oviora-wide). LAN connects use the home set, PSN / internet connects the internet one:
//   quality / remoteQuality 0..3 = 720p30 / 720p60 / 1080p30 / 1080p60 (resolution and frame rate rows),
//   codec / remoteCodec 0..2 = H.264 / H.265 / H.265 HDR,
//   bitrate / remoteBitrate index 0..7 = 5/10/15/20/30/50/80/100 Mbps,
//   bitrateAuto / remoteBitrateAuto = the old "Otomatik": 10 Mbps at 720p, 15 at 1080p, back on
//   whenever the resolution changes.
//   adaptive = Uyarlanabilir kalite (old adaptiveQuality, off by default; [Stream] adaptive, both sets).
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
        // The old app's automatic rate for 720p and 1080p (index 1 = 10 Mbps, 2 = 15 Mbps)
        readonly property int autoBitrate: resolution ? 15 : 10
        readonly property int bitrate: automatic ? (resolution ? 2 : 1) : stored("bitrate", remote ? 1 : 2)
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
                : qsTr("Şu an otomatik: çözünürlüğe göre %1 Mbps. Çözünürlüğü değiştirince yeniden otomatiğe döner.").arg(qc.autoBitrate)
            RowLayout {
                spacing: 8
                ValueSlider {
                    implicitWidth: 300
                    valueWidth: 82
                    from: 0
                    to: page.bitrates.length - 1
                    stepSize: 1
                    snapMode: Slider.SnapAlways
                    value: qc.bitrate
                    valueText: page.bitrates[Math.round(value)] + " Mbps"
                    onMoved: oviora.ps5Set(qc.key("bitrate"), Math.round(value))
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
