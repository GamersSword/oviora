// Oviora's stream menu (Ctrl+O): a drawer the full height of the window on the right, as a game's pause
// menu. The game stays in view beside it; behind the drawer itself the frame of the moment it opened shows
// softly through (oviora.menuBackdrop). The list leads to its sections inside the same drawer: Mesajlar
// (friends, groups and their end-to-end encrypted conversations), Görüntü (the view and quality presets with
// every picture setting of the engine below them), Ses ve mikrofon, Altyazı, Klavye ve fare (with the keys,
// a wider drawer). Arrow keys / d-pad: up and down through the list, Enter opens, Esc goes back.
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "ayar" as S

Item {
    id: menu
    required property Window owner
    // The native video window leaves a hole under the panel (main.cpp syncVideoCovers)
    readonly property bool videoCover: visible
    readonly property real coverRadius: 0
    visible: false
    property bool shown: false
    // On the right edge, or the left one (Ayarlar in the menu: preferences.streamMenuSide)
    readonly property bool onLeft: oviora.preferences.streamMenuSide === "left"
    onOnLeftChanged: {
        if (visible) place()
        if (shown && oviora.hasSource) oviora.requestMenuBackdrop(backdropFraction, onLeft)
    }
    // Slides in from its edge and back out of it, never fading: the video is a native window beside
    // the drawer, so a see-through drawer showed the dark window under its hole instead of the game. The
    // hole follows the slide (covers are taken in scene coordinates).
    transform: Translate {
        id: slide
        x: menu.shown ? 0 : (menu.onLeft ? -1 : 1) * (menu.width + 8)
        Behavior on x { NumberAnimation { duration: 300; easing.type: Easing.OutQuart } }
        onXChanged: if (menu.visible && !menu.shown && Math.abs(x) >= menu.width) menu.closed()
    }
    // Where a section's back leads: the list, Ayarlar (the settings it holds) or Altyazı (Telefonda altyazı)
    function parentOf(name) {
        if (name === "subsphone") return "subs"
        if (name === "picture" || name === "sound" || name === "subs" || name === "keys") return "settings"
        return "game"
    }
    function depthOf(name) { return name === "game" ? 0 : 1 + depthOf(parentOf(name)) }
    function goBack() { section = parentOf(section) }
    property string lastSection: "game"
    // A section coming in: slides from the side it is reached from and fades in (panelFx 0 -> 1)
    property real panelFx: 1
    property real panelFrom: 28
    NumberAnimation { id: panelIn; target: menu; property: "panelFx"; from: 0; to: 1; duration: 260; easing.type: Easing.OutCubic }
    // Out of sight: hidden, back on the list, the live backdrop stopped
    function closed() {
        visible = false
        section = "game"
        backdropAsked = false
        oviora.clearMenuBackdrop()
    }
    signal requestFullscreen()
    // Bağlantıyı kes: Main plays the leaving effect, then ends the session
    signal requestDisconnect()
    // "game": the list; "picture": the view, quality and every picture setting; "sound": volume and microphone;
    // "subs": subtitles; "keys": keyboard and mouse keys; "chat": friends and messages
    property string section: "game"
    // The friend whose conversation the chat panel shows; "" for the list (or a group's conversation)
    property string chatPeer: ""
    // The group whose conversation the chat panel shows (OvioraServer.groups id); 0 for none
    property int chatGroup: 0
    readonly property bool chatInConversation: chatPeer.length > 0 || chatGroup !== 0
    // The settings tab the picture section opens on (ProPage.tab)
    property string pictureTab: "quality"


    readonly property bool pictureShown: section === "picture"
    readonly property bool keysShown: section === "keys"
    readonly property bool chatShown: section === "chat"
    readonly property bool soundShown: section === "sound"
    readonly property bool subsShown: section === "subs"
    readonly property bool phoneShown: section === "subsphone"
    readonly property bool izletShown: section === "izlet"
    readonly property bool settingsShown: section === "settings"
    // A friend's broadcast watched (Canlı yayınlar): only the picture, nothing that talks to a PS5
    readonly property bool watching: oviora.connection && oviora.connection.watching === true
    // The drawer: narrow for the list and the small sections, wider for messages and the picture settings,
    // widest for the keys table; never wider than the window
    width: Math.min(owner.width, keysShown ? Math.max(560, Math.min(980, Math.round(owner.width * 0.72)))
         : pictureShown || chatShown ? Math.max(340, Math.min(480, owner.width - 40))
         : Math.max(320, Math.min(420, owner.width - 40)))
    height: owner.height
    // The drawer widens and narrows between sections smoothly (only while it is open)
    Behavior on width { enabled: menu.shown; NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }

    // Against the window's right (or left) edge, top to bottom
    function place() {
        x = onLeft ? 0 : owner.width - width
        y = 0
    }
    // The pointer moves the focus between rows only once it has moved since the menu or a section opened
    property bool hoverFollows: false
    function holdHover() { hoverFollows = false; hoverArm.restart() }
    Timer { id: hoverArm; interval: 350; onTriggered: menu.hoverFollows = true }
    function show() {
        holdHover()
        place()
        // The frame behind the drawer (taken once per opening; nothing without a stream)
        if (!shown && oviora.hasSource) { backdropAsked = true; oviora.requestMenuBackdrop(backdropFraction, onLeft) }
        // shown first: a width change while it is still false would count as the slide-out ending
        shown = true
        visible = true
    }
    function hide() {
        shown = false
        // Keys go back to the window (fullscreen, Ctrl+O): nothing in the hidden menu keeps them
        if (owner && owner.contentItem) owner.contentItem.forceActiveFocus()
    }
    function open() { show(); mainPanel.forceActiveFocus(); backItem.forceActiveFocus(Qt.TabFocusReason) }
    function toggle() { oviora.logUi("ui-stream-menu", "toggle shown=" + shown); if (shown) hide(); else open() }
    function pictureItem() { return pictureLoader.item }
    function currentPictureTab() { return pictureLoader.item ? pictureLoader.item.tab : pictureTab }
    // Back from drawing a region on the picture: the same tab, the region card open
    function returnFromDrawing(tab, allEffects) {
        open()
        openPicture(tab)
        Qt.callLater(() => { if (pictureLoader.item) pictureLoader.item.revealMask(allEffects) })
    }
    function openPicture(tab) {
        pictureTab = tab
        if (pictureLoader.item) pictureLoader.item.tab = tab
        section = "picture"
    }
    // Messages: the menu open on the chat panel, on this friend's conversation ("" for the list).
    // MessageCenter counts nothing as unread while its conversation is open, so the panel keeps
    // it open only while it is on screen.
    function openChat(name) {
        if (!shown) open()
        section = "chat"
        showConversation(name || "")
    }
    // The same for a group's conversation
    function openGroupChat(id) {
        if (!shown) open()
        section = "chat"
        showGroup(id)
    }
    function showConversation(name) {
        chatGroup = 0
        chatPeer = name
        messages.openChat(name)
        if (!chatShown) return
        if (name.length) Qt.callLater(() => { if (chatConversation.visible) chatConversation.focusField() })
        else friendList.forceActiveFocus()
    }
    function showGroup(id) {
        chatPeer = ""
        chatGroup = id
        messages.openGroupChat(id)
        if (chatShown) Qt.callLater(() => { if (chatConversation.visible) chatConversation.focusField() })
    }
    // Esc in the chat panel: from a conversation (and its text field) to the list, then to the menu
    function chatBack() {
        if (chatInConversation) showConversation("")
        else section = "game"
    }
    onWidthChanged: if (visible) place()
    // The share of the picture's width the engine copies for the live backdrop: the strip under the widest
    // drawer (the keys), so moving between sections never needs a new strip and the picture keeps running
    readonly property real backdropFraction: owner && owner.width > 0
        ? Math.min(1, (Math.min(owner.width, Math.max(560, Math.min(980, Math.round(owner.width * 0.72)))) + 24) / owner.width) : 0.75
    // Only a window resize changes it; asked again once the size has settled
    onBackdropFractionChanged: if (shown && oviora.hasSource) backdropResize.restart()
    Timer { id: backdropResize; interval: 120; onTriggered: if (menu.shown && oviora.hasSource) oviora.requestMenuBackdrop(menu.backdropFraction, menu.onLeft) }
    onHeightChanged: if (visible) place()
    onSectionChanged: {
        holdHover()
        if (visible) place()
        // Deeper in: in from the right; back out: in from the left
        panelFrom = depthOf(section) < depthOf(lastSection) ? -28 : 28
        lastSection = section
        panelIn.restart()
        if (!chatShown && chatInConversation) { chatPeer = ""; chatGroup = 0; messages.openChat("") }
        if (keysShown) keysPanel.forceActiveFocus()
        else if (chatShown) { if (!chatInConversation) friendList.forceActiveFocus() }
        else if (soundShown) soundFirst.forceActiveFocus(Qt.TabFocusReason)
        else if (subsShown) subsFirst.forceActiveFocus(Qt.TabFocusReason)
        else if (phoneShown) { subtitleScreen.start(); phoneBack.forceActiveFocus(Qt.TabFocusReason) }
        else if (izletShown) izletFirst.forceActiveFocus(Qt.TabFocusReason)
        else if (settingsShown) {
            const back = settingsPanel.cameFrom
            Qt.callLater(() => { if (menu.settingsShown) (back && back.visible && back.enabled ? back : settingsFirst).forceActiveFocus(Qt.TabFocusReason) })
        }
        else if (pictureShown) picturePanel.forceActiveFocus()
        else {
            mainPanel.forceActiveFocus()
            const back = mainPanel.cameFrom
            Qt.callLater(() => { if (menu.section === "game" && menu.visible) (back && back.visible && back.enabled ? back : mainPanel.initialFocusItem).forceActiveFocus(Qt.TabFocusReason) })
        }
    }
    Connections {
        target: menu.owner
        function onWidthChanged() { if (menu.visible) menu.place() }
        function onHeightChanged() { if (menu.visible) menu.place() }
    }
    // The stream ended: nothing left to adjust
    Connections {
        target: oviora
        // Only a stream the menu was open over closes it (a test run opens it before the stream)
        function onSnapshotChanged() {
            if (oviora.hasSource) {
                menu.sawStream = true
                // Opened before the picture came: the live backdrop starts with it
                if (menu.shown && !menu.backdropAsked) { menu.backdropAsked = true; oviora.requestMenuBackdrop(menu.backdropFraction, menu.onLeft) }
            } else if (menu.sawStream) { menu.sawStream = false; menu.hide() }
        }
    }
    property bool sawStream: false
    property bool backdropAsked: false
    // Esc while a key is being changed only stops the change (KeysPage); in the keys panel it goes back.
    // In the chat panel it leaves the conversation (and the text field) first (chatBack).
    Shortcut {
        sequence: "Escape"
        enabled: menu.shown && !(menu.keysShown && ((keysLoader.item && keysLoader.item.capturing >= 0) || keysConfirm.visible))
        onActivated: {
            if (menu.chatShown) menu.chatBack()
            else if (menu.section !== "game") menu.goBack()
            else menu.hide()
        }
    }

    // ── What the menu shows about the stream ──
    // The engine snapshot (hasSource) is current; ps5.active only follows the PS5 dialog's status text
    readonly property bool ps5Active: oviora.hasSource || !!(oviora.ps5 && oviora.ps5.active)
    readonly property bool connected: ps5Active ? oviora.running : oviora.hasSource
    // The console's name from the consoles the home knows; the address otherwise
    // The console's name: from this network's list for a LAN stream, from the source label
    // ("PS5 · <name>") for one over PSN
    readonly property string consoleName: {
        const host = (oviora.ps5 && oviora.ps5.host) || ""
        for (const c of (oviora.lanConsoles || [])) {
            if (host && c && c.address === host && c.name)
                return c.name
        }
        const title = oviora.sourceTitle || ""
        const m = /·\s*(.+)$/.exec(title)
        return m && m[1] !== host ? m[1] : ""
    }
    readonly property string streamTitle: ps5Active ? (consoleName.length ? consoleName : "PS5") : (oviora.sourceTitle || "PS5")
    // Live numbers of the PS5 stream (oviora.streamStats, as the old menu's connection line)
    readonly property var stats: oviora.streamStats || ({})
    readonly property bool statsShown: stats.active === true
    readonly property string liveResolution: statsShown && (stats.height || 0) > 0 ? stats.height + "p" : ""
    readonly property string liveFps: statsShown && (stats.fps || 0) > 0 ? Math.round(stats.fps).toString() : ""
    readonly property string liveMbps: statsShown ? Number(stats.mbps || 0).toFixed(1) : ""

    // ── Quality presets (mapped to SR + NR without touching frame generation) ──
    readonly property var sr3Cap: (oviora.effectCapabilities || {}).sr3 || ({available: true, reason: ""})
    readonly property var sr4Cap: (oviora.effectCapabilities || {}).sr4 || ({available: true, reason: ""})
    readonly property var nrCap: typeof oviora.effectAvailability === "function" ? oviora.effectAvailability("nr") : ({available: true, reason: ""})
    readonly property bool sr3Available: !!sr3Cap.available
    readonly property bool sr4Available: !!sr4Cap.available
    readonly property bool nrAvailable: !!nrCap.available
    readonly property bool presetStandard: !oviora.srEnabled && !oviora.nrAnyEnabled
    readonly property bool presetHigh: oviora.srEnabled && !oviora.nrAnyEnabled && oviora.videoSrQuality === 3 && !oviora.dualSrEnabled
    readonly property bool presetHighSharp: oviora.srEnabled && oviora.nrAnyEnabled && oviora.videoSrQuality === 3 && !oviora.dualSrEnabled
    readonly property bool presetSharpest: oviora.srEnabled && oviora.nrAnyEnabled && oviora.videoSrQuality === 4 && !oviora.dualSrEnabled
    readonly property bool presetCustom: !(presetStandard || presetHigh || presetHighSharp || presetSharpest)
    function applyQualityPreset(kind) {
        if (kind === "standard") {
            oviora.srEnabled = false
            if (oviora.nrLayers.length > 0) oviora.setAllNrEnabled(false)
        } else if (kind === "high") {
            if (oviora.dualSrEnabled) oviora.dualSrEnabled = false
            oviora.videoSrQuality = 3
            oviora.srEnabled = true
            if (oviora.nrLayers.length > 0) oviora.setAllNrEnabled(false)
        } else if (kind === "highSharp") {
            if (oviora.dualSrEnabled) oviora.dualSrEnabled = false
            oviora.videoSrQuality = 3
            oviora.srEnabled = true
            if (oviora.nrLayers.length === 0) oviora.addEffect("nr")
            oviora.setAllNrEnabled(true)
        } else if (kind === "sharpest") {
            if (oviora.dualSrEnabled) oviora.dualSrEnabled = false
            oviora.videoSrQuality = 4
            oviora.srEnabled = true
            if (oviora.nrLayers.length === 0) oviora.addEffect("nr")
            oviora.setAllNrEnabled(true)
        } else {
            menu.openPicture("quality")
        }
    }

    // ── Icons: the old Oviora's outline set (1.8 px on a 24 grid), drawn as SVG images so the
    // software scene graph of this app draws them too ──
    readonly property var ikonPaths: ({
        "camera": "M4 7.5h3l1.5 -2.5h7l1.5 2.5h3a1 1 0 0 1 1 1v10a1 1 0 0 1 -1 1h-16a1 1 0 0 1 -1 -1v-10a1 1 0 0 1 1 -1z M12 9.5a3.6 3.6 0 1 1 0 7.2a3.6 3.6 0 1 1 0 -7.2z",
        "mic": "M12 3a3 3 0 0 1 3 3v5a3 3 0 0 1 -6 0v-5a3 3 0 0 1 3 -3z M18.5 11a6.5 6.5 0 0 1 -13 0 M12 17.5v3.5 M9 21h6",
        "fit": "M3.5 8v-2.5a2 2 0 0 1 2 -2h2.5 M16 3.5h2.5a2 2 0 0 1 2 2v2.5 M20.5 16v2.5a2 2 0 0 1 -2 2h-2.5 M8 20.5h-2.5a2 2 0 0 1 -2 -2v-2.5 M8.5 8.5h7a1 1 0 0 1 1 1v5a1 1 0 0 1 -1 1h-7a1 1 0 0 1 -1 -1v-5a1 1 0 0 1 1 -1z",
        "zoom": "M10.5 3.5a7 7 0 1 1 0 14a7 7 0 1 1 0 -14z M20.5 20.5l-5 -5 M10.5 7.5v6 M7.5 10.5h6",
        "stretch": "M14.5 3.5h6v6 M9.5 20.5h-6v-6 M20.5 3.5l-6.5 6.5 M3.5 20.5l6.5 -6.5",
        "stats": "M3.5 20.5h17 M7 17v-6 M12 17v-12 M17 17v-9",
        "mic-off": "M15 10.5v-4.5a3 3 0 0 0 -5.8 -1.1 M9 9v2a3 3 0 0 0 4.9 2.3 M18.5 11a6.5 6.5 0 0 1 -0.7 2.9 M15.9 16.2a6.5 6.5 0 0 1 -10.4 -5.2 M12 17.5v3.5 M9 21h6 M4 4l16 16",
        "signal-weak": "M2.5 9a14 14 0 0 1 19 0 M5.5 12.5a9.5 9.5 0 0 1 13 0 M9 16a4.5 4.5 0 0 1 6 0 M12 19.5v0.01 M3.5 3.5l17 17",
        "display": "M5 4h14a2 2 0 0 1 2 2v9a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-9a2 2 0 0 1 2 -2z M8 21h8 M12 17v4",
        "image": "M5 4h14a2 2 0 0 1 2 2v12a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-12a2 2 0 0 1 2 -2z M3.5 16.5l4.5 -4.5l5.5 5.5 M13 15l3 -3l4.5 4.5 M15.5 6.8a1.7 1.7 0 1 1 0 3.4a1.7 1.7 0 1 1 0 -3.4z",
        "sleep": "M20 14.6a8.2 8.2 0 1 1 -10.6 -10.6a6.6 6.6 0 0 0 10.6 10.6z",
        "close": "M6 6l12 12 M18 6l-12 12",
        "volume": "M4 9.5h3l5 -4.5v14l-5 -4.5h-3a1 1 0 0 1 -1 -1v-3a1 1 0 0 1 1 -1z M15.5 9a4 4 0 0 1 0 6 M18.5 6a8 8 0 0 1 0 12",
        "volume-off": "M4 9.5h3l5 -4.5v14l-5 -4.5h-3a1 1 0 0 1 -1 -1v-3a1 1 0 0 1 1 -1z M16 9.5l5 5 M21 9.5l-5 5",
        "chevron-down": "M6 9l6 6l6 -6",
        "chevron-left": "M15 5.5l-6.5 6.5l6.5 6.5",
        "play": "M8 5.5v13l10.5 -6.5z",
        "keyboard": "M4 6.5h16a1.5 1.5 0 0 1 1.5 1.5v8a1.5 1.5 0 0 1 -1.5 1.5h-16a1.5 1.5 0 0 1 -1.5 -1.5v-8a1.5 1.5 0 0 1 1.5 -1.5z M7 10h0.01 M10.5 10h0.01 M14 10h0.01 M17.5 10h0.01 M7.5 14h9",
        "subtitles": "M4 5.5h16a1.5 1.5 0 0 1 1.5 1.5v10a1.5 1.5 0 0 1 -1.5 1.5h-16a1.5 1.5 0 0 1 -1.5 -1.5v-10a1.5 1.5 0 0 1 1.5 -1.5z M6.5 14.5h5 M13.5 14.5h4 M6.5 11h3 M11.5 11h6",
        "chevron-right": "M9 5.5l6.5 6.5l-6.5 6.5",
        "chat": "M5 4.5h14a2 2 0 0 1 2 2v9a2 2 0 0 1 -2 2h-8l-4.5 3.5v-3.5h-1.5a2 2 0 0 1 -2 -2v-9a2 2 0 0 1 2 -2z M8.5 11h0.01 M12 11h0.01 M15.5 11h0.01",
        "broadcast": "M12 10.5a1.5 1.5 0 1 1 0 3a1.5 1.5 0 1 1 0 -3z M8.5 8.5a5 5 0 0 0 0 7 M15.5 8.5a5 5 0 0 1 0 7 M5.6 5.6a9 9 0 0 0 0 12.8 M18.4 5.6a9 9 0 0 1 0 12.8",
        "eye": "M2.5 12s3.5 -6.5 9.5 -6.5s9.5 6.5 9.5 6.5s-3.5 6.5 -9.5 6.5s-9.5 -6.5 -9.5 -6.5z M12 9.5a2.5 2.5 0 1 1 0 5a2.5 2.5 0 1 1 0 -5z",
        "sliders": "M4 7h9 M17 7h3 M15 5v4 M4 17h3 M11 17h9 M9 15v4",
        "bell": "M6 16.5v-5.5a6 6 0 0 1 12 0v5.5l1.5 1.5h-15z M10 20.5a2 2 0 0 0 4 0"
    })
    component Ikon: Image {
        id: ikon
        property string name
        property color color: Tema.text
        property real stroke: 1.8
        readonly property string pathData: menu.ikonPaths[name] || ""
        fillMode: Image.PreserveAspectFit
        smooth: true
        sourceSize.width: Math.max(1, Math.ceil(width * 2 * Screen.devicePixelRatio))
        sourceSize.height: Math.max(1, Math.ceil(height * 2 * Screen.devicePixelRatio))
        source: {
            if (!pathData.length)
                return ""
            const paint = "rgb(" + Math.round(color.r * 255) + "," + Math.round(color.g * 255) + "," + Math.round(color.b * 255) + ")"
            const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="'
                + paint + '" opacity="' + color.a + '" stroke-width="' + stroke + '" stroke-linecap="round" stroke-linejoin="round">'
                + '<path d="' + pathData + '"/></svg>'
            return "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg)
        }
    }

    // ── Pieces of the stream menu (as the old Oviora's) ──
    // A big icon + label tile; `selected` marks the current choice. `glyph` names an Ikon drawing.
    component MenuTile: AbstractButton {
        id: tile
        property string glyph: ""
        property bool selected: false
        property color tone: Tema.text
        property bool tinted: false
        // A count on the icon's corner (unread messages); 0 hides it
        property int badge: 0
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        implicitWidth: 112
        implicitHeight: 112
        // Narrow windows: tiles give up width instead of pushing the row out of the panel
        Layout.fillWidth: true
        Layout.minimumWidth: 78
        Layout.maximumWidth: 112
        Layout.preferredWidth: 112
        Keys.onReturnPressed: tile.clicked()
        Keys.onEnterPressed: tile.clicked()

        // No box of its own: hover and the current choice show as a soft ground under icon and label
        background: Rectangle {
            radius: 12
            color: {
                if (tile.selected)
                    return tile.hovered || tile.activeFocus ? Qt.rgba(0x7B / 255, 0x5C / 255, 1, 0.24) : Tema.accentSoft
                if (tile.tinted)
                    return tile.down ? Tema.soft(Tema.danger, 0.18) : (tile.hovered || tile.activeFocus ? Tema.soft(Tema.danger, 0.1) : "transparent")
                return tile.down ? Tema.line : (tile.hovered || tile.activeFocus ? Tema.surface2 : "transparent")
            }
            border.width: tile.activeFocus ? 2 : 0
            border.color: Tema.accent
            opacity: tile.enabled ? 1 : 0.45
        }
        contentItem: Item {
            opacity: tile.enabled ? 1 : 0.5
            Ikon {
                id: tileGlyph
                anchors {
                    horizontalCenter: parent.horizontalCenter
                    bottom: parent.verticalCenter
                    bottomMargin: -2
                }
                width: 28
                height: 28
                name: tile.glyph
                color: tile.selected ? Tema.accentText : tile.tone
            }
            Rectangle {
                visible: tile.badge > 0
                anchors {
                    left: tileGlyph.right
                    leftMargin: -10
                    bottom: tileGlyph.top
                    bottomMargin: -10
                }
                height: 20
                width: Math.max(20, badgeText.implicitWidth + 10)
                radius: 10
                color: Tema.accent
                Text {
                    id: badgeText
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    text: tile.badge > 99 ? "99+" : String(tile.badge)
                    font.family: Tema.body
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: "white"
                }
            }
            Text {
                textFormat: Text.PlainText
                anchors {
                    top: tileGlyph.bottom
                    left: parent.left
                    right: parent.right
                    topMargin: 10
                    leftMargin: 10
                    rightMargin: 10
                }
                text: tile.text
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                lineHeight: 0.95
                font.family: Tema.body
                font.pixelSize: 14
                color: tile.tinted ? tile.tone : (tile.selected ? Tema.text : Tema.muted)
            }
        }
    }

    // A pill in a row of choices
    component MenuPill: AbstractButton {
        id: pill
        property bool selected: false
        property string note: ""
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        implicitHeight: 40
        implicitWidth: pillLabel.implicitWidth + 30
        Keys.onReturnPressed: pill.clicked()
        Keys.onEnterPressed: pill.clicked()
        background: Rectangle {
            radius: height / 2
            color: pill.selected ? Tema.accentSoft : (pill.hovered || pill.activeFocus ? Tema.surface2 : "transparent")
            border.width: pill.activeFocus ? 2 : 0
            border.color: Tema.accent
            opacity: pill.enabled ? 1 : 0.45
        }
        contentItem: Text {
            textFormat: Text.PlainText
            id: pillLabel
            text: pill.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font.family: Tema.body
            font.pixelSize: 15
            font.weight: pill.selected ? Font.DemiBold : Font.Normal
            color: pill.selected ? Tema.text : Tema.muted
            opacity: pill.enabled ? 1 : 0.5
        }
        ToolTip.visible: !pill.enabled && pill.hovered && pill.note.length > 0
        ToolTip.delay: 200
        ToolTip.text: pill.note
    }

    // An outlined pill with an icon and a label (close the menu, back to the menu)
    component OutlinePill: AbstractButton {
        id: outline
        property string glyph: ""
        property string keyHint: ""
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        implicitHeight: 40
        implicitWidth: outlineRow.implicitWidth + 30
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        background: Rectangle {
            radius: height / 2
            color: outline.hovered || outline.activeFocus ? Tema.surface2 : "transparent"
            border.width: outline.activeFocus ? 2 : 1
            border.color: outline.activeFocus ? Tema.accent : Tema.line
        }
        contentItem: Item {
            Row {
                id: outlineRow
                anchors.centerIn: parent
                spacing: 8
                Ikon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16
                    height: 16
                    name: outline.glyph
                    color: Tema.muted
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    text: outline.text
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.text
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    visible: text.length > 0
                    text: outline.keyHint
                    font.family: Tema.body
                    font.pixelSize: 13
                    color: Tema.muted
                }
            }
        }
    }

    // A slim horizontal slider in the palette; Left/Right change it, Up/Down leave it
    component MenuSlider: Basic.Slider {
        id: menuSlider
        focusPolicy: Qt.StrongFocus
        implicitHeight: 40
        background: Rectangle {
            x: menuSlider.leftPadding
            y: menuSlider.topPadding + menuSlider.availableHeight / 2 - height / 2
            width: menuSlider.availableWidth
            height: 6
            radius: 3
            color: Tema.line
            Rectangle {
                width: menuSlider.visualPosition * parent.width
                height: parent.height
                radius: 3
                color: Tema.accent
            }
        }
        handle: Rectangle {
            x: menuSlider.leftPadding + menuSlider.visualPosition * (menuSlider.availableWidth - width)
            y: menuSlider.topPadding + menuSlider.availableHeight / 2 - height / 2
            width: 20
            height: 20
            radius: 10
            color: Tema.text
            border.width: menuSlider.activeFocus ? 3 : 0
            border.color: Tema.accent
        }
    }

    // A live number with its label, for the menu's connection line
    component StatChip: Row {
        property string value: ""
        property string label: ""
        property color tone: Tema.text
        spacing: 6
        Text {
            textFormat: Text.PlainText
            anchors.baseline: statLabel.baseline
            text: parent.value
            font.family: Tema.display
            font.weight: Font.DemiBold
            font.pixelSize: 20
            color: parent.tone
        }
        Text {
            textFormat: Text.PlainText
            id: statLabel
            text: parent.label
            font.family: Tema.body
            font.pixelSize: 14
            color: Tema.muted
        }
    }

    // A row of the drawer: icon, label, a count or a value on the right. The focused (or pointed) row gets a
    // light band fading out to the right, as a game's pause menu; the pointer moves the focus with it.
    component DrawerItem: AbstractButton {
        id: row
        property string glyph: ""
        property int badge: 0
        property string value: ""
        property bool opens: false       // leads into a section: a small arrow on the right
        property color tone: Tema.text
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Layout.fillWidth: true
        implicitHeight: 50
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        // Only a pointer that moves: rows appearing under a resting one (the menu or a section opening) keep the focus
        onHoveredChanged: if (hovered && enabled && !activeFocus && menu.hoverFollows) forceActiveFocus(Qt.MouseFocusReason)
        readonly property bool lit: activeFocus && enabled
        readonly property color ink: lit ? "#16141d" : tone
        background: Rectangle {
            radius: 3
            opacity: row.enabled ? 1 : 0.45
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: row.lit ? Qt.rgba(1, 1, 1, 0.94) : (row.hovered ? Qt.rgba(1, 1, 1, 0.06) : "transparent") }
                GradientStop { position: 0.6; color: row.lit ? Qt.rgba(0.92, 0.92, 0.95, 0.84) : (row.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent") }
                GradientStop { position: 1.0; color: row.lit ? Qt.rgba(0.85, 0.85, 0.9, 0.5) : "transparent" }
            }
            border.width: row.lit ? 1 : 0
            border.color: Qt.rgba(1, 1, 1, 0.4)
        }
        contentItem: RowLayout {
            spacing: 14
            opacity: row.enabled ? 1 : 0.5
            Ikon {
                Layout.leftMargin: 16
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                name: row.glyph
                color: row.ink
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: row.text
                elide: Text.ElideRight
                font.family: Tema.body
                font.pixelSize: 18
                font.weight: row.lit ? Font.DemiBold : Font.Normal
                color: row.ink
            }
            Rectangle {
                visible: row.badge > 0
                Layout.preferredHeight: 22
                Layout.preferredWidth: Math.max(22, rowBadge.implicitWidth + 12)
                radius: 11
                color: Tema.accent
                Text {
                    id: rowBadge
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    text: row.badge > 99 ? "99+" : String(row.badge)
                    font.family: Tema.body
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: "white"
                }
            }
            Text {
                textFormat: Text.PlainText
                visible: text.length > 0
                text: row.value
                font.family: Tema.body
                font.pixelSize: 15
                color: row.lit ? "#2a2733" : Tema.muted
            }
            Ikon {
                visible: row.opens
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                name: "chevron-right"
                color: row.lit ? "#2a2733" : Tema.faint
            }
            Item { Layout.preferredWidth: 6 }
        }
    }

    // A section's top line inside the drawer: back to the list, its title, close
    component SectionHeader: RowLayout {
        id: header
        property string title: ""
        property alias backButton: headerBack
        spacing: 12
        OutlinePill {
            id: headerBack
            glyph: "chevron-left"
            text: menu.parentOf(menu.section) === "settings" ? qsTr("Ayarlar") : menu.parentOf(menu.section) === "subs" ? qsTr("Altyazı") : qsTr("Menü")
            onClicked: menu.goBack()
        }
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: header.title
            elide: Text.ElideRight
            font.family: Tema.display
            font.weight: Font.DemiBold
            font.pixelSize: 20
            font.letterSpacing: -0.3
            color: Tema.text
        }
        AbstractButton {
            id: headerClose
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            implicitWidth: 40
            implicitHeight: 40
            onClicked: menu.hide()
            background: Rectangle {
                radius: 20
                color: headerClose.hovered || headerClose.activeFocus ? Tema.surface2 : "transparent"
                border.width: headerClose.activeFocus ? 2 : 0
                border.color: Tema.accent
            }
            contentItem: Item {
                Ikon {
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    name: "close"
                    color: headerClose.hovered ? Tema.text : Tema.muted
                }
            }
        }
    }

    // Whether item sits inside ancestor (Item.isAncestorOf is not callable from QML)
    function within(ancestor, item) {
        for (let p = item; p; p = p.parent)
            if (p === ancestor) return true
        return false
    }
    // Up/Down through a section's controls in their tab order (sliders keep Left/Right)
    function stepFocus(scope, forward) {
        const now = scope.Window.activeFocusItem
        let next = now ? now.nextItemInFocusChain(forward) : null
        for (let guard = 0; next && guard < 64; ++guard) {
            if (next.visible && next.enabled && (scope === next || menu.within(scope, next))) break
            next = next.nextItemInFocusChain(forward)
        }
        if (next && next !== scope && menu.within(scope, next)) next.forceActiveFocus(Qt.TabFocusReason)
    }

    component GroupLabel: Text {
        font.family: Tema.body
        font.pixelSize: 13
        color: Tema.muted
    }

    // ── The drawer's ground: the frame of the moment the menu opened, tiny and stretched back up (soft as a
    // blur on any renderer), under a lightly see-through coat; solid when there is no frame (HDR, no stream).
    // The video window has a hole under the whole drawer (videoCover). ──
    Item {
        id: drawerGround
        anchors.fill: parent
        clip: true
        Rectangle { anchors.fill: parent; color: "#0c0b11" }
        Image {
            id: backdropImage
            x: (menu.onLeft ? 0 : menu.owner.width * (1 - menu.backdropFraction)) - menu.x
            y: -menu.y
            width: menu.owner.width * menu.backdropFraction
            height: menu.owner.height
            // Live: a new small picture of the strip under the drawer about 8 times a second (already blurred, MenuBackdropStore)
            source: oviora.menuBackdrop || ""
            fillMode: Image.Stretch
            smooth: true
            cache: false
            asynchronous: false
            opacity: source != "" ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        }
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(Tema.surface.r, Tema.surface.g, Tema.surface.b, backdropImage.opacity > 0.5 ? 0.80 : 0.97)
            Behavior on color { ColorAnimation { duration: 220 } }
        }
        Rectangle {
            x: menu.onLeft ? parent.width - 1 : 0
            width: 1
            height: parent.height
            color: Tema.line
        }
    }

    // ── The list ──
    FocusScope {
        id: mainPanel
        visible: menu.section === "game"
        opacity: menu.panelFx
        transform: Translate { x: (1 - menu.panelFx) * menu.panelFrom }
        anchors.fill: parent
        focus: true
        property Item initialFocusItem: backItem
        // The row that opened the section shown last (focused again on the way back)
        property Item cameFrom: null
        function enter(row, sectionName) { cameFrom = row; menu.section = sectionName }
        Keys.onPressed: event => {
            switch (event.key) {
            case Qt.Key_Up:
            case Qt.Key_Down:
                if (mainPanel.Window.activeFocusItem === mainPanel || !menu.within(mainPanel, mainPanel.Window.activeFocusItem))
                    initialFocusItem.forceActiveFocus(Qt.TabFocusReason)
                else
                    menu.stepFocus(mainPanel, event.key === Qt.Key_Down)
                event.accepted = true
                break
            case Qt.Key_Right:
                if (mainPanel.Window.activeFocusItem && mainPanel.Window.activeFocusItem.opens) mainPanel.Window.activeFocusItem.clicked()
                event.accepted = true
                break
            case Qt.Key_Menu:
            case Qt.Key_Back:
                menu.hide()
                event.accepted = true
                break
            }
        }

        // A short window: the list scrolls, and the focused row is kept in view
        Flickable {
            id: listFlick
            anchors.fill: parent
            contentWidth: width
            contentHeight: listColumn.height + 54
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: VScrollBar { }
            function keepInView(item) {
                if (!item || !menu.within(listColumn, item)) return
                const p = item.mapToItem(listColumn, 0, 0)
                const top = p.y + 30 - 8
                const bottom = p.y + 30 + item.height + 8
                if (top < contentY) contentY = Math.max(0, top)
                else if (bottom > contentY + height) contentY = Math.min(contentHeight - height, bottom - height)
            }
            Connections {
                target: mainPanel.Window.window
                function onActiveFocusItemChanged() { if (mainPanel.visible) listFlick.keepInView(mainPanel.Window.activeFocusItem) }
            }

        ColumnLayout {
            id: listColumn
            x: 26
            y: 30
            width: listFlick.width - 26 - 22
            // As tall as the window allows, so the last two rows sit at the bottom; taller when they do not fit
            height: Math.max(implicitHeight, listFlick.height - 54)
            spacing: 2

            RowLayout {
                spacing: 12
                OvioraMark {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                }
                Text {
                    textFormat: Text.PlainText
                    text: "Oviora"
                    font.family: Tema.display
                    font.pixelSize: 28
                    font.weight: Font.DemiBold
                    font.letterSpacing: -0.6
                    color: Tema.text
                }
            }

            // ── The console and the connection ──
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 22
                spacing: 14
                KonsolSimge {
                    Layout.preferredWidth: 48
                    Layout.preferredHeight: 14
                    tone: !oviora.hasSource && !menu.ps5Active ? Tema.faint : (menu.connected ? Tema.ok : Tema.warm)
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: menu.streamTitle
                        font.family: Tema.display
                        font.weight: Font.DemiBold
                        font.pixelSize: 19
                        color: Tema.text
                        elide: Text.ElideRight
                    }
                    Row {
                        spacing: 7
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8
                            height: 8
                            radius: 4
                            color: !oviora.hasSource && !menu.ps5Active ? Tema.muted : (menu.connected ? Tema.ok : Tema.warm)
                        }
                        Text {
                            textFormat: Text.PlainText
                            text: !oviora.hasSource && !menu.ps5Active ? qsTr("Bağlantı yok") : (menu.connected ? qsTr("Bağlı") : qsTr("Bağlanıyor…"))
                            font.family: Tema.body
                            font.pixelSize: 14
                            color: Tema.muted
                        }
                    }
                }
            }
            // The live numbers, in rows under the console
            Flow {
                Layout.fillWidth: true
                Layout.topMargin: 10
                visible: menu.statsShown
                spacing: 18
                StatChip {
                    visible: menu.liveResolution.length > 0
                    value: menu.liveResolution
                    label: menu.liveFps.length ? menu.liveFps + " fps" : ""
                }
                StatChip {
                    value: menu.liveMbps
                    label: "Mbps"
                }
                StatChip {
                    readonly property real loss: menu.stats.packetLoss || 0
                    value: "%" + (loss * 100).toFixed(1)
                    label: qsTr("paket kaybı")
                    tone: loss > 0.01 ? Tema.danger : Tema.text
                }
                StatChip {
                    value: String(oviora.skippedFrames)
                    label: qsTr("atlanan kare")
                    tone: oviora.skippedFrames > 0 ? Tema.danger : Tema.text
                }
                // Oviora: what the PS5 itself reports about the link
                StatChip {
                    readonly property real rtt: menu.stats.rttMs !== undefined ? menu.stats.rttMs : -1
                    visible: rtt > 0   // 0: the PS5 sent no figure
                    value: rtt < 10 ? rtt.toFixed(1) : rtt.toFixed(0)
                    label: qsTr("ms gecikme (PS5)")
                    tone: rtt > 80 ? Tema.danger : Tema.text
                }
            }

            Item { Layout.preferredHeight: 24 }

            DrawerItem {
                id: backItem
                text: qsTr("Oyuna dön")
                glyph: "play"
                value: "Esc"
                onClicked: menu.hide()
            }
            // Friends, groups and their messages without leaving the game (Ctrl+Enter opens it straight away)
            DrawerItem {
                id: messagesItem
                text: qsTr("Mesajlar")
                glyph: "chat"
                badge: messages.unreadTotal
                opens: true
                onClicked: { mainPanel.cameFrom = messagesItem; menu.openChat("") }
            }
            // Friends watch the stream in the browser (Ctrl+O > İzlet)
            DrawerItem {
                id: izletRow
                visible: izlet.available && !menu.watching
                text: qsTr("İzlet")
                glyph: "broadcast"
                opens: true
                value: izlet.state === "canli" ? qsTr("Yayında · %1").arg(izlet.viewers.length) : izlet.state === "aciliyor" ? qsTr("Açılıyor") : qsTr("Kapalı")
                tone: izlet.state === "canli" ? Tema.danger : Tema.text
                onClicked: mainPanel.enter(izletRow, "izlet")
            }
            DrawerItem {
                id: screenshotRow
                text: qsTr("Ekran görüntüsü")
                glyph: "camera"
                enabled: oviora.hasSource
                onClicked: oviora.takeScreenshot()
            }
            DrawerItem {
                id: settingsRow
                text: qsTr("Ayarlar")
                glyph: "sliders"
                opens: true
                onClicked: { settingsPanel.cameFrom = null; mainPanel.enter(settingsRow, "settings") }
            }

            Item { Layout.fillHeight: true }

            DrawerItem {
                id: sleepRow
                visible: !menu.watching
                text: qsTr("Dinlenmeye al")
                glyph: "sleep"
                tone: Tema.warm
                enabled: menu.statsShown
                onClicked: {
                    menu.hide()
                    oviora.ps5Rest()
                }
            }
            DrawerItem {
                id: disconnectRow
                text: menu.watching ? qsTr("İzlemeyi bitir") : qsTr("Bağlantıyı kes")
                glyph: "close"
                tone: Tema.danger
                onClicked: menu.requestDisconnect()
            }
        }
        }
    }

    // ── Ayarlar: how the menu itself is ──
    FocusScope {
        id: settingsPanel
        visible: menu.settingsShown
        opacity: menu.panelFx
        transform: Translate { x: (1 - menu.panelFx) * menu.panelFrom }
        anchors.fill: parent
        // The row that opened the section shown last (focused again on the way back)
        property Item cameFrom: null
        function enter(row, sectionName) { cameFrom = row; menu.section = sectionName }
        Keys.onUpPressed: menu.stepFocus(settingsPanel, false)
        Keys.onDownPressed: menu.stepFocus(settingsPanel, true)
        Keys.onLeftPressed: if (Window.activeFocusItem === sideRight) sideLeft.forceActiveFocus(Qt.TabFocusReason)
        Keys.onRightPressed: {
            const item = Window.activeFocusItem
            if (item && item.opens) item.clicked()
            else if (item === sideLeft) sideRight.forceActiveFocus(Qt.TabFocusReason)
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 22
            anchors.topMargin: 24
            spacing: 2
            SectionHeader { Layout.fillWidth: true; Layout.bottomMargin: 18; title: qsTr("Ayarlar") }
            DrawerItem {
                id: pictureRow
                text: qsTr("Görüntü")
                glyph: "image"
                opens: true
                value: menu.presetStandard ? qsTr("Standart") : menu.presetHigh ? qsTr("Yüksek")
                     : menu.presetHighSharp ? qsTr("Yüksek + keskin") : menu.presetSharpest ? qsTr("En keskin") : qsTr("Özel")
                onClicked: { settingsPanel.cameFrom = pictureRow; menu.openPicture("quality") }
            }
            DrawerItem {
                id: soundRow
                text: qsTr("Ses ve mikrofon")
                glyph: oviora.volume > 0 && !oviora.muted ? "volume" : "volume-off"
                opens: true
                value: "%" + Math.round(oviora.volume * 100)
                onClicked: settingsPanel.enter(soundRow, "sound")
            }
            DrawerItem {
                id: subsRow
                visible: subtitles.available
                readonly property var cfg: subtitles.config || ({})
                text: qsTr("Altyazı")
                glyph: "subtitles"
                opens: true
                value: cfg.enabled ? qsTr("Açık") : qsTr("Kapalı")
                onClicked: settingsPanel.enter(subsRow, "subs")
            }
            DrawerItem {
                id: keysRow
                visible: !menu.watching
                text: qsTr("Klavye ve fare")
                glyph: "keyboard"
                opens: true
                value: oviora.preferences.keyboardMouse !== false ? qsTr("Açık") : qsTr("Kapalı")
                onClicked: settingsPanel.enter(keysRow, "keys")
            }
            // The statistics over the picture (preferences.showStreamStats; Main.qml draws them)
            DrawerItem {
                id: statsRow
                readonly property bool on: oviora.preferences.showStreamStats === true
                text: qsTr("İstatistikler")
                glyph: "stats"
                value: on ? qsTr("Açık") : qsTr("Kapalı")
                onClicked: oviora.setPreference("showStreamStats", !on)
            }
            GroupLabel { Layout.topMargin: 22; Layout.bottomMargin: 8; text: qsTr("Menü konumu") }
            RowLayout {
                spacing: 8
                MenuPill {
                    id: sideLeft
                    text: qsTr("Sol")
                    selected: menu.onLeft
                    onClicked: oviora.setPreference("streamMenuSide", "left")
                }
                MenuPill {
                    id: sideRight
                    text: qsTr("Sağ")
                    selected: !menu.onLeft
                    onClicked: oviora.setPreference("streamMenuSide", "right")
                }
            }
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 8
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                text: qsTr("Menü, oyun ekranının bu kenarından açılır.")
                font.family: Tema.body
                font.pixelSize: 13
                color: Tema.muted
            }
            Item { Layout.fillHeight: true }
        }
    }
    readonly property Item settingsFirst: pictureRow

    // ── Ses ve mikrofon ──
    FocusScope {
        id: soundPanel
        visible: menu.soundShown
        opacity: menu.panelFx
        transform: Translate { x: (1 - menu.panelFx) * menu.panelFrom }
        anchors.fill: parent
        Keys.onUpPressed: menu.stepFocus(soundPanel, false)
        Keys.onDownPressed: menu.stepFocus(soundPanel, true)
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 22
            anchors.topMargin: 24
            spacing: 14
            SectionHeader { Layout.fillWidth: true; title: qsTr("Ses ve mikrofon") }
            GroupLabel { Layout.topMargin: 10; text: qsTr("Ses düzeyi") }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Ikon {
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                    name: volumeSlider.value > 0 && !oviora.muted ? "volume" : "volume-off"
                    color: Tema.muted
                }
                MenuSlider {
                    id: volumeSlider
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    stepSize: 0.02
                    value: oviora.volume
                    onMoved: oviora.volume = value
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.preferredWidth: 48
                    text: "%" + (volumeSlider.value * 100).toFixed(0)
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.text
                }
            }
            // The microphone to the PS5 (oviora.micOn / toggleMicrophone)
            DrawerItem {
                id: micRow
                visible: !menu.watching
                readonly property bool micOn: oviora.micOn
                Layout.topMargin: 6
                text: qsTr("Mikrofon")
                glyph: micOn ? "mic" : "mic-off"
                value: micOn ? qsTr("Açık") : qsTr("Kapalı")
                enabled: menu.statsShown
                onClicked: oviora.toggleMicrophone()
            }
            Item { Layout.fillHeight: true }
        }
    }
    readonly property Item soundFirst: volumeSlider

    // ── Altyazı (the same values as Ayarlar › Altyazı; Ctrl+T / Ctrl+B do the same) ──
    FocusScope {
        id: subsPanel
        visible: menu.subsShown
        opacity: menu.panelFx
        transform: Translate { x: (1 - menu.panelFx) * menu.panelFrom }
        anchors.fill: parent
        readonly property var cfg: subtitles.config || ({})
        Keys.onUpPressed: menu.stepFocus(subsPanel, false)
        Keys.onDownPressed: menu.stepFocus(subsPanel, true)
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 22
            anchors.topMargin: 24
            spacing: 2
            SectionHeader { Layout.fillWidth: true; Layout.bottomMargin: 18; title: qsTr("Altyazı") }
            DrawerItem {
                id: subOnRow
                text: qsTr("Altyazı")
                glyph: "subtitles"
                value: subsPanel.cfg.enabled ? qsTr("Açık") : qsTr("Kapalı")
                onClicked: if (subtitles.config) subtitles.config.enabled = !subsPanel.cfg.enabled
            }
            DrawerItem {
                text: qsTr("Bölge seç")
                glyph: "fit"
                enabled: menu.connected
                onClicked: {
                    menu.hide()
                    subtitles.selecting = true
                }
            }
            DrawerItem {
                text: qsTr("İngilizcesini kapat")
                glyph: "image"
                value: subsPanel.cfg.coverOriginal ? qsTr("Açık") : qsTr("Kapalı")
                onClicked: if (subtitles.config) subtitles.config.coverOriginal = !subsPanel.cfg.coverOriginal
            }
            DrawerItem {
                text: qsTr("İngilizcesini de göster")
                glyph: "subtitles"
                value: subsPanel.cfg.showSource ? qsTr("Açık") : qsTr("Kapalı")
                onClicked: if (subtitles.config) subtitles.config.showSource = !subsPanel.cfg.showSource
            }
            DrawerItem {
                text: qsTr("Düzelt")
                glyph: "chat"
                enabled: (subtitles.text || "").length > 0
                onClicked: subtitles.openCorrection()
            }
            // The translated line on a phone beside the screen (SubtitleScreen: a page on the home network)
            DrawerItem {
                text: qsTr("Telefonda göster")
                glyph: "display"
                opens: true
                badge: subtitleScreen.clients
                value: subtitleScreen.running && subtitleScreen.clients === 0 ? qsTr("Açık") : ""
                onClicked: menu.section = "subsphone"
            }
            GroupLabel { Layout.topMargin: 18; text: qsTr("Yazı boyutu") }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                MenuSlider {
                    id: subSizeSlider
                    Layout.fillWidth: true
                    from: 0.5
                    to: 2
                    stepSize: 0.1
                    value: subsPanel.cfg.fontRel !== undefined ? subsPanel.cfg.fontRel : 1.0
                    onMoved: if (subtitles.config) subtitles.config.fontRel = value
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.preferredWidth: 56
                    text: "%" + (subSizeSlider.value * 100).toFixed(0)
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.text
                }
            }
            Item { Layout.fillHeight: true }
        }
    }
    readonly property Item subsFirst: subOnRow

    // ── Telefonda altyazı: the QR code of the page; the phone gets every line the moment it changes ──
    FocusScope {
        id: phonePanel
        visible: menu.phoneShown
        opacity: menu.panelFx
        transform: Translate { x: (1 - menu.panelFx) * menu.panelFrom }
        anchors.fill: parent
        Keys.onUpPressed: menu.stepFocus(phonePanel, false)
        Keys.onDownPressed: menu.stepFocus(phonePanel, true)
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 22
            anchors.topMargin: 24
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                OutlinePill {
                    id: phoneBack
                    glyph: "chevron-left"
                    text: qsTr("Altyazı")
                    onClicked: menu.section = "subs"
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: qsTr("Telefonda altyazı")
                    elide: Text.ElideRight
                    font.family: Tema.display
                    font.weight: Font.DemiBold
                    font.pixelSize: 20
                    font.letterSpacing: -0.3
                    color: Tema.text
                }
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                Layout.topMargin: 4
                wrapMode: Text.WordWrap
                text: qsTr("Telefondaki Oviora uygulamasında Altyazı ekranı'nı aç: bu bilgisayarı kendisi bulur, aşağıdaki kodu bir kez gir. Uygulaması olmayan bir telefonda QR kodu kamerayla okut. Telefon ile bu bilgisayar aynı ev ağında olmalı.")
                font.family: Tema.body
                font.pixelSize: 14
                lineHeight: 1.15
                color: Tema.muted
            }
            // The phone app's one-time code, in two groups of three
            Column {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 4
                visible: subtitleScreen.running && subtitleScreen.code.length === 6
                spacing: 2
                Text {
                    textFormat: Text.PlainText
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Eşleştirme kodu")
                    font.family: Tema.body
                    font.pixelSize: 13
                    color: Tema.muted
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: subtitleScreen.code.substring(0, 3) + " " + subtitleScreen.code.substring(3)
                    font.family: Tema.display
                    font.pixelSize: 38
                    font.weight: Font.DemiBold
                    font.letterSpacing: 4
                    color: Tema.text
                }
            }
            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 6
                visible: subtitleScreen.qr.length > 0
                width: Math.min(170, phonePanel.width - 60)
                height: width
                radius: 12
                color: "white"
                Image {
                    anchors.fill: parent
                    anchors.margins: 6
                    source: subtitleScreen.qr
                    sourceSize: Qt.size(width, height)
                    smooth: false
                }
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: subtitleScreen.url.length > 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WrapAnywhere
                text: subtitleScreen.url
                font.family: Tema.body
                font.pixelSize: 12
                color: Tema.faint
            }
            Row {
                Layout.alignment: Qt.AlignHCenter
                spacing: 8
                visible: subtitleScreen.running
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 9
                    height: 9
                    radius: 4.5
                    color: subtitleScreen.clients > 0 ? Tema.ok : Tema.warm
                }
                Text {
                    textFormat: Text.PlainText
                    text: subtitleScreen.clients > 0 ? qsTr("%1 telefon bağlı").arg(subtitleScreen.clients) : qsTr("Telefon bekleniyor")
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.text
                }
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: subtitleScreen.error.length > 0
                wrapMode: Text.WordWrap
                text: subtitleScreen.error
                font.family: Tema.body
                font.pixelSize: 14
                color: Tema.danger
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Windows izin sorarsa \"İzin ver\" de. Telefonun ekranı kararıyorsa sayfadaki \"Başlat\"a bas ya da ekran zaman aşımını uzat.")
                font.family: Tema.body
                font.pixelSize: 13
                lineHeight: 1.15
                color: Tema.faint
            }
            Item { Layout.fillHeight: true }
            // A new code and key: phones paired before must pair again
            DrawerItem {
                visible: subtitleScreen.running
                text: qsTr("Kodu yenile")
                glyph: "subtitles"
                onClicked: subtitleScreen.renewCode()
            }
            DrawerItem {
                text: subtitleScreen.running ? qsTr("Paylaşımı kapat") : qsTr("Paylaşımı aç")
                glyph: subtitleScreen.running ? "close" : "display"
                tone: subtitleScreen.running ? Tema.danger : Tema.text
                onClicked: subtitleScreen.running ? subtitleScreen.stop() : subtitleScreen.start()
            }
        }
    }

    // ── İzlet: the stream watched live by friends in the browser (IzletController, context property "izlet") ──
    property bool izletTellFriends: true
    FocusScope {
        id: izletPanel
        visible: menu.izletShown
        opacity: menu.panelFx
        transform: Translate { x: (1 - menu.panelFx) * menu.panelFrom }
        anchors.fill: parent
        readonly property bool on: izlet.state === "canli" || izlet.state === "aciliyor"
        Keys.onUpPressed: menu.stepFocus(izletPanel, false)
        Keys.onDownPressed: menu.stepFocus(izletPanel, true)
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 22
            anchors.topMargin: 24
            spacing: 12
            SectionHeader { Layout.fillWidth: true; title: qsTr("İzlet") }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                Layout.topMargin: 4
                wrapMode: Text.WordWrap
                text: qsTr("Oynadığını arkadaşların ve grup üyelerin tarayıcıdan canlı izlesin. Görüntü bir kez, yeniden sıkıştırılmadan Oviora sunucusuna gider; en çok 20 kişi izleyebilir. Kaydedilmez.")
                font.family: Tema.body
                font.pixelSize: 14
                lineHeight: 1.15
                color: Tema.muted
            }
            // State: a dot and a word, the watchers' count and the rate going out to them
            Row {
                Layout.topMargin: 4
                spacing: 8
                visible: izletPanel.on
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 9
                    height: 9
                    radius: 4.5
                    color: izlet.state === "canli" ? Tema.danger : Tema.warm
                }
                Text {
                    textFormat: Text.PlainText
                    text: izlet.state === "canli"
                          ? (izlet.viewers.length > 0 ? qsTr("Yayında · %1 izleyici").arg(izlet.viewers.length) : qsTr("Yayında · izleyici bekleniyor"))
                          : qsTr("Yayın açılıyor…")
                    font.family: Tema.body
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Tema.text
                }
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: izletPanel.on
                wrapMode: Text.WrapAnywhere
                text: qsTr("İzleme adresi: %1").arg(izlet.watchUrl)
                font.family: Tema.body
                font.pixelSize: 13
                color: Tema.faint
            }
            // Who watches: a name and "Çıkar"
            Repeater {
                model: izletPanel.on ? izlet.viewers : []
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 10
                    Ikon {
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        name: "eye"
                        color: Tema.muted
                    }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: modelData.name
                        elide: Text.ElideRight
                        font.family: Tema.body
                        font.pixelSize: 15
                        color: Tema.text
                    }
                    OutlinePill {
                        text: qsTr("Çıkar")
                        glyph: "close"
                        onClicked: izlet.kick(modelData.name)
                    }
                }
            }
            // The upload falls short: viewers' picture holds now and then (the server waits for a key frame)
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: izletPanel.on && izlet.lost > 0
                wrapMode: Text.WordWrap
                text: qsTr("İnternetinin yükleme hızı bu yayına yetmiyor (%1 Mbit/s gidiyor, paket kayboluyor): izleyicilerde görüntü bir an donabilir. PS5 bağlantı ayarlarında bit hızını düşürürsen düzelir.").arg((izlet.uploadKbps / 1000).toFixed(1))
                font.family: Tema.body
                font.pixelSize: 13
                lineHeight: 1.15
                color: Tema.warm
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: izlet.codec === "h265"
                wrapMode: Text.WordWrap
                text: qsTr("Bu yayın H.265: telefonda Safari ve güncel Chrome açar; bazı tarayıcılar (Firefox gibi) açamayabilir. Herkes izleyebilsin istersen PS5 bağlantı ayarlarından H.264'e geç (bir sonraki bağlantıda).")
                font.family: Tema.body
                font.pixelSize: 13
                lineHeight: 1.15
                color: Tema.faint
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: izlet.error.length > 0
                wrapMode: Text.WordWrap
                text: izlet.error
                font.family: Tema.body
                font.pixelSize: 14
                color: Tema.danger
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: !izlet.available
                wrapMode: Text.WordWrap
                text: qsTr("Bu sürümde İzlet yok.")
                font.family: Tema.body
                font.pixelSize: 14
                color: Tema.muted
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: izlet.available && !izletPanel.on && !izlet.streaming
                wrapMode: Text.WordWrap
                text: qsTr("Yayın, PS5'te oynarken açılabilir.")
                font.family: Tema.body
                font.pixelSize: 14
                color: Tema.muted
            }
            Item { Layout.fillHeight: true }
            DrawerItem {
                id: izletTellRow
                visible: !izletPanel.on
                text: qsTr("Arkadaşlarına haber ver")
                glyph: "bell"
                value: menu.izletTellFriends ? qsTr("Açık") : qsTr("Kapalı")
                onClicked: menu.izletTellFriends = !menu.izletTellFriends
            }
            DrawerItem {
                id: izletToggle
                text: izletPanel.on ? qsTr("Yayını kapat") : qsTr("Yayını aç")
                glyph: izletPanel.on ? "close" : "broadcast"
                tone: izletPanel.on ? Tema.danger : Tema.text
                enabled: izlet.available && (izletPanel.on || izlet.streaming)
                onClicked: izletPanel.on ? izlet.stop() : izlet.start(menu.izletTellFriends)
            }
        }
    }
    readonly property Item izletFirst: izletToggle

    // ── The picture section: every picture setting of the engine ──
    FocusScope {
        id: picturePanel
        visible: menu.pictureShown
        opacity: menu.panelFx
        transform: Translate { x: (1 - menu.panelFx) * menu.panelFrom }
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: 14
            anchors.bottomMargin: 2
            anchors.leftMargin: 2
            anchors.rightMargin: 2
            spacing: 0

            SectionHeader {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                title: qsTr("Görüntü")
            }

            // The quick choices first: how the picture fits, and the quality presets
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 14
                Layout.bottomMargin: 8
                spacing: 8
                GroupLabel { text: qsTr("Görünüm") }
                // The picture's fit: aspectMode 0 fit, 2 fill and crop, 3 stretch
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    MenuPill {
                        id: normalPill
                        text: qsTr("Normal")
                        selected: oviora.aspectMode === 0
                        onClicked: oviora.aspectMode = 0
                    }
                    MenuPill {
                        text: qsTr("Yakınlaştır")
                        selected: oviora.aspectMode === 2
                        onClicked: oviora.aspectMode = selected ? 0 : 2
                    }
                    MenuPill {
                        text: qsTr("Uzat")
                        selected: oviora.aspectMode === 3
                        onClicked: oviora.aspectMode = selected ? 0 : 3
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: oviora.aspectMode === 2
                    spacing: 10
                    GroupLabel { text: qsTr("Yakınlık") }
                    MenuSlider {
                        id: zoomSlider
                        Layout.fillWidth: true
                        from: 1.0
                        to: 5.0
                        stepSize: 0.05
                        value: oviora.viewZoom
                        onMoved: oviora.viewZoom = value
                    }
                    Text {
                        textFormat: Text.PlainText
                        Layout.preferredWidth: 104
                        text: zoomSlider.value <= 1.001 ? qsTr("Siyah bant yok") : zoomSlider.value.toFixed(2) + "×"
                        font.family: Tema.body
                        font.pixelSize: 15
                        color: Tema.text
                    }
                }
                GroupLabel { Layout.topMargin: 6; text: qsTr("Kalite") }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    MenuPill {
                        text: qsTr("Standart")
                        selected: menu.presetStandard
                        onClicked: menu.applyQualityPreset("standard")
                    }
                    MenuPill {
                        text: qsTr("Yüksek")
                        selected: menu.presetHigh
                        enabled: menu.sr3Available
                        note: menu.sr3Cap.reason || ""
                        onClicked: menu.applyQualityPreset("high")
                    }
                    MenuPill {
                        text: qsTr("Yüksek + keskin")
                        selected: menu.presetHighSharp
                        enabled: menu.sr3Available && menu.nrAvailable
                        note: !menu.sr3Available ? (menu.sr3Cap.reason || "") : (menu.nrCap.reason || "")
                        onClicked: menu.applyQualityPreset("highSharp")
                    }
                    MenuPill {
                        text: qsTr("En keskin")
                        selected: menu.presetSharpest
                        enabled: menu.sr4Available && menu.nrAvailable
                        note: !menu.sr4Available ? (menu.sr4Cap.reason || "") : (menu.nrCap.reason || "")
                        onClicked: menu.applyQualityPreset("sharpest")
                    }
                    MenuPill {
                        text: qsTr("Özel")
                        selected: menu.presetCustom
                        onClicked: menu.applyQualityPreset("custom")
                    }
                }
                Rectangle { Layout.fillWidth: true; Layout.topMargin: 8; Layout.preferredHeight: 1; color: Tema.line }
            }

            // The professional page in its quick-adjust form: only its settings column
            Item {
                id: pictureHost
                Layout.fillWidth: true
                Layout.fillHeight: true
                Loader {
                    id: pictureLoader
                    anchors.fill: parent
                    active: false
                    Connections {
                        target: menu
                        function onVisibleChanged() { if (menu.visible && menu.pictureShown) pictureLoader.active = true }
                        function onPictureShownChanged() { if (menu.visible && menu.pictureShown) pictureLoader.active = true }
                    }
                    Component.onCompleted: if (menu.visible && menu.pictureShown) active = true
                    sourceComponent: ProPage {
                        pageId: "streammenu"
                        home: pictureHost
                        embedded: true
                        dragStripHeight: 0
                    }
                    onLoaded: item.tab = menu.pictureTab
                }
            }
        }
    }

    // ── The keys section: Ayarlar > Tuşlar over the picture (no key goes to the game while the menu is open) ──
    FocusScope {
        id: keysPanel
        visible: menu.keysShown
        opacity: menu.panelFx
        transform: Translate { x: (1 - menu.panelFx) * menu.panelFrom }
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: 14
            anchors.bottomMargin: 2
            anchors.leftMargin: 2
            anchors.rightMargin: 2
            spacing: 0

            SectionHeader {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                title: qsTr("Klavye ve fare")
            }
            // Off: in the stream the keyboard and the mouse send nothing to the console
            DrawerItem {
                id: klavyeFareRow
                readonly property bool active: oviora.preferences.keyboardMouse !== false
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.topMargin: 12
                text: qsTr("Klavye ve fareyle oyna")
                glyph: "keyboard"
                value: active ? qsTr("Açık") : qsTr("Kapalı")
                onClicked: oviora.setPreference("keyboardMouse", !active)
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: 6
                clip: true
                Loader {
                    id: keysLoader
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 6
                    active: menu.visible && menu.keysShown
                    sourceComponent: S.KeysPage {
                        onConfirmRequested: (title, text, action) => keysConfirm.ask(title, text, action)
                    }
                }
            }
        }

        // The page's "are you sure" inside the panel (a popup would fall outside the menu's hole in the video)
        Rectangle {
            id: keysConfirm
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.55)
            visible: false
            property var action: null
            property string title: ""
            property string text: ""
            function ask(t, x, a) { title = t; text = x; action = a; visible = true; keysConfirmNo.forceActiveFocus() }
            function close() { visible = false; action = null; keysPanel.forceActiveFocus() }
            MouseArea { anchors.fill: parent }
            Keys.onEscapePressed: close()
            Rectangle {
                anchors.centerIn: parent
                width: Math.min(460, parent.width - 48)
                height: confirmColumn.implicitHeight + 48
                radius: Tema.radiusPanel
                color: Tema.surface
                border.color: Tema.lineStrong
                ColumnLayout {
                    id: confirmColumn
                    anchors.fill: parent
                    anchors.margins: 24
                    spacing: 14
                    Label { textFormat: Text.PlainText; text: keysConfirm.title; font.family: Tema.display; font.pixelSize: 22; color: Tema.text; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    Label { textFormat: Text.PlainText; text: keysConfirm.text; font.family: Tema.body; font.pixelSize: Tema.bodySize; color: Tema.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    RowLayout {
                        Layout.alignment: Qt.AlignRight
                        spacing: 8
                        S.PillButton { id: keysConfirmNo; text: qsTr("Vazgeç"); onClicked: keysConfirm.close() }
                        S.PillButton { kind: "primary"; text: qsTr("Evet"); onClicked: { const a = keysConfirm.action; keysConfirm.close(); if (a) a(); } }
                    }
                }
            }
        }
    }

    // ── The chat section: friends and their conversations over the picture. Drawn inside the panel
    // only (a popup would fall outside the menu's hole in the video); no key goes to the game while
    // the menu is open, and GameInput skips text fields as well ──
    FocusScope {
        id: chatPanel
        visible: menu.chatShown
        opacity: menu.panelFx
        transform: Translate { x: (1 - menu.panelFx) * menu.panelFrom }
        anchors.fill: parent
        // --test-groups: made-up friends and groups while signed out (screenshots)
        readonly property bool signedIn: platform.status == "active" || platform.testGroups === true
        readonly property var friends: signedIn ? (platform.friends || []) : []
        readonly property var groups: signedIn ? (platform.groups || []) : []
        readonly property bool streamerMode: oviora.preferences.streamerMode === true
        // The list: my groups first, then the friends (kind "group" / "friend"), each part newest conversation first
        readonly property var entries: {
            const last = messages.lastMessages || ({})
            const when = key => (last[key] && last[key].at) || 0
            const newest = (a, b) => (when(b.key) - when(a.key)) || a.name.localeCompare(b.name)
            return groups.map(g => ({ kind: "group", id: g.id, name: g.name, group: g, key: "#" + g.id })).sort(newest)
                .concat(friends.map(f => ({ kind: "friend", id: 0, name: f.name, presence: f.presence, key: (f.name || "").toLowerCase() })).sort(newest))
        }
        // The conversation's newest message ({text, at, mine, failed}) or null
        function lastOf(e) { return e && messages.lastMessages ? (messages.lastMessages[e.key] || null) : null }
        // Someone typing there now (a group: anyone in it)
        function typingIn(e) {
            const now = messages.typingNow || []
            if (!e) return false
            if (e.kind === "group") return now.some(k => k.indexOf("#" + e.id + "|") === 0)
            return now.indexOf(e.key) >= 0
        }
        // "14:05" today, "Dün", else "12.11"
        function shortTime(ms) {
            const d = new Date(ms), today = new Date()
            if (d.toDateString() === today.toDateString()) return Qt.formatDateTime(d, "HH:mm")
            const y = new Date(today.getTime() - 86400000)
            if (d.toDateString() === y.toDateString()) return qsTr("Dün")
            return Qt.formatDateTime(d, "dd.MM")
        }
        function initials(name) {
            const s = (name || "").trim()
            return s.length ? s.substring(0, Math.min(2, s.length)).toLocaleUpperCase(Qt.locale("tr_TR")) : "?"
        }
        function unreadOf(name) { return (messages.unread && name && messages.unread[name.toLowerCase()]) || 0 }
        function groupUnreadOf(id) { return (messages.groupUnread && messages.groupUnread[String(id)]) || 0 }
        // The friends' line as on the home screen: "Oynuyor: GTA VI · Ahmet'in PS5'inde", "Çevrimdışı · 2 sa önce"
        // The times in it ("25 dk") are read again every minute: the account is only re-sent when something changed
        property int presenceTick: 0
        Timer { interval: 60000; running: chatPanel.visible; repeat: true; onTriggered: chatPanel.presenceTick++ }
        function presenceLine(p) { chatPanel.presenceTick; return p ? platform.presenceText(p, streamerMode) : "" }
        function presenceOn(p) { return !!p && (p.state === "playing" || p.state === "online") }
        // "5 kişi · 2 kişi oynuyor"
        function groupSummary(g) {
            const n = (g && g.members) ? g.members.length : 0
            return g && g.playing > 0 ? qsTr("%1 kişi").arg(n) + " · " + qsTr("%1 kişi oynuyor").arg(g.playing) : qsTr("%1 kişi").arg(n)
        }
        function openEntry(e) {
            if (!e) return
            if (e.kind === "group") menu.showGroup(e.id)
            else menu.showConversation(e.name)
        }
        // The open friend's entry (presence) from the account
        readonly property var peer: {
            const want = menu.chatPeer.toLowerCase()
            for (const f of friends) if (f && f.name && f.name.toLowerCase() === want) return f
            return ({ name: menu.chatPeer, online: false })
        }
        // The open group from the groups view
        readonly property var openGroup: {
            for (const g of groups) if (g && g.id === menu.chatGroup) return g
            return ({ name: messages.chatTitle, members: [] })
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: 14
            anchors.bottomMargin: 18
            anchors.leftMargin: 2
            anchors.rightMargin: 2
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                spacing: 12
                OutlinePill {
                    glyph: "chevron-left"
                    text: qsTr("Menü")
                    onClicked: menu.section = "game"
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: qsTr("Mesajlar")
                    elide: Text.ElideRight
                    font.family: Tema.display
                    font.weight: Font.DemiBold
                    font.pixelSize: 20
                    font.letterSpacing: -0.3
                    color: Tema.text
                }
                AbstractButton {
                    id: chatClose
                    focusPolicy: Qt.StrongFocus
                    hoverEnabled: true
                    implicitWidth: 40
                    implicitHeight: 40
                    onClicked: menu.hide()
                    background: Rectangle {
                        radius: 20
                        color: chatClose.hovered || chatClose.activeFocus ? Tema.surface2 : "transparent"
                        border.width: chatClose.activeFocus ? 2 : 0
                        border.color: Tema.accent
                    }
                    contentItem: Item {
                        Ikon {
                            anchors.centerIn: parent
                            width: 18
                            height: 18
                            name: "close"
                            color: chatClose.hovered ? Tema.text : Tema.muted
                        }
                    }
                }
            }

            // ── My groups, then the friends: a tap opens the conversation; Up/Down and Enter work too ──
            ListView {
                id: friendList
                visible: !menu.chatInConversation
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: 12
                Layout.leftMargin: 10
                Layout.rightMargin: 10
                clip: true
                spacing: 2
                focus: true
                keyNavigationEnabled: true
                model: chatPanel.entries
                highlightMoveDuration: 160
                Keys.onReturnPressed: if (currentIndex >= 0) chatPanel.openEntry(chatPanel.entries[currentIndex])
                Keys.onEnterPressed: if (currentIndex >= 0) chatPanel.openEntry(chatPanel.entries[currentIndex])
                // "Gruplar" / "Arkadaşlar" over the two parts, only when there are groups
                section.property: chatPanel.groups.length > 0 ? "kind" : ""
                section.delegate: Text {
                    required property string section
                    textFormat: Text.PlainText
                    width: ListView.view.width
                    leftPadding: 8
                    topPadding: section === "group" ? 2 : 14
                    bottomPadding: 4
                    text: section === "group" ? qsTr("Gruplar") : qsTr("Arkadaşlar")
                    font.family: Tema.body
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Tema.muted
                }
                delegate: AbstractButton {
                    id: friendRow
                    required property var modelData
                    required property int index
                    readonly property bool isGroup: modelData.kind === "group"
                    readonly property int unread: isGroup ? chatPanel.groupUnreadOf(modelData.id) : chatPanel.unreadOf(modelData.name)
                    readonly property bool lit: isGroup ? modelData.group.playing > 0 : chatPanel.presenceOn(modelData.presence)
                    readonly property bool current: ListView.isCurrentItem && friendList.activeFocus
                    readonly property var last: chatPanel.lastOf(modelData)
                    readonly property bool typing: chatPanel.typingIn(modelData)
                    width: ListView.view.width
                    implicitHeight: 60
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: true
                    onClicked: {
                        friendList.currentIndex = index
                        chatPanel.openEntry(modelData)
                    }
                    background: Rectangle {
                        radius: 12
                        color: friendRow.down ? Tema.line : (friendRow.hovered || friendRow.current ? Tema.surface2 : "transparent")
                        border.width: friendRow.current ? 2 : 0
                        border.color: Tema.accent
                    }
                    contentItem: RowLayout {
                        spacing: 12
                        Item { implicitWidth: 2 }
                        // A circle for a friend, a rounded square for a group
                        Rectangle {
                            implicitWidth: 34
                            implicitHeight: 34
                            radius: friendRow.isGroup ? 10 : 17
                            color: Tema.surface2
                            border.color: Tema.line
                            Text {
                                textFormat: Text.PlainText
                                anchors.centerIn: parent
                                text: chatPanel.initials(friendRow.modelData.name)
                                font.family: Tema.body
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                color: Tema.text
                            }
                            // playing or online (a group: someone in it plays): a small green light on the circle
                            Rectangle {
                                visible: friendRow.lit
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: -1
                                width: 11
                                height: 11
                                radius: 5.5
                                color: Tema.ok
                                border.width: 2
                                border.color: Tema.surface
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: friendRow.modelData.name
                                    elide: Text.ElideRight
                                    font.family: Tema.body
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                    color: Tema.text
                                }
                                Text {
                                    textFormat: Text.PlainText
                                    visible: !!friendRow.last
                                    text: friendRow.last ? chatPanel.shortTime(friendRow.last.at) : ""
                                    font.family: Tema.body
                                    font.pixelSize: 12
                                    color: friendRow.unread > 0 ? Tema.accentText : Tema.faint
                                }
                            }
                            // "yazıyor…", else the newest message ("Sen: …" for mine), else presence / the group
                            Text {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: friendRow.typing ? qsTr("yazıyor…")
                                    : friendRow.last ? (friendRow.last.mine ? qsTr("Sen: ") : "") + friendRow.last.text.replace(/\s+/g, " ")
                                    : friendRow.isGroup ? chatPanel.groupSummary(friendRow.modelData.group) : chatPanel.presenceLine(friendRow.modelData.presence)
                                elide: Text.ElideRight
                                font.family: Tema.body
                                font.pixelSize: 13
                                font.italic: friendRow.typing
                                font.weight: friendRow.unread > 0 && !friendRow.typing ? Font.DemiBold : Font.Normal
                                color: friendRow.typing ? Tema.accentText : friendRow.unread > 0 ? Tema.text
                                    : (!friendRow.last && friendRow.lit) ? Tema.ok : Tema.muted
                                Behavior on color { ColorAnimation { duration: 200 } }
                            }
                        }
                        // Unread messages: a count in the brand colour
                        Rectangle {
                            visible: friendRow.unread > 0
                            scale: visible ? 1 : 0.4
                            Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }
                            implicitWidth: Math.max(26, unreadText.implicitWidth + 14)
                            implicitHeight: 26
                            radius: 13
                            color: Tema.accent
                            Text {
                                textFormat: Text.PlainText
                                id: unreadText
                                anchors.centerIn: parent
                                text: String(friendRow.unread)
                                font.family: Tema.body
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                color: Tema.inkOnAccent
                            }
                        }
                        Item { implicitWidth: 4 }
                    }
                }
                // Nobody to write to yet: signed out, or no friends on the account
                ColumnLayout {
                    anchors.centerIn: parent
                    width: parent.width - 48
                    visible: friendList.count === 0
                    spacing: 8
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: chatPanel.signedIn ? qsTr("Henüz arkadaşın yok") : qsTr("Mesajlar için giriş yap")
                        font.family: Tema.display
                        font.weight: Font.DemiBold
                        font.pixelSize: 20
                        color: Tema.text
                    }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: chatPanel.signedIn
                            ? qsTr("Arkadaşlarını ana ekrandan ekleyebilirsin; sonra oyundan çıkmadan buradan yazışırsınız.")
                            : qsTr("Ana ekranda Oviora hesabına giriş yaptığında arkadaşlarınla oyundan çıkmadan buradan yazışabilirsin.")
                        font.family: Tema.body
                        font.pixelSize: 14
                        lineHeight: 1.15
                        color: Tema.muted
                    }
                }
            }

            // ── One conversation: back to the list, the friend or group, the warnings, the bubbles ──
            ColumnLayout {
                visible: menu.chatInConversation
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: 12
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    OutlinePill {
                        glyph: "chevron-left"
                        text: qsTr("Arkadaşlar")
                        keyHint: "Esc"
                        onClicked: menu.showConversation("")
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: menu.chatGroup ? messages.chatTitle : menu.chatPeer
                            elide: Text.ElideRight
                            font.family: Tema.display
                            font.weight: Font.DemiBold
                            font.pixelSize: 18
                            color: Tema.text
                        }
                        Text {
                            readonly property bool lit: menu.chatGroup ? chatPanel.openGroup.playing > 0
                                : chatPanel.peer.presence ? chatPanel.presenceOn(chatPanel.peer.presence) : !!chatPanel.peer.online
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: menu.chatGroup ? chatPanel.groupSummary(chatPanel.openGroup)
                                : chatPanel.peer.presence ? chatPanel.presenceLine(chatPanel.peer.presence)
                                : (chatPanel.peer.online ? qsTr("çevrimiçi") : qsTr("çevrimdışı"))
                            elide: Text.ElideRight
                            font.family: Tema.body
                            font.pixelSize: 13
                            color: lit ? Tema.ok : Tema.muted
                        }
                    }
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: menu.chatGroup
                        ? qsTr("Uçtan uca şifreli: mesajları yalnız grubun üyeleri okuyabilir, Oviora sunucusu bile göremez.")
                        : qsTr("Uçtan uca şifreli: mesajları yalnız sen ve %1 okuyabilir, Oviora sunucusu bile göremez.").arg(menu.chatPeer)
                    font.family: Tema.body
                    font.pixelSize: 13
                    color: Tema.faint
                }
                // A changed key: no safety code here; it is compared in the home screen's conversation
                Rectangle {
                    visible: messages.chatKeyChanged
                    Layout.fillWidth: true
                    implicitHeight: chatKeyWarn.implicitHeight + 20
                    radius: 8
                    color: "transparent"
                    border.color: Tema.warm
                    Text {
                        textFormat: Text.PlainText
                        id: chatKeyWarn
                        anchors.fill: parent
                        anchors.margins: 10
                        wrapMode: Text.WordWrap
                        text: menu.chatGroup
                            ? qsTr("Grupta %1 için güvenlik anahtarı değişti. Yeni bir cihaz kurmuş olabilir; emin olmak için güvenlik kodunu onunla birebir sohbetinizde karşılaştır.").arg(messages.chatKeyChangedNames.join(", "))
                            : qsTr("%1 için güvenlik anahtarı değişti. Yeni bir cihaz kurmuş olabilir; emin olmak için güvenlik kodunu ana ekrandaki sohbetten onunla karşılaştır.").arg(menu.chatPeer)
                        font.family: Tema.body
                        font.pixelSize: 13
                        color: Tema.warm
                    }
                }
                ChatView {
                    id: chatConversation
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
            }
        }
    }
}
