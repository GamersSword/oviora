pragma Singleton
import QtQuick

// Oviora's look in one place (tasarim/OVIORA_TASARIM.md, "Mürekkep moru"). Every screen takes its
// colours, faces and sizes from here; nothing else should name a hex value or a font.
QtObject {
    // ── Colours ──
    readonly property color bg: "#0E0D12"          // window / page
    readonly property color surface: "#17151E"     // panels, menus, dialogs
    readonly property color surface2: "#1F1C28"    // hover, pressed, inputs
    readonly property color line: "#26232F"        // separators, borders
    readonly property color lineStrong: "#36323F"  // borders that must show on a surface, scroll thumbs
    readonly property color text: "#F1EEF6"
    readonly property color muted: "#8E8899"
    readonly property color faint: "#5F5A69"       // disabled text, unknown state
    readonly property color accent: "#7B5CFF"      // the one brand colour
    readonly property color accentHover: "#8C71FF"
    readonly property color accentDown: "#6A4AF0"
    readonly property color accentText: "#A996FF"  // accent as small text on the dark ground
    readonly property color inkOnAccent: "#FFFFFF"
    readonly property color accentSoft: Qt.rgba(0x7B / 255, 0x5C / 255, 1, 0.16)
    readonly property color warm: "#FFB86B"        // logo arc, busy / rest mode
    readonly property color ok: "#6FDDA4"          // ready / free
    readonly property color danger: "#FF6B6B"
    readonly property color scrim: Qt.rgba(0x0E / 255, 0x0D / 255, 0x12 / 255, 0.78)

    function soft(c, a) { return Qt.rgba(c.r, c.g, c.b, a); }

    // ── Type ──
    property FontLoader displayFont: FontLoader { source: "fonts/BricolageGrotesque.ttf" }
    property FontLoader bodyFont: FontLoader { source: "fonts/Figtree.ttf" }
    readonly property string display: "Bricolage Grotesque"   // headings, the word mark
    readonly property string body: "Figtree"                  // everything else

    readonly property int titleSize: 38       // page title / greeting
    readonly property int sectionSize: 24     // section title
    readonly property int bodySize: 16
    readonly property int smallSize: 14
    readonly property int labelSize: 14       // quiet section labels (sentence case, muted)

    // ── Shape ──
    readonly property int radiusPanel: 14
    readonly property int radiusInput: 10

    // ── Motion ──
    readonly property int fade: 160
}
