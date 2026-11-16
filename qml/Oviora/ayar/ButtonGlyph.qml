import QtQuick
import QtQuick.Controls
import Oviora

// A PlayStation controller button, drawn. kind:
//   cross circle square triangle | dpad-up dpad-down dpad-left dpad-right
//   l1 l2 r1 r2 | ls-up ls-down ls-left ls-right l3 | rs-up rs-down rs-left rs-right r3
//   options create touchpad ps
Item {
    id: glyph
    property string kind: "cross"
    readonly property string group: {
        if (["cross", "circle", "square", "triangle"].indexOf(kind) >= 0) return "face";
        if (kind.startsWith("dpad")) return "dpad";
        if (["l1", "l2", "r1", "r2"].indexOf(kind) >= 0) return "shoulder";
        if (kind.startsWith("ls") || kind.startsWith("rs") || kind === "l3" || kind === "r3") return "stick";
        return "system";
    }
    implicitWidth: 40
    implicitHeight: 40

    Rectangle {
        id: plate
        anchors.centerIn: parent
        width: glyph.group === "shoulder" ? 40 : (glyph.kind === "touchpad" ? 40 : 36)
        height: glyph.group === "shoulder" ? 26 : (glyph.kind === "touchpad" ? 26 : 36)
        radius: glyph.group === "face" || glyph.group === "stick" ? width / 2 : (glyph.group === "shoulder" ? (glyph.kind === "l2" || glyph.kind === "r2" ? 8 : 13) : 9)
        color: Tema.surface
        border.width: 1
        border.color: Tema.lineStrong
    }

    Label {
        textFormat: Text.PlainText
        anchors.centerIn: parent
        visible: glyph.group === "shoulder" || glyph.kind === "l3" || glyph.kind === "r3" || glyph.kind === "ps"
        text: glyph.kind === "ps" ? "PS" : glyph.kind.toUpperCase()
        font.family: Tema.display
        font.pixelSize: glyph.kind === "ps" ? 13 : 13
        font.weight: Font.Bold
        color: Tema.text
    }

    Canvas {
        id: canvas
        anchors.centerIn: parent
        width: 36
        height: 36
        visible: !(glyph.group === "shoulder" || glyph.kind === "l3" || glyph.kind === "r3" || glyph.kind === "ps")
        contextType: "2d"
        onVisibleChanged: requestPaint()
        Connections {
            target: glyph
            function onKindChanged() { canvas.requestPaint(); }
        }
        function arrow(ctx, dir, cx, cy, s) {
            ctx.beginPath();
            switch (dir) {
            case "up": ctx.moveTo(cx, cy - s); ctx.lineTo(cx + s, cy + s * 0.6); ctx.lineTo(cx - s, cy + s * 0.6); break;
            case "down": ctx.moveTo(cx, cy + s); ctx.lineTo(cx + s, cy - s * 0.6); ctx.lineTo(cx - s, cy - s * 0.6); break;
            case "left": ctx.moveTo(cx - s, cy); ctx.lineTo(cx + s * 0.6, cy - s); ctx.lineTo(cx + s * 0.6, cy + s); break;
            case "right": ctx.moveTo(cx + s, cy); ctx.lineTo(cx - s * 0.6, cy - s); ctx.lineTo(cx - s * 0.6, cy + s); break;
            }
            ctx.closePath();
            ctx.fill();
        }
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const c = 18;
            ctx.lineWidth = 2.2;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            switch (glyph.kind) {
            case "cross":
                ctx.strokeStyle = "#8FB3FF";
                ctx.beginPath();
                ctx.moveTo(c - 7, c - 7); ctx.lineTo(c + 7, c + 7);
                ctx.moveTo(c + 7, c - 7); ctx.lineTo(c - 7, c + 7);
                ctx.stroke();
                break;
            case "circle":
                ctx.strokeStyle = "#FF7F8C";
                ctx.beginPath();
                ctx.arc(c, c, 7.5, 0, Math.PI * 2);
                ctx.stroke();
                break;
            case "square":
                ctx.strokeStyle = "#EC97D6";
                ctx.strokeRect(c - 7, c - 7, 14, 14);
                break;
            case "triangle":
                ctx.strokeStyle = "#4CD7B4";
                ctx.beginPath();
                ctx.moveTo(c, c - 8); ctx.lineTo(c + 8, c + 6); ctx.lineTo(c - 8, c + 6);
                ctx.closePath();
                ctx.stroke();
                break;
            case "options":
                ctx.strokeStyle = "#F1EEF6";
                ctx.beginPath();
                for (let i = -1; i <= 1; i++) {
                    ctx.moveTo(c - 7, c + i * 5); ctx.lineTo(c + 7, c + i * 5);
                }
                ctx.stroke();
                break;
            case "create":
                ctx.strokeStyle = "#F1EEF6";
                ctx.beginPath();
                ctx.moveTo(c, c + 7); ctx.lineTo(c, c - 7);
                ctx.moveTo(c - 5, c - 2); ctx.lineTo(c, c - 7); ctx.lineTo(c + 5, c - 2);
                ctx.stroke();
                break;
            case "touchpad":
                ctx.strokeStyle = "#F1EEF6";
                ctx.lineWidth = 1.6;
                ctx.beginPath();
                ctx.moveTo(c - 10, c - 4); ctx.lineTo(c + 10, c - 4);
                ctx.stroke();
                ctx.fillStyle = "#F1EEF6";
                ctx.beginPath();
                ctx.arc(c + 2, c + 3, 2.5, 0, Math.PI * 2);
                ctx.fill();
                break;
            default: {
                const dir = glyph.kind.split("-")[1];
                ctx.fillStyle = "#F1EEF6";
                if (glyph.group === "dpad") {
                    // the cross of the d-pad, the pressed arm lit
                    ctx.fillStyle = "#36323F";
                    ctx.fillRect(c - 4, c - 11, 8, 22);
                    ctx.fillRect(c - 11, c - 4, 22, 8);
                    ctx.fillStyle = "#7B5CFF";
                    switch (dir) {
                    case "up": ctx.fillRect(c - 4, c - 11, 8, 8); break;
                    case "down": ctx.fillRect(c - 4, c + 3, 8, 8); break;
                    case "left": ctx.fillRect(c - 11, c - 4, 8, 8); break;
                    case "right": ctx.fillRect(c + 3, c - 4, 8, 8); break;
                    }
                } else {
                    // stick: the letter and a small arrow towards the push
                    ctx.font = "bold 12px 'Figtree'";
                    ctx.textAlign = "center";
                    ctx.textBaseline = "middle";
                    ctx.fillStyle = "#8E8899";
                    ctx.fillText(glyph.kind.startsWith("ls") ? "L" : "R", c, c + 1);
                    ctx.fillStyle = "#7B5CFF";
                    const o = 11;
                    switch (dir) {
                    case "up": arrow(ctx, "up", c, c - o, 3.6); break;
                    case "down": arrow(ctx, "down", c, c + o, 3.6); break;
                    case "left": arrow(ctx, "left", c - o, c, 3.6); break;
                    case "right": arrow(ctx, "right", c + o, c, 3.6); break;
                    }
                }
            }
            }
        }
    }
}
