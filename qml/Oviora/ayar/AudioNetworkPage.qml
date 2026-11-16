import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// Old Oviora's "Ses ve ağ" page. Wired: the output device (oviora.audioDevices /
// preferences.audioDevice), the volume (oviora.volume, 0..1, kept for the next start), the
// stereo downmix (preferences.audioForceStereo, new in this app), the microphone to the PS5
// (oviora.microphones / preferences.micDevice, preferences.micStartOn), the stream statistics
// (preferences.showStreamStats, also the stream menu's İstatistikler tile) and the weak-connection
// warning (preferences.weakLinkWarning, %).
// Kept but hidden: the microphone's noise and echo processing (the old app used speex, which this
// build of the lib leaves out), the audio buffer, the packet loss / keyframe settings.
SettingsPage {
    id: page
    onVisibleChanged: if (visible) oviora.refreshAudioDevices()

    SettingsCard {
        title: qsTr("Ses")
        description: qsTr("Oyun sesinin nereden çıkacağı ve konsola hangi mikrofonun gideceği.")

        // oviora.audioDevices: [{id, label, isDefault, missing}], the first one (id "") follows Windows
        SettingRow {
            title: qsTr("Hoparlör ya da kulaklık")
            help: qsTr("Otomatik, Windows'ta seçili olanı kullanır.")
            Dropdown {
                readonly property var devices: oviora.audioDevices
                implicitWidth: 320
                popupMinWidth: 460
                model: devices.map((d, i) => i === 0 && d.id === "" ? qsTr("Otomatik") : d.label)
                currentIndex: {
                    const chosen = oviora.preferences.audioDevice || "";
                    for (let i = 0; i < devices.length; i++)
                        if (devices[i].id === chosen)
                            return i;
                    return 0;
                }
                onActivated: (index) => oviora.setPreference("audioDevice", devices[index] ? devices[index].id : "")
            }
        }
        // The recording device by its name (old: audioInDevice); "" = Windows' default
        SettingRow {
            title: qsTr("Mikrofon")
            help: qsTr("Oyun içi sesli sohbet için.")
            Dropdown {
                implicitWidth: 320
                popupMinWidth: 460
                model: [qsTr("Otomatik")].concat(oviora.microphones)
                currentIndex: Math.max(0, model.indexOf(oviora.preferences.micDevice || ""))
                onActivated: (index) => oviora.setPreference("micDevice", index ? model[index] : "")
            }
        }
        SettingRow {
            title: qsTr("Ses düzeyi")
            help: qsTr("Oyun sesinin bilgisayardaki yüksekliği.")
            ValueSlider {
                implicitWidth: 320
                valueWidth: 52
                from: 0
                to: 100
                stepSize: 1
                value: Math.round(oviora.volume * 100)
                valueText: "%" + value.toFixed(0)
                onMoved: oviora.volume = value / 100
            }
        }
        // New in this app (no old row): preferences.audioForceStereo
        SettingRow {
            title: qsTr("Sesi stereoya indir")
            help: qsTr("Çok kanallı ses iki kanala karıştırılır; kulaklıkta ya da iki hoparlörde işine yarar.")
            Toggle {
                checked: oviora.preferences.audioForceStereo === true
                onToggled: oviora.setPreference("audioForceStereo", checked)
            }
        }
        // Old startMicUnmuted (off by default)
        SettingRow {
            title: qsTr("Mikrofon açık başlasın")
            help: qsTr("Kapalıyken mikrofonu yayın menüsünden açarsın.")
            Toggle {
                checked: oviora.preferences.micStartOn === true
                onToggled: oviora.setPreference("micStartOn", checked)
            }
        }
        // Hidden: the new engine sizes its own audio buffer (old: audioBufferSize)
        AdvancedSection {
            visible: false
            SettingRow {
                title: qsTr("Ses arabelleği")
                help: qsTr("Ses cızırdıyor ya da kesiliyorsa artır; daha düşük değer daha az gecikme demek. Varsayılan 50 ms.")
                ValueSlider {
                    implicitWidth: 320
                    valueWidth: 60
                    from: 1
                    to: 10
                    stepSize: 1
                    snapMode: Slider.SnapAlways
                    value: 5
                    valueText: (value * 10).toFixed(0) + " ms"
                }
            }
        }
    }

    // Old showStreamStats and wifiDroppedNotif (Main.qml draws both over the picture); the advanced
    // rows (packetLossReportedMax, iDROnFECFailureEnabled) stay hidden: the engine keeps 5% and on
    SettingsCard {
        title: qsTr("Bağlantı")
        description: qsTr("Bağlantının durumunu görmek ve zayıfladığında haber almak için.")

        SettingRow {
            title: qsTr("Yayın bilgilerini göster")
            help: qsTr("Oynarken köşede bit hızı, gecikme ve kayıp paketler yazar.")
            Toggle {
                checked: oviora.preferences.showStreamStats === true
                onToggled: oviora.setPreference("showStreamStats", checked)
            }
        }
        SettingRow {
            title: qsTr("Zayıf bağlantı uyarısı")
            help: qsTr("Kaybolan paketler bu oranı geçince ekranda uyarı çıkar. Varsayılan %3.")
            ValueSlider {
                implicitWidth: 320
                valueWidth: 52
                from: 0
                to: 100
                stepSize: 1
                value: oviora.preferences.weakLinkWarning !== undefined ? oviora.preferences.weakLinkWarning : 3
                valueText: "%" + value.toFixed(0)
                onMoved: oviora.setPreference("weakLinkWarning", Math.round(value))
            }
        }

        AdvancedSection {
            visible: false
            SettingRow {
                title: qsTr("Konsola bildirilen en yüksek kayıp")
                help: qsTr("Konsol, kaybı bu orana kadar bilir ve görüntü kalitesini buna göre ayarlar. Varsayılan %5.")
                ValueSlider {
                    implicitWidth: 320
                    valueWidth: 52
                    from: 0
                    to: 100
                    stepSize: 1
                    value: 5
                    valueText: "%" + value.toFixed(0)
                }
            }
            SettingRow {
                title: qsTr("Bozuk karede tam kare iste")
                help: qsTr("Bir kare onarılamazsa konsoldan hemen yeni bir tam kare ister. Görüntü uzun süre bozuk kalıyorsa aç.")
                Toggle {
                    checked: false
                }
            }
        }
    }
}
