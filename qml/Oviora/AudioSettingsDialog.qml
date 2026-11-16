// audio dialog: source/settings logic is owned by this component.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

DialogLayer {
    key: "audio"
    glyph: "music"
    title: qsTr("音频设置")
    dialogWidth: 560
    actions: [{ label: qsTr("完成"), primary: true }]
    onActionTriggered: dialogHost.close()

    DialogSection { text: qsTr("音轨") }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 6
        Text {
            textFormat: Text.PlainText
            visible: oviora.audioTracks.length === 0
            text: qsTr("当前源没有可选音轨。")
            color: Theme.t3
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsSmall
        }
        Repeater {
            model: oviora.audioTracks
            delegate: Rectangle {
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: 52
                radius: 12
                color: Theme.card2
                border.width: 1
                border.color: oviora.selectedAudioTrack === modelData.index ? Theme.accent : Theme.stroke
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 12
                    // .trk .bars: the playing track's bars move (M39).
                    VEqBars {
                        objectName: "audio-track-bars-" + modelData.index
                        active: oviora.selectedAudioTrack === modelData.index && oviora.running && !oviora.paused
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            textFormat: Text.PlainText
                            text: modelData.label
                            color: Theme.t1
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fsBody
                        }
                        Text {
                            textFormat: Text.PlainText
                            text: modelData.channels > 0 ? modelData.channels + qsTr(" 声道") : ""
                            color: Theme.t3
                            font.family: Theme.fontUi
                            font.pixelSize: 11
                        }
                    }
                    VIcon {
                        visible: oviora.selectedAudioTrack === modelData.index
                        name: "check"
                        color: Theme.accent
                    }
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: oviora.selectedAudioTrack = modelData.index }
            }
        }
    }
    DialogSection { text: qsTr("输出") }
    DialogGroup {
        VRow {
            label: qsTr("音量")
            value: Math.round(oviora.volume * 100) + "%"
            VSlider {
                implicitWidth: 170
                from: 0; to: 1; value: oviora.volume; inputScale: 100
                onMoved: oviora.volume = value
            }
        }
        VRow {
            label: qsTr("静音")
            VSwitch {
                checked: oviora.muted
                onToggled: checked => oviora.muted = checked
            }
        }
    }
    DialogSection { text: qsTr("音画同步") }
    DialogGroup {
        VRow {
            label: qsTr("声音同步")
            hint: oviora.audioSyncMode === 0 ? (oviora.audioSyncLive ? qsTr("软件估算补偿 ") + oviora.audioCompensationMs.toFixed(0) + " ms" : qsTr("自动估算 · 采集卡 / PS5 实时输入时生效"))
                  : oviora.audioSyncMode === 1 ? qsTr("手动偏移 · 实时输入时生效") : qsTr("关闭补偿")
            VSeg {
                options: [{ id: "0", label: qsTr("自动估算") }, { id: "1", label: qsTr("手动") }, { id: "2", label: qsTr("关闭", "off") }]
                current: String(oviora.audioSyncMode)
                onPicked: id => oviora.audioSyncMode = Number(id)
            }
        }
        VRow {
            label: qsTr("手动偏移")
            hint: qsTr("负值 = 声音提前")
            value: oviora.audioOffsetMs + " ms"
            VSlider {
                implicitWidth: 170
                center: true
                from: -250; to: 250; value: oviora.audioOffsetMs; enabledControl: oviora.audioSyncMode === 1
                onMoved: oviora.audioOffsetMs = Math.round(value / 10) * 10
            }
        }
        VRow {
            label: qsTr("当前偏差")
            hint: qsTr("软件内测得的音画偏差，不是扬声器实测；只在采集卡 / PS5 实时输入时有")
            value: oviora.audioSkewKnown ? (oviora.audioSkewMs >= 0 ? "+" : "") + oviora.audioSkewMs.toFixed(0) + " ms" : qsTr("未测")
        }
    }
    DialogSection { text: qsTr("输出设备") }
    DialogGroup {
        VRow {
            label: qsTr("输出设备")
            hint: oviora.audioOutputStatus
            VSelect {
                objectName: "audio-device"
                implicitWidth: 240
                readonly property string chosen: oviora.preferences.audioDevice || ""
                value: { const d = oviora.audioDevices.find(x => x.id === chosen); return d ? d.label : qsTr("跟随系统默认") }
                options: oviora.audioDevices
                onPicked: id => oviora.setPreference("audioDevice", id)
            }
        }
        VRow {
            label: qsTr("立体声下混")
            hint: qsTr("多声道片源混成双声道再输出（耳机）；关闭时按设备声道布局映射")
            VSwitch {
                objectName: "audio-stereo"
                checked: oviora.preferences.audioForceStereo === true
                onToggled: checked => oviora.setPreference("audioForceStereo", checked)
            }
        }
    }
    // The list is read when the dialog opens: devices come and go.
    Connections {
        target: dialogHost
        function onDialogChanged() { if (dialogHost.dialog === "audio") oviora.refreshAudioDevices() }
    }
}
