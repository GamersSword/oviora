import QtQuick

// The PS5 picture lying flat: the console body in the text colour and its light strip in the
// state colour: blue when on/free, orange when in rest mode or busy, grey when unknown
// (tone: Tema.ok, Tema.warm, anything else). Drawn on the picture's own 135.5 x 36 grid.
// Oviora (new): a Canvas, the software scene graph of the new app cannot draw Shapes. Drawn symmetric
// (top half mirrored) with the side cover seam, as on the website.
Item {
    id: glyph
    property color tone: Tema.muted
    readonly property color strip: Qt.colorEqual(tone, Tema.ok) ? "#00A7FF"
        : (Qt.colorEqual(tone, Tema.warm) ? "#FFAE2F" : Tema.soft(Tema.muted, 0.7))
    implicitWidth: 64
    implicitHeight: 18

    onToneChanged: canvas.requestPaint()
    Canvas {
        id: canvas
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const k = Math.min(width / 135.47, height / 35.98);
            ctx.translate((width - 135.47 * k) / 2, (height - 35.98 * k) / 2);
            ctx.scale(k, k);
            // Symmetric: the top half is drawn, then the same half mirrored onto the bottom
            for (let side = 0; side < 2; ++side) {
                ctx.save();
                if (side === 1) {
                    ctx.translate(0, 35.98);
                    ctx.scale(1, -1);
                }
                ctx.beginPath();
                ctx.rect(-2, -2, 140, 17.99 + 2.6);  // a little past the middle: no hairline where the halves meet
                ctx.clip();
                ctx.fillStyle = glyph.strip;
                ctx.path = "M1.909,11.763V8.79L127.635,1.807V29.541L1.909,22.902v-3.158l6.691,-3.645z";
                ctx.fill();
                ctx.fillStyle = Tema.text;
                ctx.path = "M135.059,0.004C105.38,2.679 69.761,6.719 7.637,2.238 2.368,1.858 -2.01,10.53 0.965,10.491 65.38,9.662 117.287,1.775 124.96,2.935c1.336,0.202 1.574,0.634 2.277,3.271 0.014,0.056 0.023,0.078 0.037,0.13C127.222,6.335 127.18,6.334 127.127,6.333 111.134,6.269 42.596,12.28 1.908,11.764V19.744C42.727,19.226 111.141,25.276 127.21,25.172c-0.021,0.076 -0.034,0.111 -0.056,0.195 -0.703,2.637 -0.941,3.069 -2.278,3.271C117.203,29.799 65.81,22.017 1.395,21.187 0.89,21.181 0.027,21.658 0.07,22.395 0.3,26.365 0.437,35.819 1.119,35.819c9.352,0.013 30.52,0.601 40.773,-0.532 18.706,-2.068 32.929,-4.728 44.502,-5.39 21.532,-1.232 33.907,0.053 48.583,1.672 0.933,0.103 0.031,-1.246 -0.448,-1.241 -1.987,0.023 -4.222,-2.731 -6.798,-4.992 -0.019,-0.016 -0.028,-0.082 -0.032,-0.174 1.321,-0.022 2.268,-0.087 2.695,-0.217V16.046,15.462 6.562c-0.424,-0.129 -1.327,-0.197 -2.607,-0.219 0.006,-0.053 0.013,-0.096 0.027,-0.107 2.576,-2.261 4.811,-5.015 6.797,-4.992 0.479,0.005 1.384,-1.325 0.448,-1.241z";
                ctx.fill();
                // The side cover's seam, cut out of the picture
                ctx.globalCompositeOperation = "destination-out";
                ctx.lineWidth = 1.9;
                ctx.lineCap = "round";
                ctx.beginPath();
                ctx.moveTo(91, -2);
                ctx.lineTo(83.5, 6);
                ctx.stroke();
                ctx.restore();
            }
            // The middle band once more from the picture itself: no hairline where the halves meet
            ctx.save();
            ctx.beginPath();
            ctx.rect(-2, 16.99, 140, 2);
            ctx.clip();
            ctx.fillStyle = Tema.text;
            ctx.path = "M135.059,0.004C105.38,2.679 69.761,6.719 7.637,2.238 2.368,1.858 -2.01,10.53 0.965,10.491 65.38,9.662 117.287,1.775 124.96,2.935c1.336,0.202 1.574,0.634 2.277,3.271 0.014,0.056 0.023,0.078 0.037,0.13C127.222,6.335 127.18,6.334 127.127,6.333 111.134,6.269 42.596,12.28 1.908,11.764V19.744C42.727,19.226 111.141,25.276 127.21,25.172c-0.021,0.076 -0.034,0.111 -0.056,0.195 -0.703,2.637 -0.941,3.069 -2.278,3.271C117.203,29.799 65.81,22.017 1.395,21.187 0.89,21.181 0.027,21.658 0.07,22.395 0.3,26.365 0.437,35.819 1.119,35.819c9.352,0.013 30.52,0.601 40.773,-0.532 18.706,-2.068 32.929,-4.728 44.502,-5.39 21.532,-1.232 33.907,0.053 48.583,1.672 0.933,0.103 0.031,-1.246 -0.448,-1.241 -1.987,0.023 -4.222,-2.731 -6.798,-4.992 -0.019,-0.016 -0.028,-0.082 -0.032,-0.174 1.321,-0.022 2.268,-0.087 2.695,-0.217V16.046,15.462 6.562c-0.424,-0.129 -1.327,-0.197 -2.607,-0.219 0.006,-0.053 0.013,-0.096 0.027,-0.107 2.576,-2.261 4.811,-5.015 6.797,-4.992 0.479,0.005 1.384,-1.325 0.448,-1.241z";
            ctx.fill();
            ctx.restore();
        }
    }
}
