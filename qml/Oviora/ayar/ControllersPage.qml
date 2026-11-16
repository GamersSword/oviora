import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Oviora

// Old Oviora's "Kumandalar" page. The new app's controller settings (no old rows) come first:
// whether a controller is connected and the gyro calibration (oviora.ps5.controller / gyro /
// calibrating, oviora.ps5Calibrate) and view-only (oviora.ps5.viewOnly).
// Wired (bridge/GameInput.cpp): the button mapping (PadMappingDialog, preference padButtonMap),
// rumble strength (rumbleStrength 0..5, Normal 3) and DualSense haptics (hapticStrength %, 100).
// Kept but hidden until the new app has them: buttons by position, background input,
// Steam Deck, d-pad as touchpad.
SettingsPage {
    id: page
    signal mappingRequested(bool reset)
    property alias changeMappingButton: changeMapping
    property alias resetMappingButton: resetMapping

    readonly property var comboButtons: [qsTr("Kullanılmıyor"), qsTr("Çarpı"), qsTr("Daire"), qsTr("Kare"), qsTr("Üçgen"),
        qsTr("Yön sol"), qsTr("Yön sağ"), qsTr("Yön yukarı"), qsTr("Yön aşağı"), "L1", "R1", "L3", "R3",
        "Options", "Create", qsTr("Dokunmatik yüzey"), qsTr("PS düğmesi")]

    SettingsCard {
        title: qsTr("Kumanda")
        description: oviora.ps5.controller ? qsTr("Bilgisayara bağlı bir kumanda var.") : qsTr("Bilgisayara bağlı kumanda bulunamadı. DualSense'i kabloyla ya da Bluetooth ile bağla.")
        SettingRow {
            title: qsTr("Jiroskop")
            help: oviora.ps5.gyro ? qsTr("Kullanılabilir.") : qsTr("Bu kumandada yok ya da kullanılamıyor.")
            PillButton {
                text: oviora.ps5.calibrating ? qsTr("Ayarlanıyor…") : qsTr("Ayarla")
                enabled: oviora.ps5.gyro === true && !oviora.ps5.calibrating
                onClicked: oviora.ps5Calibrate()
            }
        }
        SettingRow {
            title: qsTr("Yalnız izle")
            help: qsTr("Bilgisayardaki kumanda PS5'e gönderilmez; oyunu PS5'in kendi kumandasıyla oynarsın.")
            Toggle {
                checked: oviora.ps5.viewOnly === true
                onToggled: oviora.ps5Set("viewOnly", checked)
            }
        }
    }

    readonly property var padMap: oviora.preferences.padButtonMap || ({})
    readonly property int padChanged: Object.keys(padMap).length
    onMappingRequested: (reset) => {
        if (reset)
            oviora.setPreference("padButtonMap", {});
        else
            mappingDialog.open();
    }
    PadMappingDialog {
        id: mappingDialog
        buttonNames: page.comboButtons.slice(1)
    }

    // The button mapping (old: controller mapping dialog). Hidden: by-position buttons and
    // background input (old: buttonsByPosition, allowJoystickBackgroundEvents)
    SettingsCard {
        title: qsTr("Kumanda tuşları")
        description: qsTr("DualSense ve DualShock kendiliğinden tanınır. Başka bir kumandanın tuşları yanlış çalışıyorsa burada yeniden eşle.")

        SettingRow {
            title: qsTr("Kumandanın tuşlarını eşle")
            help: page.padChanged > 0
                ? qsTr("%1 tuş başka bir tuşa eşlendi.").arg(page.padChanged)
                : qsTr("Bir kutuya tıkla, sonra kumandada o DualSense tuşunun yerine geçecek tuşa bas")
            RowLayout {
                spacing: 10
                PillButton {
                    id: changeMapping
                    text: qsTr("Eşle")
                    onClicked: page.mappingRequested(false)
                }
                PillButton {
                    id: resetMapping
                    enabled: page.padChanged > 0
                    kind: "quiet"
                    text: qsTr("Varsayılana döndür")
                    onClicked: page.mappingRequested(true)
                }
            }
        }
        SettingRow {
            visible: false
            title: qsTr("Tuşları yerine göre kullan")
            help: qsTr("Nintendo düzenindeki kumandalarda tuşlar üstündeki harfe göre değil, durduğu yere göre çalışır.")
            Toggle {
                checked: false
            }
        }
        SettingRow {
            title: qsTr("Pencere arkadayken de dinle")
            help: qsTr("Oviora öne alınmamışken de kumanda tuşları konsola gider.")
            Toggle {
                checked: oviora.preferences.padBackground !== false
                onToggled: oviora.setPreference("padBackground", checked)
            }
        }
    }

    // Rumble and haptics strength (old: rumbleHapticsIntensity, hapticOverride); Steam Deck hidden
    SettingsCard {
        title: qsTr("Titreşim")
        description: qsTr("Oyunların gönderdiği titreşimin kumandanda ne kadar güçlü hissedileceği.")

        SettingRow {
            title: qsTr("Titreşim gücü")
            help: qsTr("DualSense dışındaki kumandalar ve DualSense'in basit titreşimi için. Varsayılan Normal.")
            Dropdown {
                implicitWidth: 220
                model: [qsTr("Kapalı"), qsTr("Çok zayıf"), qsTr("Zayıf"), qsTr("Normal"), qsTr("Güçlü"), qsTr("Çok güçlü")]
                currentIndex: oviora.preferences.rumbleStrength !== undefined ? oviora.preferences.rumbleStrength : 3
                onActivated: (index) => oviora.setPreference("rumbleStrength", index)
            }
        }
        SettingRow {
            title: qsTr("DualSense dokunsal geri bildirim")
            help: qsTr("Gerçekçi titreşimin gücü. Ortada konsolundaki ayar aynen kullanılır.")
            ValueSlider {
                implicitWidth: 320
                valueWidth: 110
                from: 0
                to: 2
                stepSize: 0.1
                value: (oviora.preferences.hapticStrength !== undefined ? oviora.preferences.hapticStrength : 100) / 100
                valueText: value > 0.99 && value < 1.01 ? qsTr("Konsoldaki gibi") : "%" + (value * 100).toFixed(0)
                onMoved: oviora.setPreference("hapticStrength", Math.round(value * 100))
            }
        }
        AdvancedSection {
            visible: false
            title: qsTr("Steam Deck")
            SettingRow {
                title: qsTr("Steam Deck dokunsal geri bildirim")
                help: qsTr("Steam Deck'te gerçek dokunsal titreşim; daha iyi hissedilir ama daha gürültülüdür.")
                Toggle {
                    checked: false
                }
            }
            SettingRow {
                title: qsTr("Steam Deck dik tutuluyor")
                help: qsTr("Hareket algılayıcıları Steam Deck dik tutuluyormuş gibi okunur.")
                Toggle {
                    checked: false
                }
            }
        }
    }

    // Hidden: no d-pad touchpad yet (old: dpadTouchEnabled, dpadTouchShortcut1..4, dpadTouchIncrement)
    SettingsCard {
        visible: false
        title: qsTr("Yön tuşlarıyla dokunmatik yüzey")
        description: qsTr("Dokunmatik yüzeyi olmayan kumandalar için: bir tuş kombinasyonuyla yön tuşları, yüzeyde parmak kaydırmaya dönüşür.")

        SettingRow {
            title: qsTr("Kullan")
            Toggle {
                checked: false
            }
        }
        SettingRow {
            stacked: true
            title: qsTr("Açıp kapatan tuşlar")
            help: qsTr("Varsayılan: L1 + R1 + yön yukarı.")
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
                        currentIndex: [9, 10, 7, 0][index]
                    }
                }
            }
        }
        SettingRow {
            title: qsTr("Kaydırma adımı")
            help: qsTr("Her basışta parmağın ne kadar kayacağı. Varsayılan 0,3 mm.")
            ValueSlider {
                implicitWidth: 320
                valueWidth: 76
                from: 1
                to: 1079
                stepSize: 1
                value: 30
                valueText: (value / 100).toFixed(2).replace(".", ",") + " mm"
            }
        }
    }
}
