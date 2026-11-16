// Oviora's stream menu (Ctrl+O), ported from the old Oviora's StreamView menu: a floating panel near
// the bottom of the picture with the console and its live numbers, the actions as big tiles, and
// the volume. "Görüntü ayarları" turns the same window into a tall panel on the right with the
// engine's picture settings (the professional page's settings column).
// A native owned window: the D3D12 video window cannot cover it.
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Item {
    id: menu
    required property Window owner
    // The native video window leaves a hole under the panel (main.cpp syncVideoCovers)
    readonly property bool videoCover: visible && opacity > 0
    readonly property real coverRadius: Tema.radiusPanel
    visible: false
    opacity: shown ? 1 : 0
    property bool shown: false
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    onOpacityChanged: if (opacity === 0 && !shown) visible = false
    signal requestFullscreen()
    // "game": the floating panel; "picture": the picture settings
    property string section: "game"
    // The settings tab the picture section opens on (ProPage.tab)
    property string pictureTab: "quality"


    // Laid out at 1240 x 344 like the old menu and scaled down to fit narrower windows
    readonly property int menuDesignWidth: 1240
    // The subtitle row adds one line (old menu: 344 -> 398 with subtitles)
    readonly property int menuDesignHeight: subtitles.available ? 398 : 344
    readonly property real menuScale: Math.max(0.5, Math.min(1, (owner.width - 48) / menuDesignWidth))
    readonly property int menuMargin: Math.round(24 * menuScale)
    readonly property bool pictureShown: section === "picture"
    width: pictureShown ? Math.max(360, Math.min(460, owner.width - 48)) : Math.round(menuDesignWidth * menuScale)
    height: pictureShown ? Math.max(360, owner.height - 2 * menuMargin) : Math.round(menuDesignHeight * menuScale)

    // Positions inside the main window (the owner's content)
    function place() {
        if (pictureShown) {
            x = owner.width - width - menuMargin
            y = menuMargin
        } else {
            x = Math.round((owner.width - width) / 2)
            y = owner.height - height - menuMargin
        }
    }
    function show() { place(); visible = true; shown = true }
    function hide() {
        shown = false
        section = "game"
        // Keys go back to the window (fullscreen, Ctrl+O): nothing in the hidden menu keeps them
        if (owner && owner.contentItem) owner.contentItem.forceActiveFocus()
    }
    function open() { show(); mainPanel.forceActiveFocus() }
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
    onWidthChanged: if (visible) place()
    onHeightChanged: if (visible) place()
    onSectionChanged: {
        if (visible) place()
        if (!pictureShown) mainPanel.forceActiveFocus()
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
            if (oviora.hasSource) menu.sawStream = true
            else if (menu.sawStream) { menu.sawStream = false; menu.hide() }
        }
    }
    property bool sawStream: false
    Shortcut { sequence: "Escape"; enabled: menu.shown; onActivated: menu.hide() }

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
        "chevron-left": "M15 5.5l-6.5 6.5l6.5 6.5"
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

    component GroupLabel: Text {
        font.family: Tema.body
        font.pixelSize: 13
        color: Tema.muted
    }

    // ── The floating panel ──
    FocusScope {
        id: mainPanel
        visible: !menu.pictureShown
        property Item initialFocusItem: screenshotTile
        width: menu.menuDesignWidth
        height: menu.menuDesignHeight
        scale: menu.menuScale
        transformOrigin: Item.TopLeft
        focus: true

        // Arrow keys / d-pad move between the controls by where they sit on screen
        readonly property list<Item> navItems: [
            hideMenuButton,
            micTile, screenshotTile, normalTile, zoomTile, stretchTile, statsTile, displayTile, imageTile, sleepTile, disconnectTile,
            volumeSlider, picturePill, presetStandardPill, presetHighPill, presetHighSharpPill, presetSharpestPill, presetCustomPill, zoomSlider,
            subOnPill, subRegionPill, subCoverPill, subFixPill, subSourcePill, subSizeSlider,
            klavyeFarePill
        ]
        function navigate(key) {
            const current = mainPanel.Window.activeFocusItem
            const items = []
            for (const it of navItems) {
                if (it.visible && it.enabled)
                    items.push(it)
            }
            if (items.indexOf(current) < 0) {
                initialFocusItem.forceActiveFocus(Qt.TabFocusReason)
                return
            }
            const c = current.mapToItem(mainPanel, current.width / 2, current.height / 2)
            let best = null
            let bestScore = Infinity
            for (const it of items) {
                if (it === current)
                    continue
                const p = it.mapToItem(mainPanel, it.width / 2, it.height / 2)
                const dx = p.x - c.x
                const dy = p.y - c.y
                let score
                if (key === Qt.Key_Left || key === Qt.Key_Right) {
                    // stay on the same row
                    const main = key === Qt.Key_Left ? -dx : dx
                    if (main <= 4 || Math.abs(dy) > 30)
                        continue
                    score = main
                } else {
                    const main = key === Qt.Key_Up ? -dy : dy
                    if (main <= 30)
                        continue
                    score = main + 2 * Math.abs(dx)
                }
                if (score < bestScore) {
                    bestScore = score
                    best = it
                }
            }
            if (best)
                best.forceActiveFocus(Qt.TabFocusReason)
        }
        Keys.onPressed: event => {
            switch (event.key) {
            case Qt.Key_Left:
            case Qt.Key_Right:
            case Qt.Key_Up:
            case Qt.Key_Down:
                navigate(event.key)
                event.accepted = true
                break
            case Qt.Key_Menu:
            case Qt.Key_Back:
                menu.hide()
                event.accepted = true
                break
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: Tema.radiusPanel
            color: Tema.surface
            border.width: 1
            border.color: Tema.line
        }

        ColumnLayout {
            anchors {
                fill: parent
                leftMargin: 24
                rightMargin: 20
                topMargin: 18
                bottomMargin: 18
            }
            spacing: 14

            // ── The console and the connection ──
            RowLayout {
                Layout.fillWidth: true
                spacing: 14

                KonsolSimge {
                    Layout.preferredWidth: 56
                    Layout.preferredHeight: 16
                    tone: !oviora.hasSource && !menu.ps5Active ? Tema.faint : (menu.connected ? Tema.ok : Tema.warm)
                }
                ColumnLayout {
                    spacing: 0
                    Text {
                        textFormat: Text.PlainText
                        text: menu.streamTitle
                        font.family: Tema.display
                        font.weight: Font.DemiBold
                        font.pixelSize: 22
                        font.letterSpacing: -0.3
                        color: Tema.text
                        elide: Text.ElideRight
                        Layout.maximumWidth: 360
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

                Item { Layout.fillWidth: true }

                Row {
                    spacing: 26
                    visible: menu.statsShown
                    Layout.rightMargin: 12
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

                OutlinePill {
                    id: hideMenuButton
                    glyph: "chevron-down"
                    text: qsTr("Menüyü kapat")
                    keyHint: "Esc"
                    onClicked: menu.hide()
                }
            }

            // ── Tiles: sound, screen, settings, and leaving ──
            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                // The microphone to the PS5 (oviora.micOn / toggleMicrophone)
                MenuTile {
                    id: micTile
                    readonly property bool micOn: oviora.micOn
                    text: micOn ? qsTr("Mikrofon açık") : qsTr("Mikrofon kapalı")
                    glyph: micOn ? "mic" : "mic-off"
                    selected: micOn && enabled
                    enabled: menu.statsShown
                    onClicked: oviora.toggleMicrophone()
                }
                MenuTile {
                    id: screenshotTile
                    text: qsTr("Ekran görüntüsü")
                    glyph: "camera"
                    enabled: oviora.hasSource
                    onClicked: oviora.takeScreenshot()
                }

                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 72; Layout.leftMargin: 4; Layout.rightMargin: 4; color: Tema.line }

                // The picture's fit: aspectMode 0 fit, 2 fill and crop, 3 stretch
                MenuTile {
                    id: normalTile
                    text: qsTr("Normal")
                    glyph: "fit"
                    selected: oviora.aspectMode === 0
                    onClicked: oviora.aspectMode = 0
                }
                MenuTile {
                    id: zoomTile
                    text: qsTr("Yakınlaştır")
                    glyph: "zoom"
                    selected: oviora.aspectMode === 2
                    onClicked: oviora.aspectMode = selected ? 0 : 2
                }
                MenuTile {
                    id: stretchTile
                    text: qsTr("Uzat")
                    glyph: "stretch"
                    selected: oviora.aspectMode === 3
                    onClicked: oviora.aspectMode = selected ? 0 : 3
                }

                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 72; Layout.leftMargin: 4; Layout.rightMargin: 4; color: Tema.line }

                // The statistics over the picture (preferences.showStreamStats; Main.qml draws them)
                MenuTile {
                    id: statsTile
                    text: qsTr("İstatistikler")
                    glyph: "stats"
                    selected: oviora.preferences.showStreamStats === true
                    onClicked: oviora.setPreference("showStreamStats", !selected)
                }
                MenuTile {
                    id: displayTile
                    text: qsTr("Ekran ayarları")
                    glyph: "display"
                    onClicked: menu.openPicture("display")
                }
                MenuTile {
                    id: imageTile
                    text: qsTr("Görüntü ayarları")
                    glyph: "image"
                    onClicked: menu.openPicture("quality")
                }

                Item { Layout.fillWidth: true }

                MenuTile {
                    id: sleepTile
                    text: qsTr("Dinlenmeye al")
                    Layout.minimumWidth: 112
                    glyph: "sleep"
                    tone: Tema.warm
                    enabled: menu.statsShown
                    onClicked: {
                        menu.hide()
                        oviora.ps5Rest()
                    }
                }
                MenuTile {
                    id: disconnectTile
                    text: qsTr("Bağlantıyı kes")
                    glyph: "close"
                    tone: Tema.danger
                    tinted: true
                    onClicked: {
                        menu.hide()
                        oviora.ps5Cancel()
                    }
                }
            }

            // ── Volume and the picture settings ──
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
                    Layout.preferredWidth: 180
                    from: 0
                    to: 1
                    stepSize: 0.02
                    value: oviora.volume
                    onMoved: oviora.volume = value
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.preferredWidth: 44
                    text: "%" + (volumeSlider.value * 100).toFixed(0)
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.text
                }

                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 28; Layout.leftMargin: 4; Layout.rightMargin: 4; color: Tema.line }

                GroupLabel { text: qsTr("Kalite") }
                Row {
                    spacing: 6
                    MenuPill {
                        id: presetStandardPill
                        text: qsTr("Standart")
                        selected: menu.presetStandard
                        onClicked: menu.applyQualityPreset("standard")
                    }
                    MenuPill {
                        id: presetHighPill
                        text: qsTr("Yüksek")
                        selected: menu.presetHigh
                        enabled: menu.sr3Available
                        note: menu.sr3Cap.reason || ""
                        onClicked: menu.applyQualityPreset("high")
                    }
                    MenuPill {
                        id: presetHighSharpPill
                        text: qsTr("Yüksek + keskin")
                        selected: menu.presetHighSharp
                        enabled: menu.sr3Available && menu.nrAvailable
                        note: !menu.sr3Available ? (menu.sr3Cap.reason || "") : (menu.nrCap.reason || "")
                        onClicked: menu.applyQualityPreset("highSharp")
                    }
                    MenuPill {
                        id: presetSharpestPill
                        text: qsTr("En keskin")
                        selected: menu.presetSharpest
                        enabled: menu.sr4Available && menu.nrAvailable
                        note: !menu.sr4Available ? (menu.sr4Cap.reason || "") : (menu.nrCap.reason || "")
                        onClicked: menu.applyQualityPreset("sharpest")
                    }
                    MenuPill {
                        id: presetCustomPill
                        text: qsTr("Özel")
                        selected: menu.presetCustom
                        onClicked: menu.applyQualityPreset("custom")
                    }
                }
                MenuPill {
                    id: picturePill
                    text: qsTr("Görüntü ayarları")
                    onClicked: menu.openPicture("quality")
                }

                Item { Layout.fillWidth: true }

                GroupLabel {
                    visible: oviora.aspectMode === 2
                    text: qsTr("Yakınlık")
                }
                MenuSlider {
                    id: zoomSlider
                    visible: oviora.aspectMode === 2
                    Layout.preferredWidth: 150
                    from: 1.0
                    to: 5.0
                    stepSize: 0.05
                    value: oviora.viewZoom
                    onMoved: oviora.viewZoom = value
                }
                Text {
                    textFormat: Text.PlainText
                    visible: oviora.aspectMode === 2
                    Layout.preferredWidth: 104
                    text: zoomSlider.value <= 1.001 ? qsTr("Siyah bant yok") : zoomSlider.value.toFixed(2) + "×"
                    font.family: Tema.body
                    font.pixelSize: 15
                    color: Tema.text
                }
            }

            // ── Subtitles (the same values as Ayarlar › Altyazı; Ctrl+T / Ctrl+B do the same) ──
            RowLayout {
                id: subRow
                Layout.fillWidth: true
                spacing: 12
                visible: subtitles.available
                readonly property var cfg: subtitles.config || ({})

                GroupLabel { text: qsTr("Altyazı") }
                Row {
                    spacing: 6
                    MenuPill {
                        id: subOnPill
                        text: subRow.cfg.enabled ? qsTr("Açık") : qsTr("Kapalı")
                        selected: !!subRow.cfg.enabled
                        onClicked: if (subtitles.config) subtitles.config.enabled = !subRow.cfg.enabled
                    }
                    MenuPill {
                        id: subRegionPill
                        text: qsTr("Bölge seç")
                        enabled: menu.connected
                        onClicked: {
                            menu.hide()
                            subtitles.selecting = true
                        }
                    }
                    MenuPill {
                        id: subCoverPill
                        text: qsTr("İngilizcesini kapat")
                        selected: !!subRow.cfg.coverOriginal
                        onClicked: if (subtitles.config) subtitles.config.coverOriginal = !subRow.cfg.coverOriginal
                    }
                    MenuPill {
                        id: subFixPill
                        text: qsTr("Düzelt")
                        enabled: (subtitles.text || "").length > 0
                        onClicked: subtitles.openCorrection()
                    }
                    MenuPill {
                        id: subSourcePill
                        text: qsTr("İngilizcesini de göster")
                        selected: !!subRow.cfg.showSource
                        onClicked: if (subtitles.config) subtitles.config.showSource = !subRow.cfg.showSource
                    }
                }

                Item { Layout.fillWidth: true }

                GroupLabel { text: qsTr("Yazı boyutu") }
                MenuSlider {
                    id: subSizeSlider
                    Layout.preferredWidth: 150
                    from: 0.5
                    to: 2
                    stepSize: 0.1
                    value: subRow.cfg.fontRel !== undefined ? subRow.cfg.fontRel : 1.0
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

            // ── Input: keyboard and mouse ──
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                GroupLabel { text: qsTr("Klavye ve fare") }
                MenuPill {
                    id: klavyeFarePill
                    readonly property bool active: oviora.preferences.keyboardMouse !== false
                    text: active ? qsTr("Açık") : qsTr("Kapalı")
                    selected: active
                    onClicked: oviora.setPreference("keyboardMouse", !active)
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    // ── The picture section: every picture setting of the engine ──
    FocusScope {
        id: picturePanel
        visible: menu.pictureShown
        anchors.fill: parent

        Rectangle {
            anchors.fill: parent
            radius: Tema.radiusPanel
            color: Tema.surface
            border.width: 1
            border.color: Tema.line
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: 14
            anchors.bottomMargin: 2
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
                    text: qsTr("Görüntü ayarları")
                    elide: Text.ElideRight
                    font.family: Tema.display
                    font.weight: Font.DemiBold
                    font.pixelSize: 20
                    font.letterSpacing: -0.3
                    color: Tema.text
                }
                AbstractButton {
                    id: pictureClose
                    focusPolicy: Qt.StrongFocus
                    hoverEnabled: true
                    implicitWidth: 40
                    implicitHeight: 40
                    onClicked: menu.hide()
                    background: Rectangle {
                        radius: 20
                        color: pictureClose.hovered || pictureClose.activeFocus ? Tema.surface2 : "transparent"
                        border.width: pictureClose.activeFocus ? 2 : 0
                        border.color: Tema.accent
                    }
                    contentItem: Item {
                        Ikon {
                            anchors.centerIn: parent
                            width: 18
                            height: 18
                            name: "close"
                            color: pictureClose.hovered ? Tema.text : Tema.muted
                        }
                    }
                }
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
}
