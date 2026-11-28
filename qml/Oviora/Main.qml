// The application shell.
//
// Structure:
//   * a normal Windows window: the system title bar and frame move, resize,
//     maximize and close it;
//   * one page visible at a time: home / min / set;
//   * in cinema mode (the stream's picture) the WINDOW ITSELF snaps to the picture's
//     aspect ratio so it has no letterbox bars. The separate control window
//     straddles its lower edge.
//
// The video is not in this scene graph. It stays the native D3D12 child window
// that apps/oviora-qml/main.cpp places over the item named "videoHost"; Qt hosts
// it and never composites it, which is what preserved present cost in S3.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ayar" as S

Window {
    id: root
    // 16:9 like the picture (and the border drag keeps it so): 800 left bars above and below
    width: 1280
    height: 720
    // Cinema follows the picture, and a portrait picture needs a narrow window.
    minimumWidth: 720
    minimumHeight: 260
    visible: true
    // Opaque: a framed window has no transparent corners to show through.
    color: cinema ? Theme.videoBlack : Theme.bgOuter
    flags: Qt.Window
    title: "Oviora"

    // The pages the app has: home (consoles), min (the stream's picture), set (settings).
    property string page: "home"
    readonly property var knownPages: ["home", "min", "set"]
    property var lastConnect: null
    property bool pendingReport: false
    function openProblemReport() {
        goPage("set")
        if (settingsLoader.item) settingsLoader.item.openReport()
        else { pendingReport = true; settingsLoader.active = true }
    }
    function followStream() {
        // A connection being opened counts too: without it the page went back home before the
        // engine opened and the video window had no rectangle ("pre-open" error on every connect)
        const stage = (oviora.connection && oviora.connection.stage) || ""
        const streaming = oviora.hasSource || oviora.ps5.active === true || (oviora.liveOpeningText || "").length > 0
            || (stage !== "" && stage !== "failed")
        if (streaming && root.page === "home") root.goPage("min")
        else if (!streaming && root.page === "min") root.goPage("home")
    }
    function goPage(p) {
        if (knownPages.indexOf(p) < 0) { oviora.logUi("ui-page", "ignored unknown page " + p); return }
        page = p
    }
    // Test switches from the command line (main.cpp, G0.3); empty in normal use.
    readonly property var test: typeof vyTest !== "undefined" ? vyTest : ({})
    readonly property real testAspect: test.aspect !== undefined ? test.aspect : 0
    readonly property bool cinema: page === "min"
    // Maximized behaves like fullscreen for geometry: the screen owns the size,
    // so the cinema aspect rule must not fight the window manager.
    readonly property bool maximized: visibility === Window.Maximized
    // Use visibility directly: leaving fullscreen clears fullTarget before the
    // Maximized binding settles. The cinema animation must not resize a screen
    // sized window during that transition.
    readonly property bool screenSized: fullTarget || fullRestorePending || visibility === Window.FullScreen || visibility === Window.Maximized
    Binding { target: Theme; property: "reduced"; value: oviora.reducedMotion || root.test.reducedMotion === true }
    Binding { target: Theme; property: "accentName"; value: oviora.preferences.accent === "white" ? "white" : "purple" }
    Binding { target: Theme; property: "backdropLevel"; value: oviora.preferences.backdrop !== undefined ? oviora.preferences.backdrop : 1 }

    // Cinema geometry: the main window ends at the picture. The independent pill
    // can extend below it without leaving an opaque strip in this window.
    // 16:9 until a source reports its own shape: a PS5 session has no aspect while
    // it connects, and 2.39 then left a thin black strip.
    property real filmAspect: 16 / 9
    readonly property real reportedAspect: oviora.previewAspect
    onReportedAspectChanged: if (reportedAspect < 0.2) fitToFilm(16 / 9)
    property string lastPage: "home"
    property real heightBeforeCinema: 0
    // The tallest picture that still fits on the screen with the pill (46px) under it.
    // The screen under the window's top centre (Screen.desktopAvailable* is the whole
    // virtual desktop, wrong on multi-monitor setups).
    readonly property rect screenArea: oviora.screenAvailableAt(root.x + root.width / 2, root.y + 20)
    readonly property real maxPictureHeight: Math.max(200, root.screenArea.height - 46 - 16)
    readonly property real pictureHeight: Math.min(Math.round(width / filmAspect), maxPictureHeight)
    // The width a tall picture took away, given back when a wider one fits again.
    property real widthBeforeNarrow: 0

    function fitToFilm(aspect) {
        if (aspect > 0.2 && aspect < 5.0) filmAspect = aspect
        // Oviora: the window is never shaped to the picture (as the old Oviora); the picture is
        // fitted inside it. Shaping it fought maximize: the height snapped back to the picture's.
        return
        // The page, not `cinema`: onPageChanged calls this before that binding updates.
        if (page !== "min" || screenSized) return
        // Narrow the window for a picture too tall for the screen, keeping its centre.
        let w = widthBeforeNarrow > 0 ? widthBeforeNarrow : width
        if (Math.round(w / filmAspect) > maxPictureHeight)
            w = Math.max(minimumWidth, Math.round(maxPictureHeight * filmAspect))
        if (Math.abs(w - width) >= 1) {
            if (widthBeforeNarrow <= 0) widthBeforeNarrow = width
            x += Math.round((width - w) / 2)
            width = w
        }
        if (widthBeforeNarrow > 0 && w >= widthBeforeNarrow) widthBeforeNarrow = 0
        height = Math.round(pictureHeight)
        keepPillOnScreen()
    }
    // The pill hangs 46px below the picture: lift the window if that leaves the screen.
    function keepPillOnScreen() {
        const a = root.screenArea
        const bottom = a.y + a.height
        if (y + pictureHeight + 46 + 8 > bottom) y = Math.max(a.y, bottom - pictureHeight - 46 - 8)
    }
    Behavior on height {
        enabled: false
        NumberAnimation { duration: Theme.d(700); easing.bezierCurve: Theme.springSoft }
    }
    onWidthChanged: {
        videoHost.syncRect()
    }
    // The window height animates towards the picture's aspect, so the picture area
    // has to follow every step of that animation.
    onHeightChanged: videoHost.syncRect()
    onMaximizedChanged: {
        videoHost.syncRect()
    }
    onPageChanged: {
        // Tell the bridge where the user is.
        if (oviora.currentPage !== page) oviora.currentPage = page
        // Test the page itself: the cinema binding may not have caught up with
        // this change yet. Leaving cinema gives the window back the height it had before.
        const wasCinema = lastPage === "min"
        lastPage = page
        if (!screenSized && wasCinema && page !== "min" && widthBeforeNarrow > 0) {
            // A tall picture narrowed the window; the other pages get their width back.
            x -= Math.round((widthBeforeNarrow - width) / 2)
            width = widthBeforeNarrow
            widthBeforeNarrow = 0
        }
        if (!screenSized && page !== "min" && width < 720) { x -= Math.round((720 - width) / 2); width = 720 }
        if (screenSized) { /* the screen is the window */ }
        else if (page === "min") fitToFilm(filmAspect)
        else if (height < 600) height = 800
        // The old page sinks (.22s) and the new one shows 150 ms later with its
        // [data-in] items rising. Reduced motion shows it at once.
        const prev = shownPage
        if (prev === page) return
        leavingPage = Theme.reduced || prev === "" ? "" : prev
        // Leaving a page without a picture (home, settings): the engine samples the
        // host size once, so the rect is placed now, not 150 ms later.
        if (prev === "home" || prev === "set") videoHost.syncRect()
        if (leavingPage !== "") { leaveAnim.restart(); showTimer.restart() }
        else showPage(false)
    }
    // The page on screen (entering or settled) and the one sinking out.
    property string shownPage: "home"
    readonly property var pageItems: [homePage, minPage, settingsPage]
    property string leavingPage: ""
    function showPage(animated) {
        shownPage = page
        if (animated)
            // By list, not pages.children: hidden pages are out of the scene (VPage.home).
            for (const p of root.pageItems) if (p.pageId === page) p.enter()
        // The video rect follows the page, and it must be settled before the
        // engine opens anything: it samples the window's client size once.
        videoHost.syncRect()
    }
    Timer { id: showTimer; interval: 150; onTriggered: root.showPage(true) }
    property real leaveT: 0
    NumberAnimation {
        id: leaveAnim
        target: root; property: "leaveT"; from: 0; to: 1
        duration: 220; easing.bezierCurve: Theme.easeOut
        onFinished: root.leavingPage = ""
    }

    // Background: pure black in cinema mode (the picture is the window), the
    // gradient otherwise. Square and without its own stroke: the system frame
    // is the window's edge now.
    VBackdrop {
        id: backdrop
        anchors.fill: parent
        visible: !root.cinema
        radius: 0
        strokeColor: "transparent"
    }

    // The one host item for the native video window; main.cpp finds it by name.
    // Exactly one must exist in the tree.
    Item {
        id: videoHost
        objectName: "videoHost"
        visible: false          // geometry proxy; the native window draws pixels
        function syncRect() {
            const r = root.videoRect()
            x = r.x; y = r.y; width = r.width; height = r.height
        }
    }

    // Where the picture sits, per page, computed from the layout numbers rather than
    // read from a laid-out child (a child reports 0x0 on the frame a source opens).
    function videoRect() {
        // Fullscreen is a video-only viewport: the picture is the screen and the
        // Oviora has no player bar in fullscreen; the stream menu (Ctrl+O) is its own window.
        if ((fullscreen || (maximized && page === "min")) && page !== "home" && page !== "set")
            return { x: 0, y: 0, width: width, height: height }
        // Oviora: the stream page is the whole window; the picture is fitted inside it
        if (page === "min")
            return { x: 0, y: 0, width: width, height: height }
        return { x: 0, y: 0, width: 0, height: 0 }
    }

    // Pages live for the whole session; only the visible page owns the video window.
    Item {
        id: pages
        anchors.fill: parent
        // In fullscreen the picture covers the page: a hidden page control must not
        // take a click meant for the picture.
        visible: !root.fullscreen || root.page === "home" || root.page === "set"
        // Oviora's home: account, PS5s over PSN and on this network, friends
        VPage {
            id: homePage
            pageId: "home"
            home: pages
            onEnter: ovHome.enter()
            OvioraHome {
                id: ovHome
                anchors.fill: parent
                onRequestSettings: root.goPage("set")
                onRequestManualHost: oviora.openPs5Dialog()
                onConnectionStarted: fn => root.lastConnect = fn
                // Over the stream a friend's message shows on the picture (messageToast) instead
                streamShown: root.page === "min"
            }
        }
        MinimalPage {
            id: minPage
            pageId: "min"
            home: pages
            onRequestAspect: aspect => { if (root.testAspect <= 0) root.fitToFilm(aspect) }
            onRequestFullscreen: root.toggleFullscreen()
        }
        // Oviora's settings
        VPage {
            id: settingsPage
            pageId: "set"
            home: pages
            Loader {
                id: settingsLoader
                anchors.fill: parent
                active: false
                Connections {
                    target: root
                    function onPageChanged() { if (root.page === "set") settingsLoader.active = true }
                }
                Component.onCompleted: if (root.page === "set") active = true
                onLoaded: if (root.pendingReport && item) { root.pendingReport = false; item.openReport() }
                sourceComponent: Component {
                    OvioraSettings {
                        onCloseRequested: root.goPage("home")
                        onPsnLinkRequested: { root.goPage("home"); ovHome.showPsnLinkDialog() }
                        // --test-psn-links' made-up accounts (ids below 0) never reach Sony or the server
                        onPsnWideRequested: (link, name) => { if (link > 0) { root.goPage("home"); ovHome.showPsnWideDialog(link, name) } }
                    }
                }
            }
        }
    }

    // The dialogs sit above the pages.
    DialogHost {
        id: dialogs
        anchors.fill: parent
        // The dialog opens the PS5 through the bridge; show the picture.
        onStartPs5: { root.lastConnect = () => oviora.ps5Connect(); if (root.page === "home") root.goPage("min") }
    }

    // Keyboard. Text fields keep their own keys (Qt's shortcut override).
    readonly property bool fullscreen: visibility === Window.FullScreen
    // Set before the visibility changes: Windows resizes the window first, and the
    // cinema rule would otherwise snap that fullscreen size back to the picture.
    property bool fullTarget: false
    property int fullRestoreVisibility: Window.Windowed
    // Windows may briefly report Windowed while restoring a maximized window.
    // Keep cinema sizing out of that native transition, without a polling timer.
    property bool fullRestorePending: false
    property bool fullLocked: false
    onFullscreenChanged: {
        oviora.setPresentationFullscreen(fullscreen)
        fullTarget = fullscreen
        if (!fullscreen) {
            fullLocked = false
        }
        fullControls = true
        fullHide.restart()
        videoHost.syncRect()
    }
    function toggleFullscreen() {
        if (!fullscreen && oviora.fullscreenMemorySafe === false) {
            toast.show(qsTr("本次播放检测到全屏显存持续增长，请使用窗口播放；重新打开片源后可再次测试全屏。"), true)
            oviora.logUi("ui-fullscreen", "blocked after sustained video-memory growth")
            return
        }
        fullLocked = false
        if (!fullscreen) {
            fullRestoreVisibility = maximized ? Window.Maximized : Window.Windowed
            fullTarget = true
            visibility = Window.FullScreen
        } else {
            fullRestorePending = fullRestoreVisibility === Window.Maximized
            fullTarget = false
            visibility = fullRestoreVisibility
            Qt.callLater(function() { root.fullRestorePending = false })
        }
        oviora.logUi("ui-fullscreen", "enabled=" + fullscreen)
    }

    // Fullscreen: any pointer movement shows the cursor, 1.6 s without movement hides it
    // (Oviora has no player bar: the stream menu is Ctrl+O); Ctrl+L locks it away.
    property bool fullControls: true
    // Also consumed by the native video HWND: a QML cursor alone cannot hide
    // the class cursor of the foreign child window under a stationary pointer.
    // Görüntü → Pencere → Fare imlecini gizle: over the stream's picture the cursor also goes after
    // hideCursorSeconds without movement, windowed too (old: hideCursor).
    // Never while something to click is open over the picture (Ctrl+O menu, a card, the PS5 keyboard):
    // in full screen the cursor went after 1.6 s and was gone over the open menu
    readonly property bool fullscreenCursorHidden: ((fullscreen && (!fullControls || fullLocked)) || (streamCursorHides && streamCursorIdle)
        || oviora.mouseAimCaptured) && dialogs.dialog === "" && !root.streamMenuShown
        && !pinCard.visible && !errorCard.visible && !cantDisplayCard.visible && !consoleKeyboardDialog.visible && !restAsk.shown
    onFullscreenCursorHiddenChanged: oviora.logUi("ui-cursor", fullscreenCursorHidden ? "hidden" : "shown")
    readonly property bool streamCursorHides: (oviora.preferences && oviora.preferences.hideCursor === true) && page === "min" && oviora.hasSource
        && !root.streamMenuShown && !restAsk.shown
    property bool streamCursorIdle: false
    onStreamCursorIdleChanged: if (streamCursorHides) oviora.logUi("ui-cursor", streamCursorIdle ? "hidden over the stream" : "shown")
    onStreamCursorHidesChanged: { streamCursorIdle = false; if (streamCursorHides) streamCursorTimer.restart(); else streamCursorTimer.stop() }
    Timer {
        id: streamCursorTimer
        interval: Math.max(1, oviora.preferences.hideCursorSeconds || 2) * 1000
        onTriggered: if (root.streamCursorHides) root.streamCursorIdle = true
    }
    function streamPointerMoved() {
        if (!streamCursorHides) return
        streamCursorIdle = false
        streamCursorTimer.restart()
    }
    function pointerActivity() {
        if (!fullscreen || fullLocked || root.test.fullBar === "hidden") return
        if (root.test.fullDebug === true) oviora.logUi("ui-fullscreen-debug", "pointer activity")
        if (!fullControls) { fullControls = true; oviora.logUi("ui-fullscreen", "controls shown by pointer") }
        fullHide.restart()
    }
    Timer {
        id: fullHide
        interval: 1600
        onTriggered: {
            if (!root.fullscreen || !root.fullControls) return
            if (root.test.fullBar === "shown") { restart(); return }
            root.fullControls = false
            oviora.logUi("ui-fullscreen", "controls hidden; video and subtitles only")
        }
    }
    // Every pointer move over this window (a HoverHandler sees them all, whatever
    // item is under the pointer; the video window passes them through). The cursor
    // hides with the controls.
    HoverHandler {
        id: pointerWatch
        cursorShape: root.fullscreenCursorHidden ? Qt.BlankCursor : Qt.ArrowCursor
        // Qt re-sends hover while anything animates under a still pointer; only a
        // real change of position counts as movement.
        property point last: Qt.point(-1, -1)
        onPointChanged: {
            const p = point.scenePosition
            if (Math.abs(p.x - last.x) < 1 && Math.abs(p.y - last.y) < 1) return
            last = p
            root.pointerActivity()
            root.streamPointerMoved()
        }
    }
    // A double click on the picture toggles fullscreen, unless locked. Windowed, only
    // a double click inside the picture counts. A single click on a playing file
    // pauses or resumes it; live sources ignore it (a click to focus the window
    // mid-game must not stop the stream).
    function pictureContains(p) {
        return p.x >= videoHost.x && p.y >= videoHost.y && p.x <= videoHost.x + videoHost.width
            && p.y <= videoHost.y + videoHost.height
    }
    // Anything floating over the picture is cut out of the video window (videoCover).
    // A click on it belongs to it, never to the picture.
    function overVideoCover(scenePoint) {
        const visit = item => {
            if (!item || !item.visible || item.opacity <= 0) return false
            if (item.objectName === "videoCover" || item.videoCover === true) {
                const q = item.mapFromItem(null, scenePoint.x, scenePoint.y)
                if (q.x >= 0 && q.y >= 0 && q.x <= item.width && q.y <= item.height) return true
            }
            for (let i = 0; i < item.children.length; ++i) if (visit(item.children[i])) return true
            return false
        }
        return visit(root.contentItem.parent ? root.contentItem.parent : root.contentItem)
    }
    TapHandler {
        id: pictureTap
        enabled: !root.fullLocked
        // Judged where the press landed: by the release the page may already have changed.
        property bool pressOnCover: false
        property bool pressOnPicture: false
        onPressedChanged: if (pressed) {
            pressOnCover = root.overVideoCover(point.scenePosition)
            pressOnPicture = root.pictureContains(point.scenePosition)
        }
        onTapped: (eventPoint, button) => {
            if (pressOnCover || !pressOnPicture) return
            if (root.page !== "min" && !root.fullscreen) return
            if (dialogs.dialog !== "" || !oviora.hasSource) return
            root.contentItem.forceActiveFocus()
            if (oviora.isCapture || oviora.duration <= 0) return
            if (oviora.protectionDrawShape.length > 0 || oviora.compareMode === 2) return
            if (pictureTap.tapCount <= 2) oviora.togglePlayPause()
        }
    }
    TapHandler {
        enabled: !root.fullLocked
        property bool pressOnCover: false
        property bool pressOnPicture: false
        onPressedChanged: if (pressed) {
            pressOnCover = root.overVideoCover(point.scenePosition)
            pressOnPicture = root.pictureContains(point.scenePosition)
        }
        onDoubleTapped: (eventPoint, button) => {
            const p = eventPoint.position
            // Görüntü → Pencere → Çift tıklayınca tam ekran (old: fullscreenDoubleClick, on by default)
            if (oviora.preferences.fullscreenDoubleClick === false) return
            if (pressOnCover || !pressOnPicture) return
            if (root.fullscreen) { root.toggleFullscreen(); return }
            if (root.page !== "min") return
            if (dialogs.dialog !== "" || !oviora.hasSource) return
            if (oviora.protectionDrawShape.length > 0 || oviora.compareMode === 2) return
            if (p.x < videoHost.x || p.y < videoHost.y || p.x > videoHost.x + videoHost.width
                || p.y > videoHost.y + videoHost.height) return
            root.toggleFullscreen()
        }
    }
    // Region drawing from the picture settings (Tüm efektleri hariç tut / NR protection): the menu
    // closes while the user drags on the picture (the bridge's drawing window takes the mouse and Esc)
    // and opens again on the same tab when the drawing is done or cancelled.
    property string drawReturnTab: ""
    property bool drawReturnAll: false
    Connections {
        target: oviora
        function onProtectionDrawChanged() {
            if (oviora.protectionDrawShape.length > 0) {
                const sm = streamMenuLoader.item
                if (sm && sm.shown && sm.pictureShown) {
                    root.drawReturnTab = sm.currentPictureTab()
                    root.drawReturnAll = !!(oviora.exclusionState && oviora.exclusionState.drawShape === oviora.protectionDrawShape)
                    oviora.logUi("ui-protection", "menu closed for drawing, tab " + root.drawReturnTab)
                    sm.hide()
                }
            } else if (root.drawReturnTab.length > 0) {
                const tab = root.drawReturnTab
                root.drawReturnTab = ""
                if (oviora.hasSource) {
                    oviora.logUi("ui-protection", "menu opened again on tab " + tab)
                    root.ensureStreamMenu().returnFromDrawing(tab, root.drawReturnAll)
                }
            }
        }
        function onSnapshotChanged() {
            // The stream ended during a drawing: nothing left to draw on
            if (!oviora.hasSource && oviora.protectionDrawShape.length > 0) oviora.beginProtectionDraw("")
        }
    }
    // Test runs: --dialog rest puts the PS5 in rest mode once the stream shows ("Dinlenmeye al")
    Timer {
        id: testRest
        interval: 1000
        repeat: true
        onTriggered: if (root.liveOnScreen) { stop(); oviora.logUi("ui-test", "rest mode"); oviora.ps5Rest() }
    }
    // Test runs: --dialog editconsole presses "Düzenle" on the first registered console once the list is in
    Timer {
        id: testEditConsole
        interval: 2500
        onTriggered: {
            const c = oviora.lanConsoles.filter(x => x.registered)
            if (c.length > 0) oviora.lanEdit(c[0].key)
            else oviora.logUi("ui-test", "editconsole: no registered console")
        }
    }
    // Test runs: --test-mask <rect|ellipse|hdron|hdroff>@<s>[,drag@<s>|esc@<s>] presses the draw button of
    // "Tüm efektleri hariç tut" in the open picture settings, then drags on the picture (or presses
    // Esc) through messages to the app's own drawing window: no real input.
    Timer {
        id: testMaskTimer
        property var steps: []
        function next() {
            if (steps.length === 0) return
            const at = steps[0].at
            interval = Math.max(1, at * 1000 - testMaskClock.elapsed())
            start()
        }
        onTriggered: {
            const step = steps.shift()
            oviora.logUi("ui-protection", "test step " + step.what)
            if (step.what === "rect" || step.what === "ellipse") {
                const sm = root.ensureStreamMenu()
                if (!sm.shown) { sm.open(); sm.openPicture("quality") }
                oviora.beginMaskDraw(true, step.what)
            } else if (step.what === "drag") {
                oviora.testMaskDrag(0.30, 0.30, 0.55, 0.62)
            } else if (step.what === "esc") {
                oviora.testMaskEscape()
            } else if (step.what === "hdron" || step.what === "hdroff") {
                // RTX Video HDR switched as its card switch does (the Windows HDR note)
                const sm = root.ensureStreamMenu()
                if (!sm.shown) { sm.open(); sm.openPicture("quality") }
                oviora.videoHdr = step.what === "hdron"
                oviora.logUi("ui-video-hdr", "test videoHdr=" + oviora.videoHdr + " displayHdr=" + oviora.displayHdr)
                if (sm.pictureItem()) sm.pictureItem().revealVideoHdr()
            }
            next()
        }
    }
    QtObject {
        id: testMaskClock
        property double started: Date.now()
        function elapsed() { return Date.now() - started }
    }
    function startTestMask() {
        if (root.test.mask === undefined) return
        const steps = []
        for (const item of String(root.test.mask).split(",")) {
            const parts = item.split("@")
            if (parts.length === 2) steps.push({ what: parts[0], at: Number(parts[1]) })
        }
        testMaskTimer.steps = steps
        testMaskTimer.next()
    }
    // A PS5 started from the home (this network or over PSN) shows its picture; when it ends
    // with nothing else open, back to the home.
    Connections {
        target: oviora
        // The engine's snapshot says when a source (the PS5 stream) opens or closes; ps5Changed only
        // follows the dialog's status text and missed a stream started from the home.
        function onSnapshotChanged() { root.followStream() }
        function onPs5Changed() { root.followStream() }
        function onConnectionChanged() { root.followStream() }
    }
    function toggleLock() {
        fullLocked = !fullLocked
        if (!fullLocked) { fullControls = true; fullHide.restart() }
        toast.show(fullLocked ? qsTr("已锁定全屏 · Ctrl+L 解锁，Esc 退出全屏") : qsTr("已解锁全屏"), false)
        oviora.logUi("ui-fullscreen", "locked=" + fullLocked + " shortcut=Ctrl+L")
    }
    // Rebindable; the hold-to-compare key is handled by the bridge's application
    // event filter because it needs the key release too.
    // Oviora: the video player's one-key shortcuts are off during a PS5 stream: Space paused the live
    // picture, F and the arrows are game keys (F11 / Alt+Enter still switch full screen)
    readonly property bool ps5Live: oviora.ps5.active === true
    Shortcut { sequence: oviora.shortcuts.playPause; enabled: !root.ps5Live; onActivated: oviora.togglePlayPause() }
    Shortcut { sequence: oviora.shortcuts.fullscreen; enabled: !root.ps5Live; onActivated: root.toggleFullscreen() }
    Shortcut { sequence: oviora.shortcuts.screenshot; onActivated: oviora.takeScreenshot() }
    Shortcut { sequences: ["F11", "Alt+Return", "Alt+Enter"]; onActivated: root.toggleFullscreen() }
    Shortcut {
        sequence: "Esc"
        // The open stream menu owns Escape (two enabled shortcuts on one key: neither fires)
        enabled: root.fullscreen && !root.streamMenuShown && !subtitles.selecting && !connectingView.visible && !pinCard.visible && !errorCard.visible && !consoleKeyboardDialog.visible
        onActivated: {
            // A visible region drawing owns the first Escape.
            if (oviora.protectionDrawShape.length > 0) oviora.beginProtectionDraw("")
            else root.toggleFullscreen()
        }
    }
    // The arrows move between the stream menu's controls while it is open
    Shortcut { sequence: "Up"; enabled: !root.streamMenuShown && !root.ps5Live; onActivated: oviora.volume = Math.min(1, oviora.volume + 0.05) }
    Shortcut { sequence: "Down"; enabled: !root.streamMenuShown && !root.ps5Live; onActivated: oviora.volume = Math.max(0, oviora.volume - 0.05) }
    Loader {
        id: pairDialogLoader
        anchors.fill: parent
        z: 450
        active: false
        sourceComponent: Component {
            OvioraPairDialog { anchors.fill: parent }
        }
    }
    function openPairDialog() {
        pairDialogLoader.active = true
        if (pairDialogLoader.item) pairDialogLoader.item.open()
    }
    // Oviora: Ctrl+O opens and closes the stream menu while a PS5 runs
    Loader {
        id: streamMenuLoader
        z: 500
        active: false
        sourceComponent: Component {
            StreamMenu {
                owner: root
                onRequestFullscreen: root.toggleFullscreen()
                onRequestDisconnect: root.disconnectWithHole()
            }
        }
    }
    // Bağlantıyı kes from the stream menu: the black hole takes the picture, then the session ends.
    // Without a picture or with reduced motion it simply ends.
    function disconnectWithHole() {
        if (Theme.reduced || !oviora.hasSource || blackHole.running) {
            if (streamMenuLoader.item) streamMenuLoader.item.hide()
            oviora.ps5Cancel()
            return
        }
        blackHole.begin()
    }
    readonly property bool streamMenuShown: !!(streamMenuLoader.item && streamMenuLoader.item.shown)
    function ensureStreamMenu() {
        streamMenuLoader.active = true
        return streamMenuLoader.item
    }
    // Testing: --dialog streammenu opens the stream menu (Ctrl+O) on the home screen, so its rows load
    Timer {
        interval: 5000
        running: root.test.dialog === "streammenu"
        // --section <name> with it: that part of the menu (e.g. subs)
        onTriggered: { const m = root.ensureStreamMenu(); if (m) { m.open(); if (root.test.section) m.section = root.test.section } }
    }
    // "Düzelt": the subtitle line on screen, corrected in the shared translation memory
    Loader {
        id: subtitleFixLoader
        anchors.fill: parent
        z: 600
        active: false
        sourceComponent: Component {
            Item {
                property alias dialog: fixDialog
                SubtitleFixDialog { id: fixDialog }
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+O"
        enabled: oviora.hasSource
        onActivated: root.ensureStreamMenu().toggle()
    }
    // Ctrl+Enter: the messages straight away while playing (the conversation of a message just shown, if any);
    // again from the chat back to the game. Ctrl combinations never go to the PS5
    Shortcut {
        sequences: ["Ctrl+Return", "Ctrl+Enter"]
        enabled: oviora.hasSource
        onActivated: {
            const sm = root.ensureStreamMenu()
            if (root.streamMenuShown && sm.chatShown) {
                sm.hide()
            } else if (messageToast.shown && messageToast.group) {
                messageToast.shown = false
                sm.openGroupChat(messageToast.group)
            } else if (messageToast.shown && messageToast.from !== "") {
                messageToast.shown = false
                sm.openChat(messageToast.from)
            } else {
                sm.openChat("")
            }
        }
    }
    // The pad's combination (Ayarlar → Genel → Yayın menüsü) opens and closes it too
    Connections {
        target: oviora
        function onStreamMenuRequested() { if (oviora.hasSource) root.ensureStreamMenu().toggle() }
    }
    // Keyboard play: keys go to the PS5 only while its picture is shown with nothing over it
    Binding {
        target: oviora
        property: "gameKeysAllowed"
        value: root.page === "min" && !root.streamMenuShown && dialogs.dialog === "" && !(pairDialogLoader.item && pairDialogLoader.item.visible) && !restAsk.shown && !subtitles.selecting && !(subtitleFixLoader.item && subtitleFixLoader.item.dialog.visible) && !connectingView.visible && !pinCard.visible && !errorCard.visible && !cantDisplayCard.visible && !consoleKeyboardDialog.visible
    }
    // Oviora: Ctrl+M turns the drawn masks (effect exclusion and NR protection) off and on at once,
    // also while playing (Ctrl combinations never go to the PS5)
    Shortcut {
        sequence: oviora.shortcuts.maskToggle || ""
        enabled: !!oviora.shortcuts.maskToggle && !(subtitleFixLoader.item && subtitleFixLoader.item.dialog.visible)
        onActivated: root.toggleMasks()
    }
    function toggleMasks() {
        const ex = oviora.exclusionState || {}
        const pr = oviora.protectionState || {}
        const exHas = (ex.regions || []).length > 0
        const prHas = (pr.regions || []).length > 0
        if (!exHas && !prHas) {
            toast.show(qsTr("Çizilmiş maske yok. Görüntü ayarlarından bir bölge çiz."), false)
            return
        }
        const on = !((exHas && ex.enabled) || (prHas && pr.enabled))
        if (exHas) oviora.setMaskEnabled(true, on)
        if (prHas) oviora.setMaskEnabled(false, on)
        toast.show(on ? qsTr("Maskeler açık") : qsTr("Maskeler kapalı"), false)
    }
    Shortcut {
        sequence: oviora.shortcuts.lock
        enabled: root.fullscreen
        onActivated: root.toggleLock()
    }
    // Subtitle keys: B on/off, Z/X -/+50 ms (Shift: 1 s), T/Y cycle the
    // primary/secondary track.
    Shortcut { sequence: "B"; onActivated: oviora.toggleSubtitles() }
    Shortcut { sequence: "Z"; onActivated: oviora.nudgeSubtitle(-50) }
    Shortcut { sequence: "Shift+Z"; onActivated: oviora.nudgeSubtitle(-1000) }
    Shortcut { sequence: "X"; onActivated: oviora.nudgeSubtitle(50) }
    Shortcut { sequence: "Shift+X"; onActivated: oviora.nudgeSubtitle(1000) }
    Shortcut { sequence: "T"; onActivated: oviora.cycleSubtitle(false) }
    Shortcut { sequence: "Y"; onActivated: oviora.cycleSubtitle(true) }

    // Oviora: the in-game subtitle translation (context property "subtitles", src/platform/subtitle).
    // While a PS5 picture runs: Ctrl+T on/off, Ctrl+B draw the region over the subtitle, Ctrl+R show
    // the region (then Ctrl+arrows move it, Ctrl+Shift+arrows size it, Ctrl+PgUp/PgDn the threshold,
    // Ctrl+plus/minus the text size), Ctrl+D the problem record, Ctrl+E correct the line on screen.
    Shortcut { sequence: "Ctrl+T"; enabled: subtitles.streaming; onActivated: subtitles.handleKey(Qt.Key_T, Qt.ControlModifier) }
    Shortcut { sequence: "Ctrl+B"; enabled: subtitles.streaming; onActivated: subtitles.handleKey(Qt.Key_B, Qt.ControlModifier) }
    Shortcut { sequence: "Ctrl+R"; enabled: subtitles.streaming; onActivated: subtitles.handleKey(Qt.Key_R, Qt.ControlModifier) }
    Shortcut { sequence: "Ctrl+D"; enabled: subtitles.streaming; onActivated: subtitles.handleKey(Qt.Key_D, Qt.ControlModifier) }
    Shortcut { sequence: "Ctrl+E"; enabled: subtitles.streaming && dialogs.dialog === ""; onActivated: subtitles.handleKey(Qt.Key_E, Qt.ControlModifier) }
    readonly property bool subtitleRegionKeys: subtitles.streaming && !!(subtitles.config && subtitles.config.showRegion === true) && !root.streamMenuShown
    Shortcut { sequence: "Ctrl+Left"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_Left, Qt.ControlModifier) }
    Shortcut { sequence: "Ctrl+Right"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_Right, Qt.ControlModifier) }
    Shortcut { sequence: "Ctrl+Up"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_Up, Qt.ControlModifier) }
    Shortcut { sequence: "Ctrl+Down"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_Down, Qt.ControlModifier) }
    Shortcut { sequence: "Ctrl+Shift+Left"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_Left, Qt.ControlModifier | Qt.ShiftModifier) }
    Shortcut { sequence: "Ctrl+Shift+Right"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_Right, Qt.ControlModifier | Qt.ShiftModifier) }
    Shortcut { sequence: "Ctrl+Shift+Up"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_Up, Qt.ControlModifier | Qt.ShiftModifier) }
    Shortcut { sequence: "Ctrl+Shift+Down"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_Down, Qt.ControlModifier | Qt.ShiftModifier) }
    Shortcut { sequence: "Ctrl+PgUp"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_PageUp, Qt.ControlModifier) }
    Shortcut { sequence: "Ctrl+PgDown"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_PageDown, Qt.ControlModifier) }
    Shortcut { sequences: ["Ctrl++", "Ctrl+="]; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_Plus, Qt.ControlModifier) }
    Shortcut { sequence: "Ctrl+-"; enabled: root.subtitleRegionKeys; onActivated: subtitles.handleKey(Qt.Key_Minus, Qt.ControlModifier) }
    Shortcut { sequence: "Esc"; enabled: subtitles.selecting; onActivated: subtitles.selecting = false }
    // Ctrl+B / "Bölge seç": a box dragged over the picture becomes the region that is read. The
    // dimmed picture, the box and the hint are drawn on the subtitle overlay window (the native
    // video window covers QML); this area only takes the mouse.
    MouseArea {
        id: subtitleSelectArea
        visible: subtitles.selecting
        x: videoHost.x
        y: videoHost.y
        width: videoHost.width
        height: videoHost.height
        z: 440
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.CrossCursor
        property point start
        property bool dragging: false
        onVisibleChanged: dragging = false
        onPressed: mouse => {
            if (mouse.button === Qt.RightButton) { subtitles.selecting = false; return }
            start = Qt.point(mouse.x, mouse.y)
            dragging = true
            subtitles.dragSelection(mouse.x / width, mouse.y / height, mouse.x / width, mouse.y / height)
        }
        onPositionChanged: mouse => {
            if (dragging) subtitles.dragSelection(start.x / width, start.y / height, mouse.x / width, mouse.y / height)
        }
        onReleased: mouse => {
            if (!dragging || mouse.button !== Qt.LeftButton) return
            dragging = false
            subtitles.selectRegion(start.x / width, start.y / height, mouse.x / width, mouse.y / height)
        }
    }
    // "Düzelt" (Ctrl+E or the stream menu): correct the line on screen in the shared memory
    Connections {
        target: subtitles
        function onCorrectionRequested(english, translation) {
            if (streamMenuLoader.item) streamMenuLoader.item.hide()
            subtitleFixLoader.active = true
            if (subtitleFixLoader.item) subtitleFixLoader.item.dialog.start(english, translation)
        }
        function onOverlayChanged() { root.updateSubtitleGame() }
    }
    // The running game { id: PS5 title id, name } of the stream ({ id: "" } when not known). On this network from
    // the console's own report (discovery), over PSN from what the Oviora server knows about the console (the
    // owner's app on the home network, or PSN game info)
    readonly property var currentGame: {
        let id = "", name = ""
        const havePlatform = typeof platform !== "undefined" && platform
        if (havePlatform && platform.sessionState === "playing" && platform.sessionDuid) {
            for (const list of [platform.available || [], platform.consoles || []])
                for (const c of list)
                    if (c && c.duid === platform.sessionDuid && c.app) {
                        id = c.app.title_id || ""
                        name = c.app.name || ""
                    }
            if (!id) id = platform.runningTitleId(platform.sessionDuid)
        } else {
            const host = (oviora.ps5 && oviora.ps5.host) || ""
            for (const c of (oviora.lanConsoles || []))
                if (host && c && c.address === host) {
                    id = c.titleId || (c.duid && havePlatform ? platform.runningTitleId(c.duid) : "")
                    name = c.app || ""
                }
        }
        id = String(id || "").split("_")[0].toUpperCase()
        return /^[A-Z]{4}[0-9]{5}$/.test(id) ? { id: id, name: name || id } : { id: "", name: "" }
    }
    onCurrentGameChanged: { root.updateSubtitleGame(); root.applyGameProfile() }
    // The running game's title id: the shared memory keeps lines per game ("genel" when unknown)
    function updateSubtitleGame() {
        if (!subtitles.streaming) return
        subtitles.game = root.currentGame.id
    }
    // Per-game profiles (Ctrl+O > Bu oyun için kaydet): the subtitle area, the picture preset and the mouse
    // aim settings, loaded when that game starts or comes back during a stream
    property string gameProfileApplied: ""
    readonly property var gameProfile: (oviora.preferences && oviora.preferences.gameProfiles && root.currentGame.id)
        ? (oviora.preferences.gameProfiles[root.currentGame.id] || null) : null
    function applyGameProfile() {
        const g = root.currentGame
        if (!oviora.hasSource || !g.id || g.id === root.gameProfileApplied) return
        root.gameProfileApplied = g.id
        const p = root.gameProfile
        if (!p) return
        if (p.subtitle && subtitles.config)
            for (const k of ["regionX", "regionY", "regionW", "regionH"])
                if (p.subtitle[k] !== undefined) subtitles.config[k] = p.subtitle[k]
        if (p.preset) {
            const preset = (oviora.presets || []).find(x => x.name === p.preset)
            if (preset && oviora.currentPresetName !== p.preset) oviora.applyPresetIndex(preset.index)
        }
        if (p.mouseAim) oviora.setPreference("mouseAim", p.mouseAim)
        toast.show(qsTr("%1 için kaydettiğin ayarlar yüklendi").arg(p.name || g.name), false)
        oviora.logUi("game-profile", "applied " + g.id)
    }
    function saveGameProfile() {
        const g = root.currentGame
        if (!g.id) return
        const all = Object.assign({}, (oviora.preferences && oviora.preferences.gameProfiles) || {})
        const p = { name: g.name }
        if (subtitles.config && subtitles.config.regionW > 0)
            p.subtitle = { regionX: subtitles.config.regionX, regionY: subtitles.config.regionY,
                           regionW: subtitles.config.regionW, regionH: subtitles.config.regionH }
        if (oviora.currentPresetName) p.preset = oviora.currentPresetName
        if (oviora.preferences && oviora.preferences.mouseAim) p.mouseAim = oviora.preferences.mouseAim
        all[g.id] = p
        if (Object.keys(all).length > 50) {
            toast.show(qsTr("En çok 50 oyun için ayar saklanabilir"), true)
            return
        }
        if (oviora.setPreference("gameProfiles", all)) {
            root.gameProfileApplied = g.id
            toast.show(qsTr("%1 için ayarlar kaydedildi: altyazı alanı, görüntü ayarı, fare").arg(g.name), false)
        }
    }
    function forgetGameProfile() {
        const g = root.currentGame
        const all = Object.assign({}, (oviora.preferences && oviora.preferences.gameProfiles) || {})
        if (!g.id || !all[g.id]) return
        delete all[g.id]
        if (oviora.setPreference("gameProfiles", all))
            toast.show(qsTr("%1 için kayıtlı ayarlar silindi").arg(g.name), false)
    }
    // A new stream loads its game's profile again
    readonly property bool profileStream: oviora.hasSource
    onProfileStreamChanged: { if (!profileStream) root.gameProfileApplied = ""; else root.applyGameProfile() }
    Connections {
        target: oviora
        function onPs5Changed() { root.updateSubtitleGame() }
        function onLanChanged() { root.updateSubtitleGame() }
    }
    // Toasts: a refused edit, a failed open.
    // .canvas-toast: top 12px, padding 7px 12px, from translate -8px and
    // opacity 0; .on: opacity .2s, translate .45s --spring; hidden after 2600 ms.
    Rectangle {
        id: toast
        z: 30
        property string message: ""
        property bool isError: false
        property bool on: false
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 12
        // Over the picture: cut out of the video window.
        objectName: "videoCover"
        property real coverRadius: 9
        width: Math.min(toastText.implicitWidth + 24 + 2, root.width - 48)
        height: toastText.implicitHeight + 14 + 2
        radius: 9
        color: toast.isError ? "#2A1414" : "#132218"
        border.width: 1
        border.color: toast.isError ? Qt.rgba(1, 0.365, 0.365, 0.4) : Qt.rgba(0.239, 0.863, 0.518, 0.35)
        opacity: on ? 1 : 0
        visible: opacity > 0.01
        transform: Translate { id: toastShift; y: toast.on ? 0 : -8
            Behavior on y { NumberAnimation { duration: Theme.d(450); easing.bezierCurve: Theme.spring } } }
        Behavior on opacity { NumberAnimation { duration: Theme.d(200) } }
        function show(m, bad) { toast.message = m; toast.isError = bad; toast.on = true; toastTimer.restart() }
        Timer { id: toastTimer; interval: 2600; onTriggered: toast.on = false }
        Text {
            textFormat: Text.PlainText
            id: toastText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, root.width - 72)
            text: toast.message
            color: toast.isError ? "#FFC9C9" : "#BDF3D2"
            font.family: Theme.fontUi
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // RivaTuner, NVIDIA's overlay or GamePP started after Oviora (QmlPlayerBridge): RTSS's
    // OSD is kept out until a restart; the other two cannot be, and froze 2.0 in the field.
    VConfirm {
        id: overlayRestart
        objectName: "overlay-restart-confirm"
        property string kind: "rtss"
        readonly property string product: kind === "nvidia" ? qsTr("NVIDIA 信息浮窗") : qsTr("游戏加加（GamePP）")
        title: kind === "rtss" ? qsTr("检测到 RTSS 屏显服务") : qsTr("检测到 %1").arg(product)
        text: kind === "rtss"
              ? qsTr("检测到 RTSS 屏显服务正在运行。开启兼容绘制需重启 Oviora，重启后 OSD 仅显示在视频上。现在重启吗？当前播放或串流将停止。")
              : qsTr("%1 正在运行。它会同时挂接界面和视频两个画面，可能让 Oviora 卡死；开启兼容绘制需重启 Oviora，重启后界面改用软件绘制，它只作用在视频上。现在重启吗？当前播放或串流将停止。").arg(product)
        acceptText: qsTr("重启")
        rejectText: qsTr("稍后")
        onAccepted: oviora.restartForOverlayCompatibility()
    }

    Connections {
        target: oviora
        function onNotice(text, isError) { toast.show(text, isError) }
        function onOverlayRestartSuggested(kind) { overlayRestart.kind = kind; overlayRestart.open() }
        function onNavigate(p) {
            // A dialog key opens the dialog; anything else is a page. Sources and
            // pages the app no longer has are ignored.
            // Oviora: pairing a PS5 is the old Oviora's "Konsol kaydet"
            if (p === "ps5") { root.openPairDialog(); return }
            if (p === "subtitle" || p === "audio" || p === "save" || p === "manage") {
                dialogs.open(p)
                return
            }
            root.goPage(p)
        }
    }

    property string accountStatusSeen: ""
    Connections {
        target: typeof platform !== "undefined" ? platform : null
        Component.onCompleted: if (typeof platform !== "undefined" && platform) root.accountStatusSeen = platform.status
        function onAccountDeleted() {
            if (root.page === "set")
                root.goPage("home")
            toast.show(qsTr("Hesabın silindi."), false)
        }
        // Signed out (Ayarlar > Hesap > Oturumu kapat, or the token was revoked): the sign-in screen
        // is on the home page, so an open settings page must not stay over it
        function onChanged() {
            const was = root.accountStatusSeen
            root.accountStatusSeen = platform.status
            // Only a change to signed out, not the first report at start (--page set test runs)
            if (platform.status === "none" && was !== "" && was !== "none" && root.page === "set") {
                oviora.logUi("ui-page", "signed out: settings closed")
                root.goPage("home")
            }
        }
    }

    // The picture's aspect drives the window in cinema mode. Reported only: the
    // engine's own values, never an invented ratio.
    Connections {
        target: oviora
        function onSnapshotChanged() {
            if (root.fullscreen && oviora.fullscreenMemorySafe === false) root.toggleFullscreen()
            if (root.testAspect > 0) return
            if (oviora.previewAspect > 0.2 && Math.abs(oviora.previewAspect - root.filmAspect) > 0.02)
                root.fitToFilm(oviora.previewAspect)
        }
        function onCompareChanged() { if(root.testAspect <= 0) root.fitToFilm(oviora.previewAspect) }
    }

    // Motion probe (G0.5, --motion-probe): starts one motion 800 ms after load and logs
    // "motion-probe,<name>,<ms>,<value>" every frame for a second.
    Loader {
        id: probeSwitch
        active: root.test.motionProbe === "switch"
        x: 40; y: 120; z: 100
        sourceComponent: VSwitch {}
    }
    Loader {
        id: probeSeg
        active: root.test.motionProbe === "seg"
        x: 40; y: 160; z: 100
        sourceComponent: VSeg {
            options: [{ id: "a", label: qsTr("自动") }, { id: "b", label: qsTr("有限 / Limited") }, { id: "c", label: qsTr("完整 / Full") }]
            current: "a"
        }
    }
    Timer {
        running: root.test.motionProbe !== undefined
        interval: 800
        onTriggered: {
            const name = root.test.motionProbe
            if (name === "switch" && probeSwitch.item) probeSwitch.item.checked = true
            else if (name === "dialog") dialogs.open("ps5")
            else if (name === "seg" && probeSeg.item) { probe.x0 = probeSeg.item.indicatorX; probeSeg.item.current = "c"; probe.x1 = probeSeg.item.targetX }
            probe.t0 = Date.now()
            probe.running = true
        }
    }
    FrameAnimation {
        id: probe
        property real t0: 0
        property real x0: 0
        property real x1: 1
        running: false
        onTriggered: {
            const ms = Date.now() - t0
            const name = root.test.motionProbe
            let v
            if (name === "dialog") v = dialogs.motionScale.toFixed(4)
            else if (name === "seg" && probeSeg.item) v = ((probeSeg.item.indicatorX - x0) / (x1 - x0)).toFixed(4)
            else if (probeSwitch.item) v = probeSwitch.item.children[0].x - 3
            else return
            console.log("motion-probe," + name + "," + ms + "," + v)
            if (ms > 1000) running = false
        }
    }

    // Startup window size: a fixed size or the last one, clamped to the screen
    // and centred. A --size test switch resizes after this, so tests keep theirs.
    function applyStartupSize() {
        const prefs = oviora.preferences || ({})
        const choice = prefs.windowSize || "1280x720"
        const text = choice === "last" ? (prefs.lastWindow || "1280x720") : choice
        const wh = text.split("x").map(Number)
        if (wh.length !== 2 || !(wh[0] > 0) || !(wh[1] > 0)) return
        // Oviora: always 16:9 like the picture; a saved 16:10 size (the old default 1280x800)
        // keeps its width and gets the matching height, so no bars above and below the stream
        wh[1] = Math.round(wh[0] * 9 / 16)
        // Centred on the monitor the pointer is on when Oviora starts (the one it was
        // launched from), not across the whole desktop.
        const a = oviora.launchScreenAvailable()
        // Fitted to the screen as a whole, so the clamped size is 16:9 too
        const fit = Math.min(1, (a.width - 40) / wh[0], (a.height - 40) / wh[1])
        root.width = Math.max(root.minimumWidth, Math.round(wh[0] * fit))
        root.height = Math.max(root.minimumHeight, Math.round(root.width * 9 / 16))
        root.x = a.x + Math.round((a.width - root.width) / 2)
        root.y = a.y + Math.round((a.height - root.height) / 2)
    }
    // Genel → Oyunu bırakınca → Yayından çıkınca (old: disconnectAction, "ask" by default): closing
    // the window during a PS5 stream asks first, or puts the PS5 to rest before the app ends.
    // main.cpp quits after this handler unless closeHeld says the window stays for now.
    property bool closeHeld: false
    property bool closeWithoutAsking: false
    property bool closingAfterStream: false
    onClosing: (close) => {
        if (!root.fullscreen && !root.maximized && !root.cinema) oviora.rememberWindowSize(root.width, root.height)
        if (root.closeWithoutAsking || root.closingAfterStream || !(oviora.streamStats && oviora.streamStats.active === true)) return
        const action = (oviora.preferences && oviora.preferences.disconnectAction) || "ask"
        if (action === "nothing") return
        close.accepted = false
        root.closeHeld = true
        oviora.logUi("ui-close", "closing during a PS5 stream: " + action)
        if (action === "sleep") root.leaveStream(true)
        else { if (streamMenuLoader.item) streamMenuLoader.item.hide(); restAsk.open(); restAsk.forceActiveFocus() }
    }
    function leaveStream(rest) {
        root.closingAfterStream = true
        if (rest) oviora.ps5Rest()
        else oviora.ps5Cancel()
        closeAfterStream.start()
    }
    // The window closes once the stream has ended (or after a few seconds whatever happens)
    Timer { id: closeAfterStream; interval: 4000; onTriggered: root.close() }
    Connections {
        target: oviora
        enabled: root.closingAfterStream
        function onSnapshotChanged() { if (!oviora.hasSource) { closeAfterStream.stop(); Qt.callLater(root.close) } }
    }
    VConfirm {
        id: restAsk
        objectName: "rest-ask"
        z: 600
        dismissRejects: false
        title: qsTr("Bağlantıyı kes")
        text: qsTr("PS5 dinlenme moduna geçsin mi?")
        acceptText: qsTr("Dinlenmeye al")
        rejectText: qsTr("Sadece bağlantıyı kes")
        onAccepted: root.leaveStream(true)
        onRejected: root.leaveStream(false)
    }

    // ── Görüntü → Pencere → Görüntü düzeni (old: windowType): the window when a stream starts;
    // what it changed goes back when the stream page closes ──
    property string streamWindowApplied: ""
    function applyStreamWindow() {
        const mode = oviora.preferences.streamWindow || "normal"
        if (mode === "fullscreen" && !root.fullscreen) { root.toggleFullscreen(); root.streamWindowApplied = "fullscreen" }
        else if (mode === "maximized" && root.visibility === Window.Windowed) { root.visibility = Window.Maximized; root.streamWindowApplied = "maximized" }
        if (root.streamWindowApplied !== "") oviora.logUi("ui-window", "stream start: " + root.streamWindowApplied)
    }
    function restoreStreamWindow() {
        if (root.streamWindowApplied === "fullscreen" && root.fullscreen) root.toggleFullscreen()
        else if (root.streamWindowApplied === "maximized" && root.visibility === Window.Maximized) root.visibility = Window.Windowed
        root.streamWindowApplied = ""
    }
    property string streamWindowPage: "home"
    onShownPageChanged: {
        const was = streamWindowPage
        streamWindowPage = shownPage
        if (shownPage === "min" && was !== "min" && root.test.aspect === undefined) Qt.callLater(root.applyStreamWindow)
        else if (was === "min" && shownPage !== "min") root.restoreStreamWindow()
    }

    // ── Ses ve ağ → Bağlantı: the stream statistics (İstatistikler in the stream menu) and the
    // weak-connection sign, over the picture and cut out of the video window ──
    readonly property var liveStats: oviora.streamStats || ({})
    readonly property bool liveOnScreen: liveStats.active === true && root.page === "min" && !root.streamMenuShown && dialogs.dialog === "" && !restAsk.shown
    // The stream statistics (Ses ve ağ → İstatistikler, the stream menu's tile): a see-through panel
    // drawn by the bridge as a layered child of the video window (a Qt Quick item could only be cut
    // out of the picture). QML only says when it may show; the bridge tells where it ended up.
    Item {
        id: statsOverlay
        objectName: "stream-stats"
        readonly property bool wanted: root.liveOnScreen && !!(oviora.preferences && oviora.preferences.showStreamStats === true)
                                       && (oviora.protectionDrawShape || "").length === 0
        readonly property rect area: oviora.statsOverlayRect
        visible: false
        onWantedChanged: oviora.setStatsOverlayShown(wanted)
        Component.onCompleted: oviora.setStatsOverlayShown(wanted)
    }
    // End of a stream: a short summary on the home screen (length, frames, loss, delay) with a plain verdict and,
    // when something held it back, what helps. Sampled every 2 s while the stream is on screen.
    QtObject {
        id: sessionStats
        property bool on: false
        property real startedAt: 0
        property int samples: 0
        property real fpsSum: 0
        property real lossSum: 0
        property real lossMax: 0
        property real rttSum: 0
        property int rttCount: 0
        property int weakSamples: 0
        property real requestedFps: 0
        property bool internet: false
        function reset() { on = true; startedAt = Date.now(); samples = 0; fpsSum = 0; lossSum = 0; lossMax = 0; rttSum = 0; rttCount = 0; weakSamples = 0 }
    }
    Timer {
        interval: 2000
        repeat: true
        running: root.liveStats.active === true
        onRunningChanged: {
            if (running) { sessionStats.reset(); summaryCard.data_ = null; return }
            // Over: a summary when it lasted at least a minute
            if (sessionStats.on && sessionStats.samples >= 30) summaryCard.show()
            sessionStats.on = false
        }
        onTriggered: {
            const s = root.liveStats
            sessionStats.samples++
            sessionStats.fpsSum += s.fps || 0
            const loss = s.packetLoss || 0
            sessionStats.lossSum += loss
            sessionStats.lossMax = Math.max(sessionStats.lossMax, loss)
            if (loss > 0.03) sessionStats.weakSamples++
            if ((s.rttMs || 0) > 0) { sessionStats.rttSum += s.rttMs; sessionStats.rttCount++ }
            sessionStats.requestedFps = s.requestedFps || sessionStats.requestedFps
            sessionStats.internet = s.internet === true
        }
    }
    Rectangle {
        id: summaryCard
        objectName: "session-summary"
        property var data_: null
        function show() {
            const n = Math.max(1, sessionStats.samples)
            const fps = sessionStats.fpsSum / n, loss = sessionStats.lossSum / n
            const rtt = sessionStats.rttCount ? sessionStats.rttSum / sessionStats.rttCount : 0
            const minutes = Math.max(1, Math.round((Date.now() - sessionStats.startedAt) / 60000))
            const weakShare = sessionStats.weakSamples / n
            let verdict = qsTr("Bağlantın sorunsuzdu."), advice = ""
            if (weakShare > 0.1 || loss > 0.03) {
                verdict = qsTr("Takılmaların çoğu ağdan geldi: oyunun %1'inde paket kaybı vardı.").arg("%" + Math.round(weakShare * 100))
                advice = sessionStats.internet
                    ? qsTr("Öneri: Yayın ayarlarında bit hızını düşür ya da 720p'ye geç; mümkünse Wi-Fi yerine kablo kullan.")
                    : qsTr("Öneri: PS5'i kabloyla bağla ya da 5 GHz Wi-Fi kullan; modemle arasındaki mesafeyi kısalt.")
            } else if (rtt > 80) {
                verdict = qsTr("Gecikme yüksekti (ortalama %1 ms).").arg(Math.round(rtt))
                advice = qsTr("Öneri: İnternet üzerinden oynarken bu normaldir; aynı ağda olduğunda gecikme düşer. Arka planda indirme varsa durdur.")
            } else if (sessionStats.requestedFps > 0 && fps < sessionStats.requestedFps * 0.85) {
                verdict = qsTr("Ağ iyiydi ama görüntü bu bilgisayarda yavaş işlendi (ortalama %1 fps).").arg(Math.round(fps))
                advice = qsTr("Öneri: Görüntü ayarlarında iyileştirmeyi ya da ara kare üretimini bir kademe düşür.")
            }
            data_ = { minutes: minutes, fps: fps, loss: loss, rtt: rtt, verdict: verdict, advice: advice }
            summaryHide.restart()
            oviora.logUi("session-summary", "minutes=" + minutes + " fps=" + fps.toFixed(1) + " loss=" + (loss * 100).toFixed(2) + "% rtt=" + Math.round(rtt))
        }
        Timer { id: summaryHide; interval: 30000; onTriggered: summaryCard.data_ = null }
        // --dialog summary (test runs): a sample summary of a lossy internet stream
        Timer {
            interval: 4000
            running: root.test.dialog === "summary"
            onTriggered: summaryCard.data_ = { minutes: 47, fps: 58.6, loss: 0.052, rtt: 64,
                verdict: qsTr("Takılmaların çoğu ağdan geldi: oyunun %1'inde paket kaybı vardı.").arg("%18"),
                advice: qsTr("Öneri: Yayın ayarlarında bit hızını düşür ya da 720p'ye geç; mümkünse Wi-Fi yerine kablo kullan.") }
        }
        z: 470
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        width: Math.min(560, parent.width - 48)
        height: summaryColumn.implicitHeight + 32
        radius: Tema.radiusPanel
        color: Tema.surface
        border.width: 1
        border.color: Tema.lineStrong
        opacity: data_ && root.page !== "min" ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
        ColumnLayout {
            id: summaryColumn
            anchors.fill: parent
            anchors.margins: 16
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: qsTr("Oyun özeti")
                    font.family: Tema.body
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Tema.text
                }
                VButton { text: qsTr("Tamam"); onClicked: summaryCard.data_ = null }
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: summaryCard.data_ ? qsTr("%1 dk · ortalama %2 fps · paket kaybı %3 · gecikme %4")
                    .arg(summaryCard.data_.minutes).arg(Math.round(summaryCard.data_.fps))
                    .arg("%" + (summaryCard.data_.loss * 100).toFixed(1))
                    .arg(summaryCard.data_.rtt > 0 ? Math.round(summaryCard.data_.rtt) + " ms" : "—") : ""
                font.family: Tema.body
                font.pixelSize: 13
                color: Tema.muted
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: summaryCard.data_ ? summaryCard.data_.verdict : ""
                font.family: Tema.body
                font.pixelSize: 14
                color: Tema.text
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: !!summaryCard.data_ && summaryCard.data_.advice.length > 0
                wrapMode: Text.WordWrap
                text: summaryCard.data_ ? summaryCard.data_.advice : ""
                font.family: Tema.body
                font.pixelSize: 13
                color: Tema.faint
            }
        }
    }
    // Old networkIndicator: while the packet loss is above Ses ve ağ → Zayıf bağlantı uyarısı (3%)
    Rectangle {
        id: weakLink
        objectName: "weak-link"
        readonly property real limit: ((oviora.preferences && oviora.preferences.weakLinkWarning !== undefined) ? oviora.preferences.weakLinkWarning : 3) * 0.01
        // --dialog weaklink (test runs) shows it without a lossy line
        property bool forced: false
        readonly property bool weak: forced || root.liveOnScreen && (root.liveStats.packetLoss || 0) > limit
        readonly property bool videoCover: visible
        property real coverRadius: 24
        onWeakChanged: if (weak) { weakHold.restart(); oviora.logUi("ui-weak-link", "packet loss " + ((root.liveStats.packetLoss || 0) * 100).toFixed(1) + "% above " + (limit * 100).toFixed(0) + "%") }
        // Stays at least 3 s after the loss drops, so a flickering line does not blink it on and off
        Timer { id: weakHold; interval: 3000 }
        readonly property bool shown: weak || weakHold.running
        // Loss well above the limit turns it from amber to red
        readonly property bool severe: (root.liveStats.packetLoss || 0) > limit * 3
        readonly property color tone: severe ? "#ff6b6b" : "#ffb86b"
        property real lossShown: 0
        onShownChanged: if (shown) lossShown = root.liveStats.packetLoss || 0
        Connections {
            target: root
            enabled: weakLink.weak
            function onLiveStatsChanged() { weakLink.lossShown = root.liveStats.packetLoss || 0 }
        }
        z: 26
        readonly property bool underStats: statsOverlay.wanted && statsOverlay.area.height > 0
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: underStats ? Math.max(16, parent.width - statsOverlay.area.x - statsOverlay.area.width) : 32
        anchors.topMargin: underStats ? statsOverlay.area.y + statsOverlay.area.height + 12 : 32
        // A pill like the old Oviora's notices: icon, what is wrong and how much
        width: weakRow.implicitWidth + 32
        height: 48
        radius: 24
        color: Qt.rgba(0.07, 0.07, 0.09, 0.94)
        border.width: 1
        border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.55)
        Behavior on border.color { ColorAnimation { duration: 300 } }
        opacity: shown ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
        // Slides in from the right a little as it appears
        transform: Translate {
            x: weakLink.shown ? 0 : 18
            Behavior on x { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }
        }
        Row {
            id: weakRow
            anchors.centerIn: parent
            spacing: 12
            Item {
                width: 24
                height: 24
                anchors.verticalCenter: parent.verticalCenter
                // A soft glow behind the icon that breathes while the line is weak
                Rectangle {
                    anchors.centerIn: parent
                    width: 30
                    height: 30
                    radius: 15
                    color: weakLink.tone
                    opacity: 0.16
                    SequentialAnimation on opacity {
                        running: weakLink.visible && !Theme.reduced
                        loops: Animation.Infinite
                        NumberAnimation { from: 0.08; to: 0.26; duration: 900; easing.type: Easing.InOutSine }
                        NumberAnimation { from: 0.26; to: 0.08; duration: 900; easing.type: Easing.InOutSine }
                    }
                }
                Image {
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    sourceSize.width: 40
                    sourceSize.height: 40
                    source: "data:image/svg+xml;charset=utf-8," + encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="' + weakLink.tone + '" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2.5 9a14 14 0 0 1 19 0 M5.5 12.5a9.5 9.5 0 0 1 13 0 M9 16a4.5 4.5 0 0 1 6 0 M12 19.5v0.01 M3.5 3.5l17 17"/></svg>')
                }
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                Label {
                    textFormat: Text.PlainText
                    text: qsTr("Bağlantı zayıf")
                    font.family: Tema.body
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    color: Tema.text
                }
                Label {
                    textFormat: Text.PlainText
                    text: qsTr("%1 paket kaybı").arg("%" + (weakLink.lossShown * 100).toFixed(1))
                    font.family: Tema.body
                    font.pixelSize: 12
                    color: weakLink.tone
                }
            }
        }
    }
    // Packet loss above 3% for 10 s in a row: a short note how to help it, in the bottom-left
    // corner for 6 s, at most once every 5 minutes. Beside the weak-link pill, which only says
    // how bad the line is.
    Rectangle {
        id: lossNote
        objectName: "loss-note"
        readonly property bool lossy: root.liveOnScreen && (root.liveStats.packetLoss || 0) > 0.03
        property bool shown: false
        property real lossShown: 0
        property double lastShown: 0
        // --dialog lossnote (test runs) shows it without a lossy line
        property bool forced: false
        function show(loss) {
            lossShown = loss
            lastShown = Date.now()
            shown = true
            lossNoteHide.restart()
            oviora.logUi("ui-loss-note", "shown at " + (loss * 100).toFixed(1) + "% loss")
        }
        // Runs while the loss stays high: the first tick is 10 s after it went up
        Timer {
            interval: 10000
            repeat: true
            running: lossNote.lossy
            onTriggered: if (Date.now() - lossNote.lastShown >= 300000) lossNote.show(root.liveStats.packetLoss || 0)
        }
        Timer { id: lossNoteHide; interval: 6000; onTriggered: if (!lossNote.forced) lossNote.shown = false }
        readonly property bool videoCover: visible
        property real coverRadius: radius
        z: 26
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: 32
        anchors.bottomMargin: 32
        width: Math.min(lossNoteText.implicitWidth + 36, parent.width - 64)
        height: 40
        radius: 20
        color: Qt.rgba(0.05, 0.05, 0.07, 0.9)
        border.width: 1
        border.color: Tema.line
        opacity: shown && (root.liveOnScreen || forced) ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        Text {
            id: lossNoteText
            textFormat: Text.PlainText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, lossNote.width - 36)
            elide: Text.ElideRight
            text: qsTr("Bağlantıda kayıp var (%1). PS5'i kabloyla bağlamak yardımcı olur.").arg("%" + Math.max(1, Math.round(lossNote.lossShown * 100)))
            font.family: Tema.body
            font.pixelSize: 14
            color: Tema.text
        }
    }
    // A 30 fps internet stream (the "Uzak bağlantı" default): once ever, a hint that frame
    // generation can show it at 60. Only when this PC runs an FG and the user has not turned an
    // FG stage off; never switched on by itself (it changes the user's chain and adds a frame
    // of latency).
    Rectangle {
        id: fgHint
        objectName: "fg-hint"
        readonly property bool wanted: root.liveOnScreen && root.liveStats.internet === true && root.liveStats.requestedFps === 30
            && root.liveStats.fgOn !== true && root.liveStats.fgSupported === true && root.liveStats.fgTurnedOff !== true
            && !(oviora.preferences && oviora.preferences.fgHintShown === true)
        property bool shown: false
        // --dialog fghint (test runs) shows it without a stream
        property bool forced: false
        // A few seconds into the stream, after the connecting screen
        Timer {
            interval: 8000
            running: fgHint.wanted && !fgHint.shown
            onTriggered: {
                fgHint.shown = true
                fgHintHide.restart()
                oviora.setPreference("fgHintShown", true)
                oviora.logUi("ui-fg-hint", "shown once: 30 fps internet stream, frame generation off")
            }
        }
        Timer { id: fgHintHide; interval: 9000; onTriggered: if (!fgHint.forced) fgHint.shown = false }
        readonly property bool videoCover: visible
        property real coverRadius: radius
        z: 26
        anchors.left: parent.left
        anchors.bottom: lossNote.visible ? lossNote.top : parent.bottom
        anchors.leftMargin: 32
        anchors.bottomMargin: lossNote.visible ? 10 : 32
        width: Math.min(fgHintText.implicitWidth + 36, parent.width - 64)
        height: 40
        radius: 20
        color: Qt.rgba(0.05, 0.05, 0.07, 0.9)
        border.width: 1
        border.color: Tema.line
        opacity: shown && (root.liveOnScreen || forced) ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        Text {
            id: fgHintText
            textFormat: Text.PlainText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, fgHint.width - 36)
            elide: Text.ElideRight
            text: qsTr("30 fps'yi 60'a çıkarmak için Kare üretimi'ni aç (Ctrl+O › Görüntü ayarları)")
            font.family: Tema.body
            font.pixelSize: 14
            color: Tema.text
        }
    }
    // Tuşlar → Fareyle nişan alma: a short note when the pointer is captured or let go (old StreamView)
    Rectangle {
        id: mouseAimHint
        objectName: "mouse-aim-hint"
        readonly property string keyName: {
            const aim = (oviora.preferences && oviora.preferences.mouseAim) || {}
            const k = aim.toggle_key !== undefined ? aim.toggle_key : Qt.Key_F8
            const i = [Qt.Key_F6, Qt.Key_F7, Qt.Key_F8, Qt.Key_F9, Qt.Key_F10, Qt.Key_ScrollLock, Qt.Key_Pause].indexOf(k)
            return i >= 0 ? ["F6", "F7", "F8", "F9", "F10", "Scroll Lock", "Pause"][i] : "F8"
        }
        property bool captured: false
        readonly property bool videoCover: visible
        property real coverRadius: radius
        anchors.top: parent.top
        anchors.topMargin: 24
        anchors.horizontalCenter: parent.horizontalCenter
        z: 27
        width: mouseAimHintText.implicitWidth + 36
        height: 40
        radius: 20
        color: Qt.rgba(0.05, 0.05, 0.07, 0.86)
        border.width: 1
        border.color: Tema.line
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Text {
            textFormat: Text.PlainText
            id: mouseAimHintText
            anchors.centerIn: parent
            text: mouseAimHint.captured
                ? qsTr("Fareyle nişan açık · imleci bırakmak için %1").arg(mouseAimHint.keyName)
                : qsTr("İmleç serbest · fareyle nişan için %1").arg(mouseAimHint.keyName)
            font.family: Tema.body
            font.pixelSize: 14
            color: Tema.text
        }
        Timer {
            id: mouseAimHintTimer
            interval: 3200
            onTriggered: mouseAimHint.opacity = 0
        }
        Connections {
            target: oviora
            function onMouseAimChanged() {
                if (mouseAimHint.captured === oviora.mouseAimCaptured) return
                mouseAimHint.captured = oviora.mouseAimCaptured
                // Let go for a menu or a page over the picture: nothing to say
                if (!oviora.mouseAimOn || root.page !== "min" || root.streamMenuShown || dialogs.dialog !== "") { mouseAimHint.opacity = 0; return }
                mouseAimHint.opacity = 1
                mouseAimHintTimer.restart()
                oviora.logUi("ui-mouse-aim", mouseAimHint.captured ? "hint: captured" : "hint: released")
            }
        }
    }
    // Sharing 2.0, the owner's side: a friend asks to connect (Bağlanmadan önce bana sor) or for more time. The
    // oldest ask is shown with its answers, on every page and over the stream (cut out of the video window).
    Rectangle {
        id: askCard
        objectName: "ask-card"
        // --dialog askcard (test runs) shows two sample asks; nothing is sent
        readonly property bool testAsks: typeof vyTest !== "undefined" && vyTest.dialog === "askcard"
        readonly property var asks: testAsks ? [{ kind: "extend", guest: "deniz", console: "Salon PS5" }, { kind: "lease", guest: "ece", console: "Salon PS5" }]
            : typeof platform !== "undefined" && platform ? platform.pendingAsks : []
        readonly property var ask: asks.length ? asks[0] : null
        readonly property bool videoCover: visible
        property real coverRadius: radius
        z: 480
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 24
        anchors.bottomMargin: 24
        width: Math.min(380, parent.width - 48)
        height: askColumn.implicitHeight + 32
        radius: Tema.radiusPanel
        color: Tema.surface
        border.width: 1
        border.color: Tema.accent
        opacity: ask ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        ColumnLayout {
            id: askColumn
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: !askCard.ask ? "" : askCard.ask.kind === "lease" ? qsTr("Bağlanma isteği") : qsTr("Ek süre isteği")
                font.family: Tema.body
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Tema.text
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: !askCard.ask ? "" : askCard.ask.kind === "lease"
                    ? qsTr("%1, %2 adlı PS5'ine bağlanmak istiyor.").arg(askCard.ask.guest).arg(askCard.ask.console)
                    : qsTr("%1, %2 adlı PS5'inde oynamak için ek süre istiyor.").arg(askCard.ask.guest).arg(askCard.ask.console)
                font.family: Tema.body
                font.pixelSize: 14
                color: Tema.muted
            }
            Text {
                textFormat: Text.PlainText
                visible: askCard.asks.length > 1
                text: qsTr("Bekleyen %1 istek daha var").arg(askCard.asks.length - 1)
                font.family: Tema.body
                font.pixelSize: 12
                color: Tema.faint
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                VButton {
                    text: qsTr("Reddet")
                    onClicked: platform.answerAsk(0, false, 30)
                }
                VButton {
                    visible: !!askCard.ask && askCard.ask.kind === "extend"
                    text: qsTr("+1 saat")
                    onClicked: platform.answerAsk(0, true, 60)
                }
                VButton {
                    primary: true
                    text: !!askCard.ask && askCard.ask.kind === "extend" ? qsTr("+30 dk") : qsTr("İzin ver")
                    onClicked: platform.answerAsk(0, true, 30)
                }
            }
        }
    }
    // Sharing 2.0, the guest's side: another game than the owner allows is open; back to it before the countdown ends
    Rectangle {
        id: gameWarning
        objectName: "game-warning"
        readonly property var w: typeof platform !== "undefined" && platform ? platform.sessionGameWarning : ({})
        readonly property bool videoCover: visible
        property real coverRadius: radius
        z: 485
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 24
        width: Math.min(560, parent.width - 48)
        height: gameWarningText.implicitHeight + 28
        radius: Tema.radiusPanel
        color: Tema.danger
        opacity: root.page === "min" && w && w.current ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        Text {
            id: gameWarningText
            anchors.fill: parent
            anchors.margins: 14
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            // checking: the time ran out, the server waits for a fresh game reading (going back takes a while to show)
            text: !gameWarning.w || !gameWarning.w.current ? ""
                : gameWarning.w.checking
                ? qsTr("Oyun kontrol ediliyor… %1 oyunundaysan bağlantı açık kalır.").arg((gameWarning.w.allowed || []).join(", "))
                : qsTr("%1 bu paylaşımda izinli değil. %2 oyununa dön — %3 saniye içinde bağlantı kapanacak. Dönüşün görülmesi biraz sürebilir.")
                    .arg(gameWarning.w.current).arg((gameWarning.w.allowed || []).join(", ")).arg(gameWarning.w.seconds_left)
            font.family: Tema.body
            font.pixelSize: 15
            font.weight: Font.DemiBold
            color: "white"
        }
    }
    // A friend's message while the stream's picture is shown (MessageCenter.incoming, only for a
    // conversation not open): a small card in the top-right corner, cut out of the video window like
    // the other notices. A click opens the stream menu on that conversation (Ctrl+O › Mesajlar).
    // The home's own toast stays quiet meanwhile (OvioraHome.streamShown).
    Rectangle {
        id: messageToast
        objectName: "message-toast"
        property string from: ""
        property string preview: ""
        // A group's message: its id and name ("Grup adı · gönderen" on top); 0 for a friend's
        property int group: 0
        property string groupName: ""
        property bool shown: false
        // --dialog messagetoast (test runs) shows it without a stream
        property bool forced: false
        // Not over the menu's own chat panel, which shows the message already
        readonly property bool allowed: (forced || root.page === "min")
            && !(root.streamMenuShown && streamMenuLoader.item && streamMenuLoader.item.chatShown)
        function show(f, p, g, gn) {
            from = f
            group = g || 0
            groupName = gn || ""
            preview = (p || "").replace(/\s+/g, " ")
            shown = true
            messageToastTimer.restart()
            oviora.logUi("ui-message-toast", "shown")
        }
        readonly property bool videoCover: visible
        property real coverRadius: radius
        z: 470
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 24
        // Under the weak-connection pill when that is up, under the statistics panel otherwise
        anchors.topMargin: weakLink.visible ? weakLink.y + weakLink.height + 12
            : (statsOverlay.wanted && statsOverlay.area.height > 0 ? statsOverlay.area.y + statsOverlay.area.height + 12 : 24)
        width: Math.min(360, parent.width - 48)
        height: messageToastColumn.implicitHeight + 28
        radius: Tema.radiusPanel
        color: Tema.surface
        border.width: 1
        border.color: messageToastArea.containsMouse ? Tema.accent : Tema.lineStrong
        opacity: shown && allowed ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        transform: Translate {
            x: messageToast.shown ? 0 : 18
            Behavior on x { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
        }
        Timer { id: messageToastTimer; interval: 5000; onTriggered: if (!messageToast.forced) messageToast.shown = false }
        MouseArea {
            id: messageToastArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            // Held while the pointer is on it, so it does not go from under a click
            onContainsMouseChanged: if (containsMouse) messageToastTimer.stop(); else if (messageToast.shown) messageToastTimer.restart()
            onClicked: {
                messageToast.shown = false
                if (messageToast.group) root.ensureStreamMenu().openGroupChat(messageToast.group)
                else root.ensureStreamMenu().openChat(messageToast.from)
            }
        }
        ColumnLayout {
            id: messageToastColumn
            anchors.fill: parent
            anchors.margins: 14
            spacing: 3
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Rectangle { implicitWidth: 8; implicitHeight: 8; radius: 4; color: Tema.accent }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: messageToast.group ? messageToast.groupName + " · " + messageToast.from : messageToast.from
                    elide: Text.ElideRight
                    font.family: Tema.body
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Tema.text
                }
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: messageToast.preview
                elide: Text.ElideRight
                maximumLineCount: 1
                font.family: Tema.body
                font.pixelSize: 14
                color: Tema.muted
            }
            Label {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                Layout.topMargin: 3
                text: qsTr("Cevaplamak için Ctrl+Enter")
                font.family: Tema.body
                font.pixelSize: 12
                color: Tema.accentText
            }
        }
    }
    Connections {
        target: messages
        function onIncoming(from, preview, group, groupName) { if (root.page === "min") messageToast.show(from, preview, group, groupName) }
    }
    Component.onCompleted: {
        startTestMask()
        applyStartupSize()
        // Open on the configured page: the user's saved preference, or the
        // override a test passes on the command line.
        if (oviora.initialPage.length > 0) root.goPage(oviora.initialPage)
        if (root.testAspect > 0) root.fitToFilm(root.testAspect)
        if (root.test.dialog === "pair") root.openPairDialog()
        else if (root.test.dialog === "editconsole") testEditConsole.start()
        else if (root.test.dialog === "weaklink") weakLink.forced = true
        else if (root.test.dialog === "lossnote") { lossNote.forced = true; lossNote.show(0.052) }
        else if (root.test.dialog === "fghint") { fgHint.forced = true; fgHint.shown = true }
        else if (root.test.dialog === "messagetoast") { messageToast.forced = true; messageToast.show("deneme", "Selam, bu akşam bir maç atalım mı? Hazır olunca haber ver.") }
        else if (root.test.dialog === "rest") testRest.start()
        else if (root.test.dialog === "update") { const n = []; n.push("Yeni açılış ve bağlanma ekranları, daha akıcı yayın", ""); for (let i = 1; i <= 18; ++i) n.push("- Deneme maddesi " + i + ": uzun bir sürüm notu satırı, ekrana sığmayacak kadar çok madde olduğunda kart kendi içinde kayar."); ovHome.updatePreview = n.join("\n") }
        else if (root.test.dialog === "psnlookup") { root.openPairDialog(); pairDialogLoader.item.startLookup() }
        else if (root.test.dialog === "restask") { restAsk.open(); restAsk.forceActiveFocus() }
        else if (root.test.dialog === "streammenu") testStreamMenu.start()
        else if (root.test.dialog !== undefined) dialogs.open(root.test.dialog)
        videoHost.syncRect()
        if (root.test.fullBar !== undefined) testFullTimer.start()
    }
    // Test runs: --dialog streammenu [--section game|picture|keys|chat] opens the stream menu
    Timer {
        id: testStreamMenu; interval: 1500
        // --section game | settings | picture | picture:<tab> (quality, fg, color, audio, display) | sound | subs | keys | chat | chat:<friend> | chat:#<group id>
        onTriggered: {
            const sec = root.test.section || "game"
            const sm = root.ensureStreamMenu()
            sm.open()
            if (sec.indexOf("picture") === 0) sm.openPicture(sec.indexOf(":") > 0 ? sec.split(":")[1] : "quality")
            else if (sec === "keys" || sec === "sound" || sec === "subs" || sec === "subsphone" || sec === "settings") sm.section = sec
            else if (sec.indexOf("chat:#") === 0) sm.openGroupChat(parseInt(sec.substring(6)))
            else if (sec.indexOf("chat") === 0) sm.openChat(sec.indexOf(":") > 0 ? sec.substring(sec.indexOf(":") + 1) : "")
        }
    }
    // --full-bar shown|hidden: enter fullscreen with the control window held shown
    // or held hidden, for the present-timing comparison (G2.5).
    Timer { id: testFullTimer; interval: 1500; onTriggered: { root.toggleFullscreen(); if (root.test.fullBar === "hidden") root.fullControls = false } }

    // --test-hole <s>: the black hole that many seconds into a stream; the window grabbed at a few moments
    Timer {
        id: testHole
        property bool done: false
        interval: (root.test.hole || 0) * 1000
        running: (root.test.hole || 0) > 0 && oviora.hasSource && !done
        onTriggered: { done = true; root.disconnectWithHole(); testHoleGrab.start() }
    }
    Timer {
        id: testHoleGrab
        property int n: 0
        interval: 500
        repeat: true
        onTriggered: {
            const k = n++
            root.contentItem.grabToImage(r => r.saveToFile("C:/claude/inceleme/ekran/delik-" + k + ".png"))
            oviora.logUi("test-hole", "grab " + k + " page=" + root.page + " shown=" + root.shownPage + " hole=" + blackHole.shown
                + " t=" + blackHole.t.toFixed(2) + " lift=" + blackHole.lift.toFixed(2) + " source=" + oviora.hasSource + " active=" + (oviora.ps5.active === true)
                + " stage=" + ((oviora.connection && oviora.connection.stage) || ""))
            if (n > 14) stop()
        }
    }
    // ── The black hole (Bağlantıyı kes): the last sharp picture of the game glides into a point while
    // the dark closes in from the edges and a bright ring burns at the centre; the session ends
    // underneath, then the dark lifts off the home, whose sections rise in. Transforms only: the
    // interface may be drawn in software (no shader effects there). ──
    Item {
        id: blackHole
        anchors.fill: parent
        z: 950
        visible: shown
        property bool shown: false
        property bool running: false
        readonly property bool videoCover: visible
        property string picture: ""
        property real t: 0         // 0..1 the fall into the hole
        property real lift: 0      // 0..1 the dark lifting off the home
        function begin() {
            running = true
            picture = ""
            t = 0
            lift = 0
            oviora.captureSnapshot()
            snapshotWait.restart()
        }
        function go(url) {
            if (!running || shown) return
            snapshotWait.stop()
            picture = url
            shown = true
            if (streamMenuLoader.item) streamMenuLoader.item.hide()
            fall.restart()
        }
        Connections {
            target: oviora
            function onSnapshotReady(url) { blackHole.go(url) }
        }
        // No picture within the moment: the effect plays on the dark alone
        Timer { id: snapshotWait; interval: 600; onTriggered: blackHole.go("") }
        MouseArea { anchors.fill: parent; enabled: blackHole.visible; acceptedButtons: Qt.AllButtons; hoverEnabled: true }

        // The dark (the home's own ground, not pure black): closes in as the picture falls, lifts off the home
        Rectangle {
            anchors.fill: parent
            color: Tema.bg
            opacity: Math.min(1, blackHole.t * 1.6) * (1 - blackHole.lift)
        }
        // The picture: glides into the centre without turning (as PS Portal leaves a stream), easing in
        // and out, and fades over the last stretch
        Image {
            id: holePicture
            anchors.centerIn: parent
            width: parent.width
            height: parent.height
            source: blackHole.picture
            fillMode: Image.PreserveAspectFit
            smooth: true
            cache: false
            asynchronous: false
            readonly property real e: blackHole.t < 0.5 ? 4 * Math.pow(blackHole.t, 3) : 1 - Math.pow(-2 * blackHole.t + 2, 3) / 2
            scale: Math.max(0.001, 1 - 0.985 * e)
            opacity: blackHole.t < 0.7 ? 1 : Math.max(0, 1 - (blackHole.t - 0.7) / 0.3)
            // Its edges melt into the dark (no hard frame), and it darkens as it goes
            readonly property real edge: Math.min(paintedWidth, paintedHeight) * 0.16
            Item {
                anchors.centerIn: parent
                width: holePicture.paintedWidth
                height: holePicture.paintedHeight
                Rectangle {
                    anchors { left: parent.left; right: parent.right; top: parent.top }
                    height: holePicture.edge
                    gradient: Gradient { GradientStop { position: 0; color: "#ff000000" } GradientStop { position: 1; color: "#00000000" } }
                }
                Rectangle {
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: holePicture.edge
                    gradient: Gradient { GradientStop { position: 0; color: "#00000000" } GradientStop { position: 1; color: "#ff000000" } }
                }
                Rectangle {
                    anchors { top: parent.top; bottom: parent.bottom; left: parent.left }
                    width: holePicture.edge
                    gradient: Gradient { orientation: Gradient.Horizontal; GradientStop { position: 0; color: "#ff000000" } GradientStop { position: 1; color: "#00000000" } }
                }
                Rectangle {
                    anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
                    width: holePicture.edge
                    gradient: Gradient { orientation: Gradient.Horizontal; GradientStop { position: 0; color: "#00000000" } GradientStop { position: 1; color: "#ff000000" } }
                }
                Rectangle {
                    anchors.fill: parent
                    color: "#000000"
                    opacity: 0.55 * holePicture.e
                }
            }
        }
        // The event horizon: a ring that wakes at the centre, swallows the last of the picture and closes
        Rectangle {
            anchors.centerIn: parent
            // Follows the picture: a third of its height as it sinks, waking only in the second half
            readonly property real k: blackHole.t < 0.45 ? 0 : blackHole.t < 0.8 ? (blackHole.t - 0.45) / 0.35 : Math.max(0, 1 - (blackHole.t - 0.8) / 0.2)
            width: Math.max(14, holePicture.paintedHeight * holePicture.scale * 0.36)
            height: width
            radius: width / 2
            color: "#05040a"
            border.width: 3 + 5 * k
            border.color: Qt.rgba(0.66, 0.59, 1.0, 0.25 + 0.6 * k)
            opacity: k * (1 - blackHole.lift)
            Rectangle {
                anchors.centerIn: parent
                width: parent.width + 26 * parent.k
                height: width
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: Qt.rgba(1.0, 0.68, 0.18, 0.35 * parent.k)
            }
        }
        SequentialAnimation {
            id: fall
            NumberAnimation { target: blackHole; property: "t"; from: 0; to: 0.75; duration: 712; easing.type: Easing.Linear }
            // The picture is small by now: the session ends under the last of the fall, so the home is
            // (nearly) ready when the picture is gone and the dark lifts at once
            ScriptAction { script: { oviora.ps5Cancel(); homeWait.restart() } }
            NumberAnimation { target: blackHole; property: "t"; from: 0.75; to: 1; duration: 238; easing.type: Easing.Linear }
            ScriptAction { script: blackHole.picture = "" }
        }
        // The dark lifts once the home page is really there (the stream's end turns the page), or after 3 s
        Timer {
            id: homeWait
            interval: 50
            repeat: true
            property int ticks: 0
            onTriggered: {
                ticks++
                if (blackHole.t >= 1 && ((root.page === "home" && root.shownPage === "home" && !oviora.hasSource) || ticks > 60)) {
                    stop()
                    ticks = 0
                    if (root.page === "home") ovHome.enter()
                    lifting.restart()
                }
            }
        }
        SequentialAnimation {
            id: lifting
            NumberAnimation { target: blackHole; property: "lift"; from: 0; to: 1; duration: 520; easing.type: Easing.OutCubic }
            ScriptAction { script: { blackHole.shown = false; blackHole.running = false; blackHole.t = 0; blackHole.lift = 0 } }
        }
    }

    // ── PS5 connection states: connecting screen, error card, login PIN, protected content ──
    ConnectingView {
        id: connectingView
        anchors.fill: parent
        z: 460
        onMinPage: root.page === "min"
    }

    Rectangle {
        id: cantDisplayCard
        objectName: "cant-display-card"
        readonly property bool active: root.page === "min" && oviora.hasSource
            && !!(oviora.connection && oviora.connection.cantDisplay)
            && !root.streamMenuShown && !connectingView.visible && !pinCard.visible && !errorCard.visible
        opacity: active ? 1 : 0
        visible: opacity > 0
        readonly property bool videoCover: visible
        property real coverRadius: radius
        Behavior on opacity { NumberAnimation { duration: 250 } }
        anchors.centerIn: parent
        z: 25
        width: Math.min(parent.width - 64, 580)
        height: cantDisplayText.implicitHeight + 56
        radius: Tema.radiusPanel
        color: Tema.surface
        border.width: 1
        border.color: Tema.lineStrong
        Text {
            textFormat: Text.PlainText
            id: cantDisplayText
            anchors.centerIn: parent
            width: parent.width - 56
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("Ekrandaki içerik uzaktan oynatmada gösterilemiyor.")
            font.family: Tema.display
            font.pixelSize: 22
            font.weight: Font.DemiBold
            font.letterSpacing: -0.3
            color: Tema.text
        }
    }

    Rectangle {
        id: errorCard
        objectName: "connection-error-card"
        anchors.fill: parent
        z: 480
        color: Tema.scrim
        visible: ((oviora.connection && oviora.connection.stage) || "") === "failed" && root.page !== "set"
        readonly property bool videoCover: visible
        property real inT: 1
        opacity: inT
        scale: 0.97 + 0.03 * inT
        onVisibleChanged: if (visible && !Theme.reduced) { inT = 0; errorIn.restart() }
        NumberAnimation { id: errorIn; target: errorCard; property: "inT"; to: 1; duration: 420; easing.bezierCurve: Theme.springSoft }
        MouseArea { anchors.fill: parent; enabled: errorCard.visible; acceptedButtons: Qt.AllButtons; hoverEnabled: true }
        Shortcut {
            sequence: "Esc"
            enabled: errorCard.visible
            onActivated: { oviora.clearConnectionError(); root.goPage("home") }
        }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 64, 560)
            height: errorCol.implicitHeight + 64
            radius: Tema.radiusPanel
            color: Tema.surface
            border.width: 1
            border.color: Tema.lineStrong
            Column {
                id: errorCol
                anchors.centerIn: parent
                width: parent.width - 64
                spacing: 14
                OvioraMark {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 64
                    height: 64
                }
                Item { width: 1; height: 4 }
                Label {
                    textFormat: Text.PlainText
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: qsTr("Bağlantı kurulamadı")
                    font.family: Tema.display
                    font.pixelSize: 28
                    font.weight: Font.DemiBold
                    color: Tema.text
                }
                Label {
                    textFormat: Text.PlainText
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: (oviora.connection && oviora.connection.detail) || ""
                    visible: text.length > 0
                    font.family: Tema.body
                    font.pixelSize: 15
                    lineHeight: 1.25
                    color: Tema.muted
                }
                Item { width: 1; height: 8 }
                S.PillButton {
                    id: errorBackButton
                    anchors.horizontalCenter: parent.horizontalCenter
                    kind: "primary"
                    navUpDown: false
                    text: qsTr("Ana ekrana dön")
                    onClicked: { oviora.clearConnectionError(); root.goPage("home") }
                }
                S.PillButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: errorCard.visible && typeof root.lastConnect === "function"
                    navUpDown: false
                    text: qsTr("Yeniden bağlan")
                    onClicked: {
                        const fn = root.lastConnect
                        oviora.clearConnectionError()
                        if (fn) fn()
                    }
                }
                S.PillButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: errorCard.visible && platform.configured && platform.status === "active"
                    kind: "quiet"
                    navUpDown: false
                    text: qsTr("Sorun bildir")
                    onClicked: { oviora.clearConnectionError(); root.openProblemReport() }
                }
                S.PillButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: errorCard.visible
                    kind: "quiet"
                    navUpDown: false
                    text: qsTr("Discord'da sor")
                    onClicked: Qt.openUrlExternally("https://discord.gg/XRPXMCqCqe")
                }
            }
        }
    }

    Rectangle {
        id: pinCard
        objectName: "session-pin-card"
        anchors.fill: parent
        z: 490
        color: Tema.scrim
        visible: ((oviora.connection && oviora.connection.stage) || "") === "pin"
        readonly property bool videoCover: visible
        property real inT: 1
        opacity: inT
        scale: 0.97 + 0.03 * inT
        onVisibleChanged: if (visible) {
            pinField.text = ""
            pinField.forceActiveFocus()
            if (!Theme.reduced) { inT = 0; pinIn.restart() }
        }
        NumberAnimation { id: pinIn; target: pinCard; property: "inT"; to: 1; duration: 420; easing.bezierCurve: Theme.springSoft }
        MouseArea { anchors.fill: parent; enabled: pinCard.visible; acceptedButtons: Qt.AllButtons; hoverEnabled: true }
        Shortcut {
            sequence: "Esc"
            enabled: pinCard.visible
            onActivated: oviora.ps5Cancel()
        }
        function submit() {
            if (pinField.acceptableInput) oviora.ps5SendLoginPin(pinField.text)
        }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 64, 460)
            height: pinCol.implicitHeight + 56
            radius: Tema.radiusPanel
            color: Tema.surface
            border.width: 1
            border.color: Tema.lineStrong
            ColumnLayout {
                id: pinCol
                anchors.centerIn: parent
                width: parent.width - 56
                spacing: 14
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Konsol giriş PIN'i")
                    font.family: Tema.display
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                    color: Tema.text
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("PS5'te bir giriş PIN'i açık. Dört haneli PIN'i yaz.")
                    font.family: Tema.body
                    font.pixelSize: Tema.smallSize
                    color: Tema.muted
                }
                S.InputField {
                    id: pinField
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    placeholderText: qsTr("PIN")
                    implicitHeight: 54
                    font.pixelSize: 22
                    font.letterSpacing: 6
                    validator: RegularExpressionValidator { regularExpression: /[0-9]{4}/ }
                    Keys.onReturnPressed: if (acceptableInput) pinCard.submit()
                    Keys.onEnterPressed: if (acceptableInput) pinCard.submit()
                    Keys.onEscapePressed: oviora.ps5Cancel()
                }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    Layout.topMargin: 4
                    spacing: 10
                    S.PillButton {
                        kind: "quiet"
                        navUpDown: false
                        focusPolicy: Qt.NoFocus
                        text: qsTr("Vazgeç")
                        onClicked: oviora.ps5Cancel()
                    }
                    S.PillButton {
                        kind: "primary"
                        navUpDown: false
                        focusPolicy: Qt.NoFocus
                        text: qsTr("Gir")
                        enabled: pinField.acceptableInput
                        onClicked: pinCard.submit()
                    }
                }
            }
        }
    }

    ConsoleKeyboardDialog {
        id: consoleKeyboardDialog
        z: 495
        active: root.page === "min" && !!(oviora.connection && oviora.connection.keyboard)
    }

    // ── Opening scene: the mark (amber arc travelling round) while the home page gets ready ──
    Rectangle {
        id: splash
        objectName: "splash"
        anchors.fill: parent
        z: 1000
        color: Tema.bg
        readonly property bool testLaunch: {
            if (Object.keys(root.test).length > 0) return true
            const args = Qt.application.arguments || []
            for (let i = 1; i < args.length; ++i) {
                const a = String(args[i])
                if (a === "--page" || a === "--section" || a === "--dialog"
                    || a === "--exit-after" || a === "--size" || a === "--aspect"
                    || a === "--full-bar" || a === "--motion-probe"
                    || a === "--video-probe" || a.indexOf("--test-") === 0)
                    return true
            }
            return false
        }
        opacity: testLaunch ? 0 : 1
        visible: opacity > 0
        readonly property bool videoCover: visible
        property bool minElapsed: false
        property bool dismissed: testLaunch
        readonly property bool homeReady: typeof platform === "undefined" || !platform.configured
                                          || platform.loaded || (platform.status !== "active" && !platform.busy)
        function tryDismiss() {
            if (!dismissed && minElapsed && homeReady) dismiss()
        }
        function dismiss() {
            if (dismissed) return
            dismissed = true
            splashFade.start()
            // The home rises in behind the opening as it goes
            if (root.page === "home") ovHome.enter()
        }
        onHomeReadyChanged: tryDismiss()

        // Takes clicks and keys only while it is up
        focus: visible
        Keys.onPressed: (event) => { event.accepted = true }
        Keys.onReleased: (event) => { event.accepted = true }
        Keys.onShortcutOverride: (event) => { event.accepted = true }
        MouseArea {
            anchors.fill: parent
            enabled: splash.visible
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
        }

        OvioraAnimasyon {
            anchors.fill: parent
            showTitle: true
            status: qsTr("Açılıyor…")
        }

        // The developer's word mark, quiet, at the bottom
        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12
            height: 17
            width: height * 512 / 192
            sourceSize: Qt.size(width * 2, height * 2)
            source: "ayar/arfep-lettermark-light.svg"
            fillMode: Image.PreserveAspectFit
            smooth: true
            opacity: 0.38
        }

        // Counted from the first frame on screen, not from loading: the window shows a while
        // after the scene is built, and the splash was gone before anyone saw it
        Connections {
            target: root
            enabled: !splash.testLaunch && !splashMinTimer.running && !splash.minElapsed
            function onFrameSwapped() { splashMinTimer.start(); splashMaxTimer.start() }
        }
        Timer {
            id: splashMinTimer
            interval: Theme.reduced ? 400 : 1200
            onTriggered: {
                splash.minElapsed = true
                splash.tryDismiss()
            }
        }
        Timer {
            id: splashMaxTimer
            interval: Theme.reduced ? 1200 : 3000
            onTriggered: splash.dismiss()
        }
        ParallelAnimation {
            id: splashFade
            NumberAnimation { target: splash; property: "opacity"; to: 0; duration: Theme.d(520); easing.type: Easing.InOutCubic }
            NumberAnimation { target: splash; property: "scale"; to: 1.05; duration: Theme.d(560); easing.type: Easing.OutCubic }
        }
    }
}
