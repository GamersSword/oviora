// Connecting screen over the stream page: matches the old StreamView loadingView.
// Shows OvioraAnimasyon with stage-specific Turkish status/detail, controller menu
// shortcut hint at the bottom, and double-press Esc / right-click / Circle-B (or Ctrl+Q) cancel.
import QtQuick

Rectangle {
    id: connecting
    color: Tema.bg

    property bool onMinPage: false
    readonly property var conn: oviora.connection || ({})
    readonly property string stage: conn.stage || ""
    readonly property bool active: stage === "opening" || stage.indexOf("psn-") === 0 || stage === "retrying" || stage === "waking" || stage === "stopping"

    property bool allowCancel: false
    property bool cancelArmed: false
    property bool cancelling: false
    property int lastPad: 0

    opacity: active ? 1 : 0
    visible: onMinPage && opacity > 0
    readonly property bool videoCover: visible

    Behavior on opacity { NumberAnimation { duration: 250 } }

    onActiveChanged: {
        if (active) {
            allowCancel = false
            cancelArmed = false
            cancelling = false
            cancelArmTimer.stop()
            cancelEnableTimer.restart()
        } else {
            cancelEnableTimer.stop()
            cancelArmTimer.stop()
            allowCancel = false
            cancelArmed = false
        }
    }

    Timer {
        id: cancelEnableTimer
        interval: 1500
        onTriggered: connecting.allowCancel = true
    }
    Timer {
        id: cancelArmTimer
        interval: 3000
        onTriggered: connecting.cancelArmed = false
    }

    function tryCancel() {
        if (!visible || cancelling || !allowCancel) return
        if (!cancelArmed) {
            cancelArmed = true
            cancelArmTimer.restart()
            return
        }
        doCancel()
    }
    function doCancel() {
        if (!visible || cancelling) return
        cancelling = true
        cancelArmed = false
        cancelArmTimer.stop()
        oviora.logUi("ui-connect", "cancelled on the connecting screen at stage " + stage)
        oviora.ps5Cancel()
    }

    Shortcut {
        sequence: "Esc"
        enabled: connecting.visible && !connecting.cancelling
        onActivated: connecting.tryCancel()
        // Main.qml has its own Esc (full screen); both enabled made the key ambiguous and nothing ran
        onActivatedAmbiguously: connecting.tryCancel()
    }
    Shortcut {
        sequence: "Ctrl+Q"
        enabled: connecting.visible && !connecting.cancelling
        onActivated: connecting.doCancel()
    }
    Connections {
        target: oviora
        function onPadPressedChanged() {
            const p = oviora.padPressed || 0
            const circleNow = (p & 2) !== 0
            const circleWas = (connecting.lastPad & 2) !== 0
            connecting.lastPad = p
            if (connecting.visible && circleNow && !circleWas)
                connecting.tryCancel()
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: connecting.visible
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton)
                connecting.tryCancel()
        }
    }

    OvioraAnimasyon {
        anchors.fill: parent
        progress: -1
        status: {
            if (connecting.cancelling || connecting.stage === "stopping") return qsTr("Bağlantı kapatılıyor…")
            if (connecting.stage === "psn-reach") return qsTr("PS5'e ulaşılıyor…")
            if (connecting.stage === "psn-punch" || connecting.stage === "psn-data")
                return qsTr("PS5'e ulaşıldı, görüntü bağlantısı kuruluyor…")
            if (connecting.stage === "retrying") return qsTr("Bir kez daha bağlanılıyor…")
            if (connecting.stage === "waking") return qsTr("PS5 uyandırılıyor…")
            const c = connecting.conn.console || ""
            return c.length > 0 ? qsTr("%1 ile bağlantı kuruluyor…").arg(c) : qsTr("PS5'e bağlanılıyor…")
        }
        detail: {
            if (connecting.cancelArmed) return qsTr("Bağlantıyı iptal etmek için bir kez daha bas.")
            let lines = []
            const d = connecting.conn.detail || ""
            if (connecting.stage === "retrying" && d.length > 0) lines.push(d)
            if (connecting.stage === "waking") lines.push(qsTr("Konsol açılınca kendiliğinden bağlanılacak."))
            if (connecting.stage === "stopping") lines.push(qsTr("PS5 oturumu bırakana kadar birkaç saniye sürebilir."))
            if (connecting.conn.slow === true)
                lines.push(qsTr("İnternet üzerinden bağlantı birkaç dakika sürebilir, beklemen yeterli."))
            return lines.join("\n")
        }
    }

    readonly property var comboButtons: [qsTr("Kullanılmıyor"), qsTr("Çarpı"), qsTr("Daire"), qsTr("Kare"), qsTr("Üçgen"),
        qsTr("Yön sol"), qsTr("Yön sağ"), qsTr("Yön yukarı"), qsTr("Yön aşağı"), "L1", "R1", "L3", "R3",
        "Options", "Create", qsTr("Dokunmatik yüzey"), qsTr("PS düğmesi")]

    readonly property string padComboText: {
        const prefs = oviora.preferences || ({})
        if (prefs.padMenuCombo === false) return ""
        const btns = prefs.padMenuButtons !== undefined ? prefs.padMenuButtons : [9, 10, 11, 12]
        let names = []
        for (let i = 0; i < btns.length; ++i) {
            const idx = btns[i] || 0
            if (idx > 0 && idx < comboButtons.length)
                names.push(comboButtons[idx])
        }
        return names.join(" + ")
    }

    Text {
        textFormat: Text.PlainText
        anchors {
            horizontalCenter: parent.horizontalCenter
            bottom: parent.bottom
            bottomMargin: 36
        }
        width: Math.min(parent.width - 64, 760)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        lineHeight: 1.35
        font.family: Tema.body
        font.pixelSize: 14
        color: Tema.faint
        text: (oviora.ps5 && oviora.ps5.controller && connecting.padComboText.length > 0)
            ? qsTr("Yayın menüsünü klavyede Ctrl+O, kolda %1 ile açabilirsin.").arg(connecting.padComboText)
            : qsTr("Yayın menüsünü Ctrl+O ile açabilirsin.")
    }
}
