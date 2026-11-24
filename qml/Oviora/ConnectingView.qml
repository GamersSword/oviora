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

    // A picture of the game behind the scene as soon as the decoder has one: on PSN from "PS5'e
    // ulaşıldı" on (the decoder runs some seconds before the picture pipeline is ready), on the home
    // network while opening. Blurred and dimmed; the scene carries on over it, and once connected the
    // view opens onto the sharp, live picture.
    readonly property bool wantPicture: (active && onMinPage && (stage === "opening" || stage === "psn-punch" || stage === "psn-data" || stage === "retrying")) || holding
    onWantPictureChanged: {
        oviora.requestConnectingPicture(wantPicture)
        if (wantPicture) pictureClear.stop()
    }
    // Bağlan pressed: the stream page shows at once, before the bridge reports the first stage; the
    // scene covers it from the first frame instead of the page's black
    property bool starting: false
    onOnMinPageChanged: if (onMinPage && !oviora.hasSource && stage === "") { starting = true; startingMax.restart() }
    Timer { id: startingMax; interval: 3000; onTriggered: connecting.starting = false }
    // Connected: the scene's words and mark step back over the picture (the view stays opaque: the
    // game is a native window under a hole the view cuts, so a fading view showed black), then the
    // view goes at once and the sharp picture is there
    property bool leaving: false
    Timer { id: leaveDone; interval: Theme.d(300); onTriggered: connecting.leaving = false }
    // Once connected the scene stays a while: the game's picture behind it for at least pictureMs in
    // all (over PSN it came during the last steps), "bağlantı kuruldu" over it, then the view opens
    // onto the game. 7 s over PSN, 3 s on the home network.
    readonly property int pictureMs: sawPsn ? 7000 : 3000
    property bool sawPsn: false
    property real pictureSince: 0
    property bool holding: false
    Timer {
        id: holdDone
        onTriggered: { connecting.holding = false; connecting.leaving = true; leaveDone.restart() }
    }
    function startLeaving() {
        const shownFor = pictureSince > 0 ? Date.now() - pictureSince : 0
        const rest = !Theme.reduced ? pictureMs - shownFor : 0
        if (rest > 200) {
            holding = true
            holdDone.interval = (pictureSince > 0 ? rest : pictureMs)
            holdDone.restart()
        } else {
            leaving = true
            leaveDone.restart()
        }
    }
    readonly property bool held: active || starting || leaving || holding
    // Gone from the screen: the picture goes too (unless the stream menu took it over)
    onVisibleChanged: if (!visible) pictureClear.restart()
    Timer {
        id: pictureClear
        interval: 50
        onTriggered: if (!connecting.visible && !(typeof root !== "undefined" && root.streamMenuShown)) oviora.clearMenuBackdrop()
    }

    visible: onMinPage && held
    readonly property bool videoCover: visible

    // How far along: PSN goes through its steps; on the home network (or while stopping) the
    // hairline only runs. A retry keeps where it got to.
    property real stageProgress: -2
    onStageChanged: {
        if (stage !== "") starting = false
        if (stage.indexOf("psn-") === 0) sawPsn = true
        // A new connection, or the held one ended (cancelled, failed): nothing is held any more
        if (stage === "" || (holding && stage !== "live")) { holding = false; holdDone.stop() }
        if (stage === "" ) { sawPsn = false; pictureSince = 0 }
        const steps = { "waking": 0.12, "psn-reach": 0.3, "psn-punch": 0.58, "psn-data": 0.8 }
        if (steps[stage] !== undefined) stageProgress = steps[stage]
        else if (stage === "opening") stageProgress = stageProgress >= 0.5 ? 0.92 : -2
        else if (stage === "stopping") stageProgress = -2
        else if (!active) stageProgress = -2
    }

    onActiveChanged: {
        if (active) starting = false
        // Connected (or failed / PIN): the words step back over the picture, then the view goes
        else if (visible && stage === "live") startLeaving()
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

    // The picture, small and stretched (blurred), under a dim that keeps the scene readable
    Image {
        id: gamePicture
        anchors.fill: parent
        source: connecting.visible ? (oviora.menuBackdrop || "") : ""
        fillMode: Image.PreserveAspectCrop
        smooth: true
        cache: false
        asynchronous: false
        opacity: status === Image.Ready && connecting.visible ? 1 : 0
        onStatusChanged: if (status === Image.Ready && connecting.pictureSince === 0) connecting.pictureSince = Date.now()
        Behavior on opacity { NumberAnimation { duration: Theme.d(700); easing.type: Easing.OutCubic } }
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(Tema.bg.r, Tema.bg.g, Tema.bg.b, 0.55)
        }
    }

    OvioraAnimasyon {
        anchors.fill: parent
        opacity: connecting.leaving ? 0 : 1
        scale: connecting.leaving ? 1.04 : 1
        Behavior on opacity { NumberAnimation { duration: Theme.d(260); easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: Theme.d(300); easing.type: Easing.OutCubic } }
        progress: connecting.cancelling ? -2 : connecting.holding || connecting.leaving ? 1 : connecting.stageProgress
        status: {
            if (connecting.holding || connecting.leaving) {
                const name = connecting.conn.console || ""
                return name.length > 0 ? qsTr("%1 ile bağlantı kuruldu").arg(name) : qsTr("PS5 ile bağlantı kuruldu")
            }
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
        opacity: connecting.leaving ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: Theme.d(200) } }
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
