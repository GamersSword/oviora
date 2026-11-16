// Professional-view reusable presentation component.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import QtQuick.Effects

ColumnLayout {
    Layout.fillWidth: true
    spacing: 0
    VRow {
        label: qsTr("显示同步")
        hint: qsTr("自动（推荐）：不撕裂，也不等待垂直同步，每次刷新显示最新一帧，延迟最低 · 垂直同步：每帧都等到刷新才显示，比自动多等最多一个刷新 · 允许撕裂：可能出现撕裂线")
        VSeg {
            objectName: "presentation-display-sync"
            options: [{ id: "0", label: qsTr("允许撕裂") }, { id: "1", label: qsTr("垂直同步") }, { id: "2", label: qsTr("自动") }]
            current: String(oviora.displaySync)
            onPicked: id => oviora.displaySync = Number(id)
        }
    }
    // 可变刷新 (G-SYNC / FreeSync): the switch greys when the monitor cannot
    // do it. The state is read on events only; 刷新显示器状态 in the 信号
    // card asks the driver again.
    VRow {
        label: qsTr("可变刷新")
        hint: oviora.vrrHint
        VSwitch {
            objectName: "presentation-vrr"
            enabled: oviora.vrrState >= 2
            opacity: enabled ? 1 : 0.5
            checked: oviora.variableRefresh
            onToggled: checked => oviora.variableRefresh = checked
        }
    }
    VRow {
        label: qsTr("输出上限")
        enabled: !oviora.presentationOwned
        hint: oviora.presentationStatus
        VSeg {
            objectName: "presentation-output-rate"
            options: [{ id: "0", label: qsTr("关闭", "off") }, { id: "1", label: qsTr("跟随显示器") }, { id: "2", label: qsTr("自定义") }]
            current: String(oviora.outputRateMode)
            onPicked: id => oviora.outputRateMode = Number(id)
        }
    }
    VRow {
        label: qsTr("自定义上限")
        enabled: !oviora.presentationOwned
        visible: oviora.outputRateMode === 2
        RowLayout {
            spacing: 6
            DialogHost.VTextField {
                objectName: "presentation-custom-fps"
                implicitWidth: 90
                text: oviora.outputCustomFps.toFixed(3)
                onEdited: text => { const v = Number(text); if (isFinite(v)) oviora.outputCustomFps = v }
            }
            Text { textFormat: Text.PlainText; text: "FPS"; color: Theme.t3; font.family: Theme.fontUi; font.pixelSize: 11 }
        }
    }
}
