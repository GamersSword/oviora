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

    Column {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -20 * scene.scale_
        spacing: 0

        OvioraMark {
            anchors.horizontalCenter: parent.horizontalCenter
            width: (scene.showTitle ? 104 : 88) * scene.scale_
            height: width
            travelling: scene.visible && !Theme.reduced
        }

        Text {
            textFormat: Text.PlainText
            visible: scene.showTitle
            anchors.horizontalCenter: parent.horizontalCenter
            topPadding: 18 * scene.scale_
            text: "Oviora"
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
            text: scene.status
            font.family: scene.showTitle ? Tema.body : Tema.display
            font.pixelSize: (scene.showTitle ? 17 : 26) * scene.scale_
            font.weight: scene.showTitle ? Font.Normal : Font.DemiBold
            color: scene.showTitle ? Tema.muted : Tema.text
        }

        // Known amount only: a hairline that fills
        Item {
            visible: scene.progress >= 0
            anchors.horizontalCenter: parent.horizontalCenter
            width: 280 * scene.scale_
            height: 26 * scene.scale_
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 3
                radius: 1.5
                color: Tema.line
                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, scene.progress))
                    height: parent.height
                    radius: 1.5
                    color: Tema.accent
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
            text: scene.detail
            font.family: Tema.body
            font.pixelSize: 15 * scene.scale_
            color: Tema.muted
        }
    }
}
