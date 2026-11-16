// Professional-view audioCol controls; settings remain in the shared bridge.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import QtQuick.Effects

ColumnLayout {
    id: audioCol
    Layout.fillWidth: true
    spacing: 6
    VGroup {
        VRow {
            label: qsTr("音量")
            value: Math.round(oviora.volume * 100) + "%"
            VSlider {
                implicitWidth: 150
                from: 0; to: 1; value: oviora.volume; inputScale: 100
                onMoved: value => oviora.volume = value
            }
        }
        VRow {
            label: qsTr("静音")
            VSwitch {
                checked: oviora.muted
                onToggled: checked => oviora.muted = checked
            }
        }
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
            label: qsTr("音频偏移")
            hint: qsTr("正值延后音频")
            value: oviora.audioOffsetMs + " ms"
            VSlider {
                implicitWidth: 150
                center: true
                from: -250; to: 250; value: oviora.audioOffsetMs; enabledControl: oviora.audioSyncMode === 1
                onMoved: value => oviora.audioOffsetMs = Math.round(value)
            }
        }
    }
    VEyebrow { text: qsTr("音轨") }
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
            implicitHeight: 44
            radius: 12
            color: Theme.card2
            border.width: 1
            border.color: oviora.selectedAudioTrack === modelData.index ? Theme.accent : Theme.stroke
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 12
                VEqBars {
                    active: oviora.selectedAudioTrack === modelData.index && oviora.running && !oviora.paused
                    opacity: oviora.selectedAudioTrack === modelData.index ? 1 : 0.6
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
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: oviora.selectedAudioTrack = modelData.index }
        }
    }
}
