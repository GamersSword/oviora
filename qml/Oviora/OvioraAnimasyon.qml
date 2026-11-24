import QtQuick

// The waiting scene (opening, connecting to a PS5, PSN connecting): the Oviora mark whose amber arc
// travels round the ring, what is happening in one line, and an optional quieter detail under it.
// Nothing else moves.
Item {
    id: scene

    property string status: ""
    property string detail: ""
    // 0..1 for a known amount (a thin line under the status), below 0 when unknown
    property real progress: -1
    // The word mark under the ring (the opening scene)
    property bool showTitle: false
    // kept for callers of the old scene; there is nothing at the bottom any more
    property bool showSparks: false
    property real scale_: Math.max(0.75, Math.min(width / 1100, height / 760, 1.25))

    // Coming in: the mark grows out of the dark, the lines rise after it (again each time it is shown)
    property real enterT: 1
    function playEnter() {
        if (Theme.reduced) { enterT = 1; return }
        enterT = 0
        enterAnim.restart()
    }
    NumberAnimation { id: enterAnim; target: scene; property: "enterT"; to: 1; duration: 900; easing.bezierCurve: Theme.springSoft }
    onVisibleChanged: if (visible) playEnter()
    Component.onCompleted: if (visible) playEnter()

    // The lines on screen: a new text fades the old one up and out, then rises in itself
    property string shownStatus: status
    property string shownDetail: detail
    property real lineT: 1
    onStatusChanged: swapLines()
    onDetailChanged: swapLines()
    function swapLines() {
        if (!visible || Theme.reduced) { shownStatus = status; shownDetail = detail; return }
        if (lineSwap.running) return  // the running swap takes the newest text when it switches
        lineSwap.restart()
    }
    SequentialAnimation {
        id: lineSwap
        NumberAnimation { target: scene; property: "lineT"; to: 0; duration: 140; easing.type: Easing.InCubic }
        ScriptAction { script: { scene.shownStatus = scene.status; scene.shownDetail = scene.detail } }
        NumberAnimation { target: scene; property: "lineT"; to: 1; duration: 320; easing.bezierCurve: Theme.springSoft }
        // Changed again while it was coming in: once more with the newest
        onFinished: if (scene.shownStatus !== scene.status || scene.shownDetail !== scene.detail) scene.swapLines()
    }

    Column {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -20 * scene.scale_
        spacing: 0

        OvioraMark {
            anchors.horizontalCenter: parent.horizontalCenter
            width: (scene.showTitle ? 104 : 88) * scene.scale_
            height: width
            travelling: scene.visible && !Theme.reduced
            opacity: Math.min(1, scene.enterT * 1.6)
            scale: 0.82 + 0.18 * scene.enterT
        }

        Text {
            textFormat: Text.PlainText
            visible: scene.showTitle
            anchors.horizontalCenter: parent.horizontalCenter
            topPadding: 18 * scene.scale_
            text: "Oviora"
            opacity: Math.max(0, Math.min(1, scene.enterT * 1.4 - 0.25))
            transform: Translate { y: (1 - scene.enterT) * 14 * scene.scale_ }
            font.family: Tema.display
            font.pixelSize: 40 * scene.scale_
            font.weight: Font.DemiBold
            font.letterSpacing: -0.8 * scene.scale_
            color: Tema.text
        }

        Text {
            textFormat: Text.PlainText
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(scene.width - 64, 720 * scene.scale_)
            topPadding: (scene.showTitle ? 10 : 28) * scene.scale_
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: scene.shownStatus
            opacity: Math.max(0, Math.min(1, scene.enterT * 1.4 - 0.35)) * scene.lineT
            transform: Translate { y: (1 - scene.enterT) * 18 * scene.scale_ + (1 - scene.lineT) * 8 }
            font.family: scene.showTitle ? Tema.body : Tema.display
            font.pixelSize: (scene.showTitle ? 17 : 26) * scene.scale_
            font.weight: scene.showTitle ? Font.Normal : Font.DemiBold
            color: scene.showTitle ? Tema.muted : Tema.text
        }

        // A hairline: fills to a known amount (0..1, gliding), or runs a short segment to and fro (-2)
        Item {
            visible: scene.progress >= 0 || scene.progress === -2
            anchors.horizontalCenter: parent.horizontalCenter
            width: 280 * scene.scale_
            height: 26 * scene.scale_
            opacity: Math.max(0, Math.min(1, scene.enterT * 1.4 - 0.45))
            Rectangle {
                id: track
                anchors.bottom: parent.bottom
                width: parent.width
                height: 3
                radius: 1.5
                color: Tema.line
                clip: true
                Rectangle {
                    visible: scene.progress >= 0
                    width: track.width * Math.max(0, Math.min(1, scene.progress))
                    Behavior on width { NumberAnimation { duration: Theme.d(700); easing.bezierCurve: Theme.springSoft } }
                    height: parent.height
                    radius: 1.5
                    color: Tema.accent
                }
                Rectangle {
                    id: runner
                    visible: scene.progress === -2
                    width: track.width * 0.28
                    height: parent.height
                    radius: 1.5
                    color: Tema.accent
                    SequentialAnimation on x {
                        running: runner.visible && scene.visible && !Theme.reduced
                        loops: Animation.Infinite
                        NumberAnimation { from: -runner.width; to: track.width; duration: 1300; easing.type: Easing.InOutCubic }
                        PauseAnimation { duration: 120 }
                    }
                }
            }
        }

        Text {
            textFormat: Text.PlainText
            visible: text.length > 0
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(scene.width - 64, 600 * scene.scale_)
            topPadding: 12 * scene.scale_
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: 1.3
            text: scene.shownDetail
            opacity: Math.max(0, Math.min(1, scene.enterT * 1.4 - 0.5)) * scene.lineT
            transform: Translate { y: (1 - scene.enterT) * 22 * scene.scale_ + (1 - scene.lineT) * 6 }
            font.family: Tema.body
            font.pixelSize: 15 * scene.scale_
            color: Tema.muted
        }
    }
}
