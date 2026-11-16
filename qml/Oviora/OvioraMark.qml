import QtQuick
import QtQuick.Shapes

// The Oviora mark drawn live (tasarim/logo/oviora-mark.svg): a purple ring open at the top right and
// a short amber arc in the gap. With `travelling` the amber arc goes once around the ring every
// 2.4 s, slowing as it leaves and comes back to the gap; that is the only waiting animation.
Item {
    id: mark
    property bool travelling: false
    // stroke 7 of 64 (9 for small sizes, as oviora-mark-small.svg)
    property real stroke: width <= 24 ? 9 : 7
    property color ringColor: Tema.accent
    property color arcColor: Tema.warm
    implicitWidth: 64
    implicitHeight: 64

    // Degrees the amber arc has travelled from its place in the gap
    property real turn: 0
    NumberAnimation on turn {
        running: mark.travelling && mark.visible
        from: 0
        to: 360
        duration: 2400
        loops: Animation.Infinite
        easing.type: Easing.InOutSine
        onRunningChanged: if (!running) mark.turn = 0
    }

    readonly property real k: Math.min(width, height) / 64

    Shape {
        anchors.centerIn: parent
        width: 64 * mark.k
        height: 64 * mark.k
        preferredRendererType: Shape.CurveRenderer

        // Purple: from the top, counter-clockwise three quarters round to the right (-90 -> -360)
        ShapePath {
            strokeColor: mark.ringColor
            strokeWidth: mark.stroke * mark.k
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: 32 * mark.k
                centerY: 32 * mark.k
                radiusX: 21 * mark.k
                radiusY: 21 * mark.k
                startAngle: -90
                sweepAngle: -270
            }
        }
        // Amber: the short arc in the gap (-65.2 -> -24.8), moved round by `turn`
        ShapePath {
            strokeColor: mark.arcColor
            strokeWidth: mark.stroke * mark.k
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: 32 * mark.k
                centerY: 32 * mark.k
                radiusX: 21 * mark.k
                radiusY: 21 * mark.k
                startAngle: -65.2 + mark.turn
                sweepAngle: 40.4
            }
        }
    }
}
