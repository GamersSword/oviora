// Oviora QML front end.
//
// The window is Qt; the video is not. The engine presents into a native child
// HWND that Qt hosts but never composites (measured in S3: 499/499 presents at
// 0.205 ms while the QML scene animated at its full 100 Hz panel rate). The QML
// scene draws the interface around and above that window.
//
// The video child window is created here, before the QML engine loads, so the
// first frame has somewhere to go. Its geometry is driven by the QML item named
// "videoHost": when that item moves or resizes, this code moves the native
// window to match. That keeps the layout in QML (where it belongs) and the
// pixels in D3D12 (where the latency budget lives).
#include <QApplication>
#include <QProcess>
#include <QThread>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include "../oviora/resource.h"
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QKeyEvent>
#include <QTimer>
#include <QDateTime>
#include <QRegularExpression>
#include <atomic>
#include <QPointer>
#include <QHash>
#include <QVariantMap>
#include <QList>
#include <QRect>
#include <QtCore/private/qabstractanimation_p.h>
#include <QDebug>
#include <QAbstractNativeEventFilter>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QSysInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <dbghelp.h>
#include <d3d12.h>
#pragma comment(lib, "comctl32.lib")

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <thread>
#include <vector>
#include <QEvent>

#include "oviora/Log.h"
#include "oviora/RuntimePaths.h"
#include "oviora/engine/EngineController.h"
#include "oviora/engine/PresentationGeometry.h"
#include "oviora/gfx/PresentationHooks.h"
#include "oviora/gfx/NvidiaAppProfile.h"
#include "oviora/pipeline/AmdHipPreflight.h"
#include "oviora/ui/InterfaceRenderer.h"
#include "oviora/ui/QmlDataDirectory.h"
#include "oviora/ui/QmlDiskCache.h"
#include "oviora/ui/QmlPlayerBridge.h"
#include <qpa/qwindowsysteminterface.h>
#include "oviora/platform/OvioraServer.h"
#include "oviora/platform/MessageCenter.h"
#include "SubtitleTranslator.h"
#include "oviora/platform/PsnRelayBridge.h"
#include "oviora/platform/PsnWebView.h"
#include "oviora/platform/PsnAccountLookup.h"
#include "oviora/platform/DiscordPresence.h"
#include "oviora/platform/SubtitleScreen.h"
#include "oviora/platform/IzletController.h"
#include "oviora/ui/MenuBackdropStore.h"
#include "oviora/remoteplay/ChiakiBackend.h"
#include "oviora/remoteplay/WatchRoute.h"

using namespace oviora;

namespace {
HWND g_video = nullptr;
bool g_videoCursorHidden = false;
QList<QRect> g_videoCoverHoles;
int g_videoInsetBand = 0;
// The region is emptied until the new source presents (blankVideo below)
bool g_videoBlanked = false;

// The video is a foreign HWND, not a Qt item. Match the QML cursor policy here
// without a repaint, a polling timer or ShowCursor's process-wide counter.
bool pointerOverVideo() {
    if (!g_video || !IsWindowVisible(g_video)) return false;
    const HWND parent = GetParent(g_video);
    if (!parent || GetForegroundWindow() != parent) return false;
    POINT p{};
    if (!GetCursorPos(&p)) return false;
    const HWND hit = WindowFromPoint(p);
    if (hit != g_video && hit != parent) return false;
    ScreenToClient(g_video, &p);
    RECT client{};
    GetClientRect(g_video, &client);
    if (!PtInRect(&client, p) || g_videoBlanked) return false;
    if (g_videoCoverHoles.isEmpty() && g_videoInsetBand > 0) {
        if (p.y < g_videoInsetBand || p.y >= client.bottom - g_videoInsetBand) return false;
    }
    // Floating QML controls are holes in this region; never override their
    // pointing/text cursors, nor any other application's cursor.
    for (qsizetype i = 0; i + 1 < g_videoCoverHoles.size(); i += 2) {
        const QRect& h = g_videoCoverHoles[i];
        if (h.contains(p.x, p.y)) return false;
    }
    return true;
}

void syncVideoCursor(bool hidden) {
    if (g_videoCursorHidden == hidden) return;
    g_videoCursorHidden = hidden;
    const bool overVideo = pointerOverVideo();
    if (overVideo) SetCursor(hidden ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
    CURSORINFO cursor{sizeof(cursor)};
    const bool known = GetCursorInfo(&cursor) != 0;
    oviora::log::info("ui-cursor", std::format("hidden={} overVideo={} applied={} cursorKnown={} cursorShowing={}",
        hidden, overVideo, overVideo, known, known && (cursor.flags & CURSOR_SHOWING) != 0));
}
enum class VideoProbeMode { Normal, Hidden, NoRegion, CoversOnly };
VideoProbeMode g_videoProbe = VideoProbeMode::Normal;
bool g_videoHiddenByProbe = false;
// Set while a new source is opening: the window keeps the previous session's last
// frame until the new one presents, so its region is emptied instead. A slow PS5
// connect after a capture card left the card's last frame up for 20 s and the
// window looked frozen (field report 2026-10-01).

void blankVideo(const char* why) {
    if (!g_video || g_videoBlanked) return;
    g_videoBlanked = true;
    g_videoCoverHoles.clear();
    g_videoInsetBand = 0;
    const int result = SetWindowRgn(g_video, CreateRectRgn(0, 0, 0, 0), TRUE);
    oviora::log::info("qml-window", std::format("blank video until the new source presents ({}) result={}", why, result));
}

const char* videoProbeName() {
    switch (g_videoProbe) {
    case VideoProbeMode::Hidden: return "hidden";
    case VideoProbeMode::NoRegion: return "no-region";
    case VideoProbeMode::CoversOnly: return "covers-only";
    default: return "normal";
    }
}

void logVideoWindowState(QQuickWindow* window, QQuickItem* host, const char* event) {
    if (!window || !host || !g_video) return;
    const HWND parent = GetParent(g_video);
    RECT parentClient{}, client{}, screen{};
    GetClientRect(parent, &parentClient);
    GetClientRect(g_video, &client);
    GetWindowRect(g_video, &screen);
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    const int regionType = GetWindowRgn(g_video, region);
    RECT regionBox{};
    const int regionBoxType = GetRgnBox(region, &regionBox);
    DeleteObject(region);
    const QPointF scene = host->mapToScene(QPointF(0, 0));
    oviora::log::info("qml-window", std::format(
        "{} probe={} root={}x{} dpr={:.3f} host=({:.1f},{:.1f}) {:.1f}x{:.1f} "
        "parent={} parentClient={}x{} client={}x{} screen=({},{}) {}x{} "
        "style=0x{:X} visible={} regionType={} regionBoxType={} regionBox=({},{}) {}x{}",
        event, videoProbeName(), window->width(), window->height(), window->devicePixelRatio(),
        scene.x(), scene.y(), host->width(), host->height(), parent != nullptr,
        parentClient.right, parentClient.bottom, client.right, client.bottom,
        screen.left, screen.top, screen.right - screen.left, screen.bottom - screen.top,
        static_cast<unsigned long long>(GetWindowLongPtrW(g_video, GWL_STYLE)),
        IsWindowVisible(g_video) != 0, regionType, regionBoxType,
        regionBox.left, regionBox.top, regionBox.right - regionBox.left,
        regionBox.bottom - regionBox.top));
}

LRESULT CALLBACK videoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // The engine owns this window's pixels. Qt must not paint it, or the two
    // swapchains fight and we lose the whole point of the native path.
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) { ValidateRect(h, nullptr); return 0; }
    // The pointer belongs to the QML layer under the picture (the dock's hot zone
    // at the top edge, the cinema controls): pass hit testing through to it.
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT && g_videoCursorHidden && pointerOverVideo()) {
        SetCursor(nullptr);
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND createVideoWindow() {
    const wchar_t* cls = L"OvioraQmlVideoHost";
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = videoProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = cls;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);
    // Created as an unrealised popup, then reparented into the QML window before
    // anything reads its size. Creating it as WS_CHILD with no parent does not
    // work: Windows refuses with ERROR_CANNOT_MAKE (1406), which is what left the
    // video window invalid and the swapchain at 1x1. WS_POPUP with no WS_VISIBLE
    // creates cleanly; SetParent plus the child style below turns it into a child.
    HWND created = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_TOOLWINDOW, cls, L"",
                                   WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                   0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (created == nullptr) {
        oviora::log::error("qml", std::format("CreateWindowExW failed err={}", GetLastError()));
    }
    return created;
}

// Places the native video window at the QML item's position. Qt reports the
// item's geometry in scene coordinates, which for a child of the window's
// content item are already window-relative.
bool syncVideoGeometry(QQuickWindow* window, QQuickItem* host) {
    if (!window || !host || !g_video || GetParent(g_video) != reinterpret_cast<HWND>(window->winId()))
        return false;
    const QPointF topLeft = host->mapToScene(QPointF(0, 0));
    const qreal dpr = window->devicePixelRatio();
    if (host->width() < 1 || host->height() < 1 ||
        topLeft.x() + host->width() <= 0 || topLeft.y() + host->height() <= 0 ||
        topLeft.x() >= window->width() || topLeft.y() >= window->height()) {
        if (IsWindowVisible(g_video)) ShowWindow(g_video, SW_HIDE);
        return false;
    }
    // Keep partial QML viewports stable while they animate. A viewport that
    // spans the window must use its actual physical client extent: Qt rounds
    // the HWND at fractional DPI, whereas flooring width*dpr can leave a
    // one-pixel strip outside the video (for example 642 DIP at 125%).
    const bool aligned = window->property("gameAligned").toBool();
    const auto physical = [aligned](qreal value) { return int(aligned ? std::round(value) : std::floor(value)); };
    const int x = physical(topLeft.x() * dpr);
    const int y = physical(topLeft.y() * dpr);
    int w = std::max(1, physical(host->width() * dpr));
    int h = std::max(1, physical(host->height() * dpr));
    RECT parentClient{};
    if (GetClientRect(reinterpret_cast<HWND>(window->winId()), &parentClient)) {
        if (std::abs(topLeft.x()) < 0.001 && std::abs(host->width() - window->width()) < 0.001)
            w = std::max(1L, parentClient.right);
        if (std::abs(topLeft.y()) < 0.001 && std::abs(host->height() - window->height()) < 0.001)
            h = std::max(1L, parentClient.bottom);
    }
    // A film-fitted cinema window can have no fractional HWND pixels. Tell
    // the presenter the exact rounding bound; real bars/zoom/fullscreen retain
    // their normal mapping. Keep this synchronized even when the rect is equal.
    const qreal aspect = window->property("filmAspect").toReal();
    const bool filmWindow = window->property("cinema").toBool() &&
        !window->property("screenSized").toBool() && aspect > 0 &&
        std::abs(topLeft.x()) < 0.001 && std::abs(topLeft.y()) < 0.001 &&
        std::abs(host->width() - window->width()) < 0.001 &&
        std::abs(host->height() - window->height()) < 0.001 &&
        std::abs(window->height() - window->width() / aspect) <= 0.501;
    const uintptr_t roundingUnits = filmWindow
        ? uintptr_t(std::ceil((0.5 * dpr + 0.5 + 0.5 / aspect) * 1024)) : 0;
    constexpr auto roundingProperty = oviora::engine::kFilmPixelRoundingProperty;
    if (reinterpret_cast<uintptr_t>(GetPropW(g_video, roundingProperty)) != roundingUnits) {
        if (roundingUnits && !SetPropW(g_video, roundingProperty, reinterpret_cast<HANDLE>(roundingUnits)))
            oviora::log::error("qml-window", std::format("SetProp film pixel rounding failed err={}", GetLastError()));
        if (!roundingUnits) RemovePropW(g_video, roundingProperty);
        oviora::log::info("qml-window", std::format("film pixel rounding allowance={} height pixels", double(roundingUnits) / 1024));
    }
    // Called every rendered frame: only a changed rect reaches the window manager,
    // so a moving popover never makes the presenter see a spurious resize.
    static RECT last{-1, -1, -1, -1};
    if (IsWindowVisible(g_video) && last.left == x && last.top == y && last.right == w && last.bottom == h) return true;
    last = RECT{x, y, w, h};
    SetLastError(0);
    const BOOL placed = SetWindowPos(g_video, HWND_TOP, x, y, w, h,
                                     SWP_NOACTIVATE | (g_videoHiddenByProbe ? 0 : SWP_SHOWWINDOW));
    const DWORD error = placed ? 0 : GetLastError();
    oviora::log::info("qml-window", std::format(
        "SetWindowPos rect=({},{}) {}x{} result={} error={}", x, y, w, h, placed != 0, error));
    logVideoWindowState(window, host, "geometry");
    return placed != 0;
}

struct Cover { QRectF rect; qreal radius = 0; };

// cineIn (pages.css @keyframes cineIn, plan M21): the stage is clipped with
// inset(12% 0 12% 0) and expands to inset(0). The picture is a native window, so
// the equivalent is its REGION: bands are cut off the top and bottom while the
// animation runs. The window and the swapchain keep their size, so this costs no
// ResizeBuffers - only the visible band changes. MinimalPage.inset carries the
// fraction (0.12 -> 0).
//
// The walk starts at the window's content item, NOT at the video host: the host is a
// sibling of the pages, so searching under it finds nothing. (The cover walk has the
// same shape and the same reason; both are rooted at the scene.)
QQuickItem* findVideoInsetItem(QQuickItem* item) {
    if (!item) return nullptr;
    if (item->objectName() == QLatin1String("videoInset")) return item;
    for (QQuickItem* child : item->childItems())
        if (QQuickItem* found = findVideoInsetItem(child)) return found;
    return nullptr;
}

qreal sceneInsetFraction(QQuickWindow* window) {
    if (!window) return 0.0;
    static QPointer<QQuickItem> cachedInset;
    if (!cachedInset) {
        QQuickItem* content = window->contentItem();
        QQuickItem* root = content && content->parentItem() ? content->parentItem() : content;
        cachedInset = findVideoInsetItem(root);
    }
    return cachedInset ? cachedInset->property("frac").toReal() : 0.0;
}

void collectCovers(QQuickItem* item, QList<Cover>& out) {
    if (!item->isVisible() || item->opacity() <= 0.0) return;
    // A cover is named "videoCover" or carries videoCover: true; the property lets a
    // dialog keep its own objectName for tests (an overridden name used to drop
    // the hole, leaving five dialogs behind the picture).
    // Hover tips are Controls ToolTips: their popup item sits in the window overlay and
    // its QObject parent is the ToolTip. Every tip is a cover, so a tip over the
    // picture is no longer hidden behind it (field report 2026-10-02).
    // Only the popup's own item: its background and text are QObject children of the
    // ToolTip too, and would only add the same hole again.
    const bool toolTip = item->inherits("QQuickPopupItem") && item->parent() && item->parent()->inherits("QQuickToolTip");
    if (toolTip || item->objectName() == QLatin1String("videoCover") || item->property("videoCover").toBool()) {
        // Scale is folded into the scene rect; the corner radius scales with it.
        const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        const qreal s = item->width() > 0 ? r.width() / item->width() : 1.0;
        out << Cover{r, item->property("coverRadius").toReal() * s};
    }
    for (QQuickItem* child : item->childItems()) collectCovers(child, out);
}

// The video is a native child window, so it is drawn above the whole QML scene
// (airspace): a menu or a dialog over the picture would sit behind it. Items named
// "videoCover" (popover panels, the dialog scrim) are cut out of the window's
// region instead, so the QML under them shows. Scene rects include transforms,
// so the hole follows a popover's scale-in. The region only changes when a cover
// moves or the cineIn inset changes; with neither the window gets its rectangle back.
void syncVideoCovers(QQuickWindow* window, QQuickItem* host, qreal inset) {
    if (!window || !host || !g_video) return;
    if (g_videoProbe == VideoProbeMode::CoversOnly) inset = 0;
    QList<Cover> covers;
    collectCovers(window->contentItem()->parentItem() ? window->contentItem()->parentItem()
                                                      : window->contentItem(), covers);
    const qreal dpr = window->devicePixelRatio();
    const QPointF origin = host->mapToScene(QPointF(0, 0));
    // The region clips the native HWND, so its dimensions must come from that
    // same HWND. Repeating the logical-size floor here would cut off the last
    // column again whenever a menu/control cover is present.
    RECT nativeClient{};
    if (!GetClientRect(g_video, &nativeClient)) {
        oviora::log::error("qml-window", std::format("GetClientRect for video covers failed err={}", GetLastError()));
        return;
    }
    const int w = std::max(1L, nativeClient.right);
    const int h = std::max(1L, nativeClient.bottom);
    static QList<QRect> last;
    // blankVideo() set the region behind this cache's back.
    if (g_videoBlanked) { last.clear(); g_videoBlanked = false; }
    QList<QRect> holes;       // x, y, w, h; the radius rides along as a fifth rect
    for (const Cover& c : covers) {
        const QRectF local = c.rect.translated(-origin);
        holes << QRect(int(std::floor(local.left() * dpr)), int(std::floor(local.top() * dpr)),
                       int(std::ceil(local.width() * dpr)) + 1, int(std::ceil(local.height() * dpr)) + 1);
        holes << QRect(0, 0, int(std::round(c.radius * dpr * 2)), 0);
    }
    // cineIn rides along as a marker entry so one cache covers both, and the last
    // entry keeps its steadiness on a scene that renders without moving either.
    const int insetBand = int(std::round(inset * h));
    {
        QList<QRect> key = holes;
        key << QRect(0, 0, insetBand, 0) << QRect(0, 0, w, h);
        if (key == last) return;
        last = key;
    }
    g_videoCoverHoles = holes;
    g_videoInsetBand = insetBand;
    if (g_videoProbe == VideoProbeMode::NoRegion) {
        SetLastError(0);
        const int result = SetWindowRgn(g_video, nullptr, TRUE);
        oviora::log::info("qml-window", std::format(
            "SetWindowRgn no-region result={} error={}", result, result ? 0 : GetLastError()));
        logVideoWindowState(window, host, "region");
        return;
    }
    // Covers win: a popover over the picture shows the QML beneath it, and the
    // clip inset only ever runs without one. A cover opening mid-animation simply
    // takes the whole rectangle back for as long as it is up.
    if (holes.isEmpty() && insetBand > 0) {
        // The design's inset(12% 0 12% 0 round 8px): both bands off the top and
        // bottom, corners eased with the band so the last frame has no snap.
        const int radius = std::max(2, int(std::round(8 * dpr * (inset / 0.12))));
        HRGN band = CreateRoundRectRgn(0, insetBand, w + 1, h - insetBand + 1, radius, radius);
        SetLastError(0);
        const int result = SetWindowRgn(g_video, band, TRUE);
        const DWORD error = result ? 0 : GetLastError();
        if (!result) DeleteObject(band);
        oviora::log::info("qml-window", std::format(
            "SetWindowRgn inset={}x{} band={} result={} error={}",
            w, h, insetBand, result, error));
        logVideoWindowState(window, host, "region");
        // The picture is a native window PrintWindow cannot show, so the cineIn
        // animation is evidenced here rather than in a screenshot.
        oviora::log::info("qml", std::format("cineIn inset band={}px of {} (frac={:.3f}) radius={}",
                                            insetBand, h, inset, radius));
        return;
    }
    if (holes.isEmpty()) {
        SetLastError(0);
        const int result = SetWindowRgn(g_video, nullptr, TRUE);
        oviora::log::info("qml-window", std::format(
            "SetWindowRgn full={}x{} result={} error={}", w, h, result, result ? 0 : GetLastError()));
        logVideoWindowState(window, host, "region");
        return;
    }
    HRGN region = CreateRectRgn(0, 0, w, h);
    for (qsizetype i = 0; i + 1 < holes.size(); i += 2) {
        const QRect& hole = holes[i];
        const int d = holes[i + 1].width();
        HRGN cut = d > 0 ? CreateRoundRectRgn(hole.left(), hole.top(), hole.left() + hole.width() + 1,
                                              hole.top() + hole.height() + 1, d, d)
                         : CreateRectRgn(hole.left(), hole.top(), hole.left() + hole.width(), hole.top() + hole.height());
        CombineRgn(region, region, cut, RGN_DIFF);
        DeleteObject(cut);
    }
    SetLastError(0);
    const int result = SetWindowRgn(g_video, region, TRUE); // the system owns the region on success
    const DWORD error = result ? 0 : GetLastError();
    if (!result) DeleteObject(region);
    oviora::log::info("qml-window", std::format(
        "SetWindowRgn covers={} size={}x{} result={} error={}",
        covers.size(), w, h, result, error));
    logVideoWindowState(window, host, "region");
}
} // namespace

namespace {
// The main window, for the border-drag rule below (set once QML has made it)
QPointer<QQuickWindow> g_mainWindow;

// Border drag on the stream page: the CLIENT area keeps the picture's shape (16:9 for a PS5),
// so the picture fills the window without bars. *rect is the proposed window rect (WM_SIZING),
// changed in place along the dragged edge. Maximize and fullscreen don't go through WM_SIZING.
bool keepClientAspect(HWND hwnd, WPARAM edge, RECT* rect) {
    QQuickWindow* window = g_mainWindow;
    if (!window || !rect || reinterpret_cast<HWND>(window->winId()) != hwnd) return false;
    if (!window->property("cinema").toBool() || window->property("screenSized").toBool()) return false;
    double aspect = window->property("filmAspect").toReal();
    if (!(aspect > 0.2 && aspect < 5.0)) aspect = 16.0 / 9.0;
    // The frame and title bar as they are now (physical pixels at this window's DPI)
    RECT outer{}, client{};
    if (!GetWindowRect(hwnd, &outer) || !GetClientRect(hwnd, &client)) return false;
    const int frameW = (outer.right - outer.left) - client.right;
    const int frameH = (outer.bottom - outer.top) - client.bottom;
    const qreal dpr = window->devicePixelRatio();
    const int minW = int(std::ceil(window->minimumWidth() * dpr));
    const int minH = int(std::ceil(window->minimumHeight() * dpr));
    // The monitor's work area bounds the result
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    const int maxW = (monitor.rcWork.right - monitor.rcWork.left) - frameW;
    const int maxH = (monitor.rcWork.bottom - monitor.rcWork.top) - frameH;

    int cw = (rect->right - rect->left) - frameW, ch = (rect->bottom - rect->top) - frameH;
    const bool left = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
    const bool top = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
    const bool topOrBottom = edge == WMSZ_TOP || edge == WMSZ_BOTTOM;
    const bool corner = edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT;
    // A side edge drives the width, top/bottom the height, a corner the larger of the two
    const bool byWidth = corner ? cw / aspect >= ch : !topOrBottom;
    if (byWidth) ch = int(std::lround(cw / aspect));
    else cw = int(std::lround(ch * aspect));
    if (cw < minW) { cw = minW; ch = int(std::lround(cw / aspect)); }
    if (ch < minH) { ch = minH; cw = int(std::lround(ch * aspect)); }
    if (maxW > 0 && cw > maxW) { cw = maxW; ch = int(std::lround(cw / aspect)); }
    if (maxH > 0 && ch > maxH) { ch = maxH; cw = int(std::lround(ch * aspect)); }
    // The dragged edges move, the opposite ones stay; top/bottom grow the width around the centre
    if (left) {
        rect->left = rect->right - (cw + frameW);
    } else if (topOrBottom) {
        const int centre = (rect->left + rect->right) / 2;
        rect->left = centre - (cw + frameW) / 2;
        rect->right = rect->left + cw + frameW;
    } else {
        rect->right = rect->left + cw + frameW;
    }
    if (top) rect->top = rect->bottom - (ch + frameH);
    else rect->bottom = rect->top + ch + frameH;
    return true;
}

// WM_SIZING never reaches Qt's native event filters (Qt hands it straight to DefWindowProc),
// so the main window is subclassed for it
LRESULT CALLBACK sizingSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_SIZING && keepClientAspect(hwnd, wParam, reinterpret_cast<RECT*>(lParam))) return TRUE;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, sizingSubclass, 1);
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
} // namespace

namespace {
// Last-chance handler, as in the Win32 shell (apps/oviora/main.cpp): provider and
// driver faults are structured exceptions no C++ catch sees. The 2.0 beta had no
// handler, so a field crash left neither a reason nor the last buffered log lines.
std::wstring g_crashDumpDirectory;
LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    static LONG entered = 0;
    if (InterlockedIncrement(&entered) > 1) return EXCEPTION_CONTINUE_SEARCH;
    const auto code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0u;
    const auto address = info && info->ExceptionRecord
        ? reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) : uintptr_t(0);
    HMODULE module = nullptr;
    wchar_t moduleName[MAX_PATH]{};
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(address), &module) && module)
        GetModuleFileNameW(module, moduleName, MAX_PATH);
    oviora::log::error("crash", std::format("unhandled exception code=0x{:08X} address=0x{:X} module={} offset=0x{:X} thread={}",
        code, address, std::filesystem::path(moduleName).filename().string(),
        module ? address - reinterpret_cast<uintptr_t>(module) : uintptr_t(0), GetCurrentThreadId()));
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[96]{};
    swprintf_s(name, L"oviora-crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", unsigned(st.wYear), unsigned(st.wMonth),
               unsigned(st.wDay), unsigned(st.wHour), unsigned(st.wMinute), unsigned(st.wSecond), GetCurrentProcessId());
    const std::wstring path = (std::filesystem::path(g_crashDumpDirectory) / name).wstring();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                          MiniDumpWithIndirectlyReferencedMemory, info ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(file);
        oviora::log::error("crash", std::format("minidump written={} file={}", ok != FALSE,
                                               std::filesystem::path(name).string()));
    } else {
        oviora::log::error("crash", std::format("minidump open failed error={}", GetLastError()));
    }
    // What crashed and which known-risky injected components were loaded, for the next
    // start to explain (QmlPlayerBridge reads and deletes it).
    const std::wstring note = L"crash " + std::filesystem::path(moduleName).filename().wstring() + L"\n" +
                              oviora::gfx::riskyInjections() + L"\n";
    HANDLE marker = CreateFileW((std::filesystem::path(g_crashDumpDirectory) / L"oviora-last-failure.txt").c_str(),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(marker, note.data(), DWORD(note.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(marker);
    }
    oviora::Logger::instance().flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

bool g_traceUiStalls = false;
struct UiStallStats {
    uint64_t notifyCount = 0;
    double notifyTotalMs = 0.0;
    double notifyMaxMs = 0.0;
    uint64_t over4Ms = 0;
    uint64_t over16Ms = 0;
    uint64_t over50Ms = 0;
    uint64_t followCount = 0;
    double followMaxMs = 0.0;
    uint64_t wmMouseCount = 0;
    uint64_t wmSetCursorCount = 0;
    double wmMouseMaxGapMs = 0.0;
    std::chrono::steady_clock::time_point lastWmMouse{};
} g_uiStalls;

class TracingApplication final : public QApplication {
public:
    using QApplication::QApplication;
    bool notify(QObject* receiver, QEvent* event) override {
        if (!g_traceUiStalls || QThread::currentThread() != thread())
            return QApplication::notify(receiver, event);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = QApplication::notify(receiver, event);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++g_uiStalls.notifyCount;
        g_uiStalls.notifyTotalMs += ms;
        if (ms > g_uiStalls.notifyMaxMs) g_uiStalls.notifyMaxMs = ms;
        if (ms >= 4.0) ++g_uiStalls.over4Ms;
        if (ms >= 16.0) ++g_uiStalls.over16Ms;
        if (ms >= 50.0) ++g_uiStalls.over50Ms;
        if (ms >= 5.0) {
            const char* cls = receiver ? receiver->metaObject()->className() : "null";
            const std::string name = receiver && !receiver->objectName().isEmpty()
                ? receiver->objectName().toStdString() : std::string("-");
            oviora::log::info("ui-stall-notify", std::format("ms={:.2f} type={} receiver={}/{}",
                ms, int(event ? event->type() : QEvent::None), cls, name));
        }
        return ok;
    }
};
} // namespace

static int runApplication(int argc, char** argv, QString& restartProgram, QStringList& restartArgs) {
    // A log file has to be opened explicitly; without this the QML app produced
    // no diagnostics at all, which made every failure look like a silent exit.
    // Same path and override as the Win32 build, so support instructions do not
    // change between the two front ends.
    wchar_t logOverride[32768]{};
    const auto overrideLength = GetEnvironmentVariableW(L"OVIORA_LOG_FILE", logOverride, 32768);
    auto logPath = overrideLength > 0 && overrideLength < 32768
                       ? std::filesystem::path(logOverride)
                       : oviora::runtime::logsDirectory() / L"oviora-qml.log";
    // Bounded: earlier sessions move to oviora-qml.old.log once this file passes
    // 16 MB at start, and a session that alone reaches 64 MB (about five hours of
    // capture) continues in a fresh file (audit 2026-10-15 B3).
    uint64_t rotateAtStart = 16ull << 20, rotateWhileRunning = 64ull << 20;
    // OVIORA_TEST_LOG_ROTATE_BYTES=N sets both limits to N bytes (rotation tests only).
    wchar_t rotateTest[32]{};
    if (GetEnvironmentVariableW(L"OVIORA_TEST_LOG_ROTATE_BYTES", rotateTest, 32) > 0 && _wtoi64(rotateTest) > 0)
        rotateAtStart = rotateWhileRunning = uint64_t(_wtoi64(rotateTest));
    if(!oviora::Logger::instance().startAsync())
        oviora::log::warn("log", "asynchronous output unavailable; using synchronous logging");
    oviora::Logger::instance().setRotation(rotateAtStart, rotateWhileRunning);
    if (!oviora::Logger::instance().openFile(logPath.wstring(), true)) {
        logPath = logPath.parent_path() / (L"oviora-qml-" + std::to_wstring(GetCurrentProcessId()) + L".log");
        (void)oviora::Logger::instance().openFile(logPath.wstring(), true);
    }
    g_crashDumpDirectory = logPath.parent_path().wstring();
    oviora::engine::EngineWatchdog::setDumpDirectory(g_crashDumpDirectory);
    SetUnhandledExceptionFilter(crashFilter);

    // Interface scale (设置 → 界面缩放) has to be fixed before Qt starts, so the
    // preference file is read here, from the same data directory the bridge uses.
    // "Auto" leaves Qt following the Windows display scale.
    int uiScale = 0;
    bool obsGameCapture = false;
    bool overlayCompatAuto = true;   // 设置 → 监控软件兼容: "auto" (default) or "off"
    bool overlayCompatRestart = false;
    bool gpuUiChosen = false;        // 设置 → 界面绘制: "software" (default) or "gpu"
    {
        int count = 0;
        LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        std::filesystem::path dir = oviora::runtime::localDataDirectory() / L"user-data-2.0.0";
        bool obsGameCaptureArg = false;
        for (int i = 1; args && i < count; ++i) {
            if (std::wstring(args[i]) == L"--data-dir" && i + 1 < count) dir = args[++i];
            else if (std::wstring(args[i]) == L"--obs-game-capture") obsGameCaptureArg = true;
            else if (std::wstring(args[i]) == L"--overlay-compat-restart") overlayCompatRestart = true;
        }
        if (args) LocalFree(args);
        QFile prefs(QString::fromStdWString((dir / L"qml-preferences.v1.json").wstring()));
        if (prefs.open(QIODevice::ReadOnly)) {
            const auto saved = QJsonDocument::fromJson(prefs.readAll()).object();
            uiScale = saved.value(QStringLiteral("uiScale")).toInt();
            obsGameCapture = saved.value(QStringLiteral("obsGameCapture")).toBool();
            overlayCompatAuto = saved.value(QStringLiteral("overlayCompat")).toString() != QStringLiteral("off");
            gpuUiChosen = saved.value(QStringLiteral("uiRenderer")).toString() == QStringLiteral("gpu");
        }
        obsGameCapture = obsGameCapture || obsGameCaptureArg;
        if (uiScale == 100 || uiScale == 125 || uiScale == 150) {
            qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
            qputenv("QT_SCALE_FACTOR", QByteArray::number(uiScale / 100.0));
            oviora::log::info("app", std::format("interface scale fixed at {}%", uiScale));
        } else {
            uiScale = 0;
        }
    }
    // OBS 32.1.2 shares capture state across DXGI swapchains. Keeping Qt off
    // DXGI prevents its resize/destruction from freeing the video's capture
    // resources on another thread. This opt-in affects UI drawing only;
    // the native D3D12 video and enhancement pipeline are unchanged.
    //
    // Monitoring overlays: RivaTuner Statistics Server (MSI Afterburner's OSD) draws on one
    // Direct3D 12 swapchain at a time, and a D3D12 interface next to the video's swapchain
    // had the GPU device removed within a second of a video starting (gfx/PresentationHooks.h).
    // A D3D11 interface survives but gets an OSD of its own showing the interface's rate, and
    // crashed with XeSS frame generation in the field. Drawn in software the interface has no
    // swapchain at all: RTSS sees only the video and puts its numbers there, as with 1.4.4's
    // GDI interface. OVIORA_UI_RHI picks a renderer explicitly and skips this.
    wchar_t forcedRenderer[16]{};
    GetEnvironmentVariableW(L"OVIORA_UI_RHI", forcedRenderer, 16);
    // OVIORA_TEST_IGNORE_RTSS=1 starts as if RivaTuner were not running (its late-start path).
    // An orphan hooks loader can inject the DLL after RTSS itself has exited.
    // Module presence does not prove an active OSD server. A user-accepted
    // compatibility restart carries its intent for this launch only, so timing
    // changes in server detection cannot turn that restart into another prompt.
    const bool rivaTuner = !GetEnvironmentVariableW(L"OVIORA_TEST_IGNORE_RTSS", nullptr, 0)
                           && oviora::gfx::rivaTunerRunning();
    // NVIDIA's in-game overlay and GamePP (游戏加加) hook every swapchain too; with the
    // interface on D3D12 next to the video, the two presenting threads deadlocked in the
    // field (logs(15)) and GamePP broke every resize (see gfx/PresentationHooks.h).
    // OVIORA_TEST_IGNORE_NVIDIA_OVERLAY / OVIORA_TEST_IGNORE_GAMEPP start as if they were off.
    const bool nvidiaOverlay = !GetEnvironmentVariableW(L"OVIORA_TEST_IGNORE_NVIDIA_OVERLAY", nullptr, 0)
                               && oviora::gfx::nvidiaOverlayRunning();
    const bool gamePP = !GetEnvironmentVariableW(L"OVIORA_TEST_IGNORE_GAMEPP", nullptr, 0) && oviora::gfx::gamePPRunning();
    // 抖音直播伴侣 captures the screen or this window while Oviora runs; with it the GPU
    // hung in the field (gfx/PresentationHooks.h). The interface leaves the GPU like for
    // the other capture and overlay tools; the bridge also lowers the GPU priority while
    // it runs. OVIORA_TEST_IGNORE_DOUYIN=1 starts as if it were not running.
    const bool douyin = !GetEnvironmentVariableW(L"OVIORA_TEST_IGNORE_DOUYIN", nullptr, 0) && oviora::gfx::douyinLiveCompanionRunning();
    // 设置 → 界面绘制 (software unless GPU is chosen) together with the compatibility
    // rules above, which are unchanged: oviora/ui/InterfaceRenderer.h.
    const auto renderer = oviora::ui::decideInterfaceRenderer({gpuUiChosen, obsGameCapture, overlayCompatAuto,
        rivaTuner || nvidiaOverlay || gamePP || douyin, overlayCompatRestart, forcedRenderer[0] != 0});
    const bool overlayCompat = renderer.overlayCompat;
    const bool softwareUi = renderer.software;
    if (softwareUi) {
        QQuickWindow::setSceneGraphBackend(QStringLiteral("software"));
        oviora::log::info("app", "software UI: cached raster paths; normal partial updates; no additional frame requests");
    }
    using Reason = oviora::ui::InterfaceRendererReason;
    if (renderer.reason == Reason::ObsGameCapture)
        oviora::log::info("app", "OBS game capture compatibility: software UI; native D3D12 video unchanged");
    else if (renderer.reason == Reason::OverlayCompat)
        oviora::log::info("app", std::format("RTSS compatibility: software UI; activeServer={} nvidiaOverlay={} gamePP={} douyinCompanion={} acceptedRestart={}; native D3D12 video unchanged",
                                          rivaTuner, nvidiaOverlay, gamePP, douyin, overlayCompatRestart));
    else if (renderer.reason == Reason::Default)
        oviora::log::info("app", "software UI by default (interface drawing preference: software); native D3D12 video unchanged");
    else
        oviora::log::info("app", std::format("GPU UI: {}", forcedRenderer[0] ? "renderer forced by OVIORA_UI_RHI" : "interface drawing preference: GPU"));
    {
        // A GPU interface keeps RTSS from drawing in this process at all, so a RivaTuner
        // started after Oviora (or kept with 监控软件兼容 off) cannot take the device down.
        // RTSS reads the variable when it loads its profile here; a software interface
        // leaves it out (and clears it after a restart from a GPU-interface run).
        wchar_t current[64]{};
        const DWORD length = GetEnvironmentVariableW(oviora::gfx::kRivaTunerProfileOverride, current, 64);
        const bool ours = length > 0 && wcscmp(current, oviora::gfx::kRivaTunerOsdOff) == 0;
        const bool gpuD3d12 = !softwareUi && _wcsicmp(forcedRenderer, L"d3d11") != 0;
        if (gpuD3d12 && length == 0) SetEnvironmentVariableW(oviora::gfx::kRivaTunerProfileOverride, oviora::gfx::kRivaTunerOsdOff);
        else if (!gpuD3d12 && ours) SetEnvironmentVariableW(oviora::gfx::kRivaTunerProfileOverride, nullptr);
        oviora::log::info("app", std::format("RivaTuner running={} injected={} compat={} OSD in this process={}", rivaTuner,
            GetModuleHandleW(L"RTSSHooks64.dll") != nullptr, overlayCompatAuto ? "auto" : "off", gpuD3d12 ? "off (GPU interface)" : "on"));
        oviora::log::info("app", std::format("NVIDIA overlay running={} GamePP running={} Douyin companion running={} interface={}", nvidiaOverlay, gamePP, douyin, softwareUi ? "software" : "GPU"));
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--trace-ui-stalls") == 0) g_traceUiStalls = true;
    }
    if (qEnvironmentVariableIsSet("OVIORA_TEST_TRACE_UI_STALLS")) g_traceUiStalls = true;
    if (g_traceUiStalls) SetEnvironmentVariableW(L"OVIORA_TEST_TRACE_UI_STALLS", L"1");
    TracingApplication app(argc, argv);
    // G-SYNC off for Oviora in the NVIDIA driver profile (gfx/NvidiaAppProfile.h); the driver database is slow
    std::thread([] { oviora::log::info("gsync-profile", oviora::gfx::ensureFixedRefreshProfile()); }).detach();
    if (g_traceUiStalls) {
        app.setProperty("ovioraTraceUiStalls", true);
        oviora::log::info("ui-stall", "main-thread stall tracing enabled");
    }
    // Qt does not automatically use the executable's Win32 icon for its windows.
    // Load the same embedded multi-size resource for the taskbar and Alt+Tab.
    QIcon appIcon;
    for (const int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
        const auto handle = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_OVIORA), IMAGE_ICON, size, size, 0));
        if (!handle) {
            oviora::log::error("app-icon", std::format("LoadImageW size={} error={}", size, GetLastError()));
            continue;
        }
        const auto image = QImage::fromHICON(handle);
        DestroyIcon(handle);
        if (!image.isNull()) appIcon.addPixmap(QPixmap::fromImage(image));
    }
    app.setWindowIcon(appIcon);
    oviora::log::info("app-icon", std::format("embedded window icon sizes={}", appIcon.availableSizes().size()));
    // A GPU interface (设置 → 界面绘制 = GPU, or OVIORA_UI_RHI) draws with Direct3D 12, like the video. Qt's default is Direct3D 11,
    // and a D3D11 device in the process changed how overlays treat it: RivaTuner (MSI
    // Afterburner) then crashed in d3d11.dll inside its own hooks while drawing its OSD on
    // the XeSS swapchain through D3D11On12 (field dumps 2026-10-01). 1.4.4 drew its UI with
    // GDI and had no D3D11 device; the overlays coexisted. OVIORA_UI_RHI=d3d11 restores
    // Qt's default; without a usable D3D12 device Qt keeps it too.
    if (!softwareUi) {
        const bool wantD3d11 = _wcsicmp(forcedRenderer, L"d3d11") == 0;
        const bool d3d12Usable = SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
        if (!wantD3d11 && d3d12Usable) QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D12);
        oviora::log::info("app", std::format("interface renderer={} (d3d12 usable={} forced={})",
            !wantD3d11 && d3d12Usable ? "d3d12" : "d3d11", d3d12Usable, QString::fromWCharArray(forcedRenderer).toStdString()));
        // Qt Quick's own pipeline cache goes to QStandardPaths::CacheLocation by default, which is
        // %LOCALAPPDATA%\Oviora\Oviora\cache: the OLD Oviora's folder (same organization and
        // application name). Every window of this run reads and writes this app's own file instead
        // (QQuickGraphicsConfiguration takes QSG_RHI_PIPELINE_CACHE_SAVE / _LOAD as its defaults,
        // read when the first window is made, which is after this point). Qt's OpenGL program
        // cache (qtshadercache-*) is only written by an OpenGL interface, which this app never uses.
        const QString folder = QString::fromStdWString((oviora::runtime::localDataDirectory() / L"cache").wstring())
                               + QStringLiteral("/qtpipelinecache-") + QSysInfo::buildAbi();
        // Set through Qt (qputenv) so Qt's own runtime sees it; like the QML cache below, a
        // folder the ANSI code page cannot spell turns the cache off instead.
        const QString file = QDir::toNativeSeparators(folder + QStringLiteral("/qqpc_") + (!wantD3d11 && d3d12Usable ? QStringLiteral("d3d12") : QStringLiteral("d3d11")));
        const QByteArray local = file.toLocal8Bit();
        // The automatic cache is switched off altogether: without a load file Qt still looked
        // into the automatic folder (the old app's) for one. An empty load file is skipped by Qt.
        qputenv("QSG_RHI_DISABLE_DISK_CACHE", "1");
        if (QDir().mkpath(folder) && QString::fromLocal8Bit(local) == file) {
            const bool existed = QFileInfo::exists(file);
            if (!existed) { QFile empty(file); (void)empty.open(QIODevice::WriteOnly); }
            qputenv("QSG_RHI_PIPELINE_CACHE_SAVE", local);
            qputenv("QSG_RHI_PIPELINE_CACHE_LOAD", local);
            oviora::log::info("app", std::format("Qt pipeline cache {} ({})", file.toStdString(), existed ? "loaded" : "new"));
        } else {
            // No folder of our own: no cache at all rather than the old app's folder.
            oviora::log::warn("app", "Qt pipeline cache off: no folder of this app for it");
        }
    }
    app.setProperty("ovioraLogFile", QString::fromStdWString(logPath.wstring()));
    app.setProperty("ovioraUiScale", uiScale);
    app.setProperty("ovioraObsGameCapture", obsGameCapture);
    app.setProperty("ovioraOverlayCompat", overlayCompat);
    app.setProperty("ovioraSoftwareUi", softwareUi);
    app.setProperty("ovioraUiRendererForced", forcedRenderer[0] != 0);
    // HTTRANSPARENT forwards the native video child's hit test to Qt's parent.
    // Refuse the class arrow there too while the picture hides the cursor; hover
    // over a control or another window is untouched.
    struct VideoCursorFilter final : QAbstractNativeEventFilter {
        bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override {
            if (type != "windows_generic_MSG") return false;
            const auto* msg = static_cast<const MSG*>(message);
            if (msg->message == WM_SETCURSOR && LOWORD(msg->lParam) == HTCLIENT
                && g_videoCursorHidden && (msg->hwnd == g_video || msg->hwnd == GetParent(g_video))
                && pointerOverVideo()) {
                SetCursor(nullptr);
                if (result) *result = TRUE;
                return true;
            }
            return false;
        }
    };
    static VideoCursorFilter videoCursorFilter;
    app.installNativeEventFilter(&videoCursorFilter);
    app.setApplicationName(QStringLiteral("Oviora"));
    app.setOrganizationName(QStringLiteral("Oviora"));
    oviora::log::info("app", "Oviora QML session started");

    engine::EngineController controller;

    // The window exists before QML loads so the engine can be handed a valid
    // HWND as soon as the user opens something.
    // --test-terms <title id>: sends sample lines through the game's term lists and Google, logs the
    // sent and restored text and quits (no window, no PS5)
    if (const QStringList a = QCoreApplication::arguments(); a.indexOf(QStringLiteral("--test-terms")) > 0) {
        const int at = int(a.indexOf(QStringLiteral("--test-terms")));
        return oviora::subtitle::runTermsTest(at + 1 < a.size() ? a.at(at + 1) : QString());
    }
    // --test-discord: a sample Discord activity for ~8 s, then cleared (0 = done, 2 = Discord not running)
    if (QCoreApplication::arguments().contains(QStringLiteral("--test-discord"))) return DiscordPresence::runTest();

    g_video = createVideoWindow();

    // Keep 2.0.0 settings separate from 1.4.4 so the old executable remains a
    // working rollback. An explicit data directory is for isolated runs and is
    // never populated from the user's old settings.
    std::filesystem::path dataDirectory = oviora::runtime::localDataDirectory() / L"user-data-2.0.0";
    std::filesystem::path legacyDirectory = oviora::runtime::localDataDirectory();
    bool explicitDataDirectory = false;
    {
        const QStringList early = QCoreApplication::arguments();
        const auto at = early.indexOf(QStringLiteral("--data-dir"));
        if (at > 0 && at + 1 < early.size()) {
            dataDirectory = std::filesystem::path(early.at(at + 1).toStdWString());
            explicitDataDirectory = true;
            oviora::log::info("app", "test data directory " + early.at(at + 1).toStdString());
        }
        const auto importAt = early.indexOf(QStringLiteral("--import-1.4.4-data"));
        if (importAt > 0 && importAt + 1 < early.size())
            legacyDirectory = std::filesystem::path(early.at(importAt + 1).toStdWString());
    }
    if (!explicitDataDirectory) {
        const auto migration = ui::prepareQmlDataDirectory(legacyDirectory, dataDirectory);
        if (!migration.ok) {
            oviora::log::error("app", "2.0.0 data migration failed: " + migration.error);
            return 2;
        }
        oviora::log::info("app", std::format("2.0.0 data directory ready; copied {} legacy files",
                                             migration.copied));
    }
    ui::QmlPlayerBridge bridge(controller, dataDirectory);
    bridge.attachVideoWindow(reinterpret_cast<qulonglong>(g_video));

    // Qt reports QML failures through its own message handler. Routing them to
    // the log is the difference between "it did not load" and why.
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& message) {
        const std::string utf8 = message.toUtf8().toStdString();
        if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
            oviora::log::error("qml", utf8);
        } else {
            oviora::log::info("qml", utf8);
        }
    });

    // Compiled QML goes to a cache folder of this build's own QML (see
    // QmlDiskCache.h): with Qt's shared folder, 2.1.3 extracted over 2.1.2 ran
    // the 2.1.2 pages. Qt reads QML_DISK_CACHE_PATH once, at the first QML load,
    // with fromLocal8Bit; a folder the ANSI code page cannot spell turns the disk
    // cache off rather than letting Qt fall back to the shared one.
    {
        const auto started = std::chrono::steady_clock::now();
        const QString testRoot = qEnvironmentVariable("OVIORA_TEST_QML_CACHE_ROOT");
        const auto cache = ui::chooseQmlDiskCache(
            QCoreApplication::applicationDirPath() + QStringLiteral("/qml/Oviora"),
            // The new Oviora's own data folder: CacheLocation is %LOCALAPPDATA%\Oviora\Oviora\cache, the
            // old Oviora's folder (same organization and application name)
            testRoot.isEmpty() ? QString::fromStdWString((oviora::runtime::localDataDirectory() / L"cache").wstring()) : testRoot,
            qgetenv("QML_DISK_CACHE_PATH"), qgetenv("QML_DISABLE_DISK_CACHE"));
        QString outcome = cache.note;
        if (!cache.directory.isEmpty()) {
            const QString folder = QDir::toNativeSeparators(cache.directory);
            const QByteArray local = folder.toLocal8Bit();
            if (QString::fromLocal8Bit(local) == folder) {
                qputenv("QML_DISK_CACHE_PATH", local);
                outcome = QStringLiteral("folder=") + folder;
            } else {
                qputenv("QML_DISABLE_DISK_CACHE", "1");
                outcome = QStringLiteral("disk cache off: the code page cannot spell ") + folder;
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        oviora::log::info("qml-cache", std::format("{} digest={} files={} bytes={} ms={:.1f}",
            outcome.toStdString(), cache.digest.isEmpty() ? "none" : cache.digest.toStdString(), cache.files, cache.bytes, ms));
    }

    // In-game subtitle translation (context property "subtitles"): made once the server client exists,
    // destroyed after the QML engine and before the engine controller.
    std::unique_ptr<oviora::subtitle::SubtitleTranslator> subtitles;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("vySoftwareUi"), softwareUi);
    engine.addImportPath(QCoreApplication::applicationDirPath() + "/qml");
    engine.rootContext()->setContextProperty(QStringLiteral("oviora"), &bridge);
    // The stream menu's live backdrop (image://oviora-menu/<serial>, QmlPlayerBridge::requestMenuBackdrop)
    engine.addImageProvider(QStringLiteral("oviora-menu"), new oviora::ui::MenuBackdropProvider);
    bridge.setQmlEngine(&engine);   // Qt.uiLanguage drives the per-language font (Theme.qml)

    // Oviora platform: account, friends, consoles over PSN (the Oviora server). Only a package
    // with sunucu.txt next to the exe talks to a server; otherwise "configured" is false.
    OvioraServer::Hooks platformHooks;
    OvioraServer* platform = nullptr;
    platformHooks.connect_psn = [&bridge, &platform](const QString& token, const QString& accountId, const QString& duid, int,
                                                     const QString& nickname, const QString& relaySid) {
        auto route = std::make_shared<oviora::remoteplay::PsnRoute>();
        route->duid = duid.toStdString();
        route->token = token.toStdString();
        if (const auto id = oviora::remoteplay::accountIdFromBase64(accountId.toStdString())) route->accountId = *id;
        if (!relaySid.isEmpty()) route->relay = std::make_shared<PsnRelayBridge>(platform, relaySid);
        oviora::log::info("platform", std::format("PSN connect relay={}", !relaySid.isEmpty()));
        bridge.connectPsn(std::static_pointer_cast<const void>(std::shared_ptr<const oviora::remoteplay::PsnRoute>(route)), nickname);
    };
    platformHooks.stop_session = [&bridge] { bridge.ps5Cancel(); };
    platformHooks.session_active = [&bridge] { return bridge.remotePlayActive(); };
    platform = new OvioraServer(std::move(platformHooks), &app);
    // A downloaded release is installed at once (Oviora restarts by itself); during a PS5 session
    // applyUpdate() waits, and the server announces it again when the session has ended.
    QObject::connect(platform, &OvioraServer::updateReady, &app, [platform] { platform->applyUpdate(); }, Qt::QueuedConnection);
    engine.rootContext()->setContextProperty(QStringLiteral("platform"), platform);
    // PS5s paired on the home network are kept in the Oviora account (bridge/RemotePlay.cpp): only
    // while signed in, merged once at start and at every sign-in.
    bridge.setAccountCall([platform](const QString& action, const QJsonObject& body,
                                     std::function<void(const QJsonObject&, const QString&)> done) {
        if (platform->signedIn()) platform->call(action, body, std::move(done));
    });
    {
        auto syncedUser = std::make_shared<QString>();
        auto sync = [platform, &bridge, syncedUser] {
            const QString user = platform->signedIn() ? platform->user() : QString();
            if (user == *syncedUser) return;
            *syncedUser = user;
            if (!user.isEmpty()) bridge.syncAccountConsoles();
        };
        QObject::connect(platform, &OvioraServer::changed, &app, sync);
        sync();
    }
    // End-to-end encrypted messages between friends (the same server actions as the old Oviora)
    auto* messages = new MessageCenter(platform, &app);
    engine.rootContext()->setContextProperty(QStringLiteral("messages"), messages);
    subtitles = std::make_unique<oviora::subtitle::SubtitleTranslator>(controller, g_video,
        QString::fromStdWString((dataDirectory / L"altyazi.json").wstring()));
    subtitles->setServer(platform);
    engine.rootContext()->setContextProperty(QStringLiteral("subtitles"), subtitles.get());
    // The translated line on a phone (Ctrl+O > Altyazı > Telefonda göster): pushed as it changes
    auto* subtitleScreen = new SubtitleScreen(&app);
    QObject::connect(subtitles.get(), &oviora::subtitle::SubtitleTranslator::textChanged, subtitleScreen,
                     [subtitleScreen, s = subtitles.get()] { subtitleScreen->setText(s->text(), s->sourceText()); });
    engine.rootContext()->setContextProperty(QStringLiteral("subtitleScreen"), subtitleScreen);
    // --test-subscreen: sharing on at start, a made-up line every 700 ms and the address in the log (test runs only)
    if (QCoreApplication::arguments().contains(QStringLiteral("--test-subscreen"))) {
        subtitleScreen->setLocalOnly(true);
        subtitleScreen->start();
        oviora::log::info("subtitle-screen", "test address " + subtitleScreen->url().toStdString() + " test code " + subtitleScreen->code().toStdString());
        auto* feed = new QTimer(subtitleScreen);
        auto n = std::make_shared<int>(0);
        QObject::connect(feed, &QTimer::timeout, subtitleScreen, [subtitleScreen, n] {
            ++*n;
            subtitleScreen->setText(QStringLiteral("Deneme satırı %1 / İkinci satır: ğüşıöç").arg(*n), QStringLiteral("Test line %1").arg(*n));
        });
        feed->start(700);
    } else {
        subtitleScreen->resume();   // left on last time: phones find it again by themselves
    }
    // İzlet (Ctrl+O > İzlet): the stream watched live by friends in the browser, through the server's web gateway
    IzletController::Hooks izletHooks;
#ifdef OVIORA_ENABLE_REMOTEPLAY
    izletHooks.setTee = [&controller](oviora::remoteplay::MediaRelayCallbacks tee) { controller.setRemotePlayMediaTee(std::move(tee)); };
    izletHooks.requestIdr = [&controller] { controller.requestRemotePlayIdr(); };
    // Canlı yayınlar > İzle: a friend's broadcast in this app's own player
    izletHooks.connectWatch = [&bridge](std::shared_ptr<oviora::remoteplay::WatchRoute> route, const QString& title) {
        return bridge.connectWatch(std::move(route), title);
    };
    izletHooks.stopWatch = [&bridge] { bridge.ps5Cancel(); };
#endif
    auto* izlet = new IzletController(platform, std::move(izletHooks), &app);
    engine.rootContext()->setContextProperty(QStringLiteral("izlet"), izlet);
    {
        auto sync = [izlet, &bridge] {
            const QVariantMap c = bridge.connection();
            const QString stage = c.value(QStringLiteral("stage")).toString();
            const bool watching = c.value(QStringLiteral("watching")).toBool();
            izlet->setWatchState(watching, stage);
            izlet->setStream(!watching && (stage == QLatin1String("live") || stage == QLatin1String("retrying")),
                             c.value(QStringLiteral("codec")).toString(),
                             c.value(QStringLiteral("console")).toString());
        };
        if (QCoreApplication::arguments().contains(QStringLiteral("--test-izlet"))) {
            izlet->showTest();
        } else {
            QObject::connect(&bridge, &ui::QmlPlayerBridge::connectionChanged, izlet, sync);
            sync();
        }
    }
    auto* psnWeb = new PsnWebView(nullptr, &app);
    engine.rootContext()->setContextProperty(QStringLiteral("psnWeb"), psnWeb);
    // Oyun bilgisi: Sony's sign-in value goes from the page straight to the server, never through QML
    psnWeb->setWideSink([platform](int link, QByteArray& value, bool linked) { platform->wideLinkPsn(link, value, linked); });
    // "PSN hesabını bağla" with "Oyun bilgisini de aç": the page held after the redirect reads Sony's
    // sign-in for the account psn_link just linked (or renewed); a failed link drops it
    QObject::connect(platform, &OvioraServer::psnLinkFinished, psnWeb, [psnWeb](bool ok, int link) {
        if (!psnWeb->holdingForWide()) return;
        if (ok) psnWeb->readWideFor(link);
        else psnWeb->close();
    });
    QObject::connect(psnWeb, &PsnWebView::linkWideFailed, platform, [platform] { platform->wideLinkSoftFailed(); });
    // The console dialog's "PSN ile giriş yap": any account's id, nothing linked or stored
    engine.rootContext()->setContextProperty(QStringLiteral("psnLookup"), new PsnAccountLookup(&app));
    // The leased session ended (or never started): the server frees the console
    {
        auto* watch = new QTimer(&app);
        auto wasActive = std::make_shared<bool>(false);
        QObject::connect(watch, &QTimer::timeout, platform, [platform, &bridge, wasActive] {
            const bool active = bridge.remotePlayActive() && !bridge.connection().value(QStringLiteral("watching")).toBool();
            if (platform->sessionState() == QLatin1String("playing") && !active && *wasActive) platform->sessionEnded();
            *wasActive = active || platform->sessionState() == QLatin1String("asking");
        });
        watch->start(2000);
    }
    // The game running on a PS5 of this network goes to the server (console_app): friends see it whichever
    // PSN account plays, also with "Oyun bilgisi" off. The server keeps only my own PS5s.
    QObject::connect(&bridge, &ui::QmlPlayerBridge::lanChanged, platform, [platform, &bridge] {
        if (!platform->signedIn()) return;
        for (const auto& v : bridge.lanConsoles()) {
            const QVariantMap c = v.toMap();
            const QString id = c.value(QStringLiteral("id")).toString();
            if (!c.value(QStringLiteral("discovered")).toBool() || id.isEmpty()) continue;
            const QString app = c.value(QStringLiteral("app")).toString();
            const QString state = c.value(QStringLiteral("state")).toString() == QLatin1String("standby") ? QStringLiteral("standby")
                                : app.isEmpty() ? QStringLiteral("idle") : QStringLiteral("running");
            platform->reportConsoleApp(id, app, c.value(QStringLiteral("titleId")).toString(), state, c.value(QStringLiteral("hostName")).toString());
        }
    });
    // Discord Rich Presence (Genel -> Gizlilik): the game, whose PS5 and since when, while a stream is live
    {
        auto* discord = new DiscordPresence(&app);
        auto startedMs = std::make_shared<qint64>(0);
        auto wasLive = std::make_shared<bool>(false);
        auto update = [discord, platform, &bridge, startedMs, wasLive] {
            // Fast exit when idle and Remote Play is inactive so ps5Changed/lanChanged/accountChanged
            // do not build QVariantMaps unnecessarily.
            const bool active = bridge.remotePlayActive() && !bridge.connection().value(QStringLiteral("watching")).toBool();
            if (!active && !*wasLive && *startedMs == 0) {
                discord->setEnabled(bridge.preferences().value(QStringLiteral("discordPresence"), true).toBool());
                discord->setIdle();
                return;
            }
            const QVariantMap prefs = bridge.preferences();
            const bool enabled = prefs.value(QStringLiteral("discordPresence"), true).toBool();
            discord->setEnabled(enabled);
            if (!enabled) { *startedMs = 0; *wasLive = false; discord->setIdle(); return; }
            // Live from the first picture; a short reconnect keeps the start time
            const QString stage = active ? bridge.connection().value(QStringLiteral("stage")).toString() : QString();
            const bool live = active
                && (stage == QLatin1String("live") || (*startedMs && stage == QLatin1String("retrying")));
            if (!live) { *startedMs = 0; *wasLive = false; discord->setIdle(); return; }
            *wasLive = true;
            if (!*startedMs) *startedMs = QDateTime::currentMSecsSinceEpoch();
            // Whose PS5: a platform lease names the console (available: mine, host = its owner's name);
            // a connection on this network is always to a PS5 paired with this PC
            QString duid, game, titleId, owner, icon;
            bool friendConsole = false;
            int psnLink = 0;   // the own PSN account of the console's row (its "Oyun bilgisi" knows the game)
            auto fromAccount = [&](const QString& id) {
                for (const auto& list : {platform->available(), platform->consoles()})
                    for (const auto& v : list) {
                        const QVariantMap c = v.toMap();
                        if (id.isEmpty() || c.value(QStringLiteral("duid")).toString().compare(id, Qt::CaseInsensitive) != 0) continue;
                        if (game.isEmpty()) game = c.value(QStringLiteral("app")).toMap().value(QStringLiteral("name")).toString();
                        if (!psnLink) psnLink = c.value(QStringLiteral("psn_link")).toInt();
                        if (c.contains(QStringLiteral("mine")) && !c.value(QStringLiteral("mine")).toBool()) {
                            friendConsole = true;
                            owner = c.value(QStringLiteral("host")).toString();
                        }
                        return true;
                    }
                return false;
            };
            // What an own PSN account with "Oyun bilgisi" plays now (server: psn_links[].playing, at most a minute old)
            auto ownPlaying = [&](int link) {
                for (const auto& v : platform->psnLinks()) {
                    const QVariantMap l = v.toMap(), p = l.value(QStringLiteral("playing")).toMap();
                    if (!p.value(QStringLiteral("game")).toString().isEmpty() && (!link || l.value(QStringLiteral("id")).toInt() == link)) return p;
                }
                return QVariantMap();
            };
            bool lan = false;
            if (platform->sessionState() == QLatin1String("playing") && !platform->sessionDuid().isEmpty()) {
                duid = platform->sessionDuid();
                fromAccount(duid);
            } else {
                lan = true;
                // What this network's discovery saw the console run before the stream (it pauses while streaming)
                const QString host = bridge.ps5().value(QStringLiteral("host")).toString();
                for (const auto& v : bridge.lanConsoles()) {
                    const QVariantMap c = v.toMap();
                    if (host.isEmpty() || c.value(QStringLiteral("address")).toString() != host) continue;
                    game = c.value(QStringLiteral("app")).toString();
                    titleId = c.value(QStringLiteral("titleId")).toString();
                    duid = c.value(QStringLiteral("id")).toString();
                }
                fromAccount(duid);
                friendConsole = false;
                owner.clear();
            }
            // Own PS5: PSN's word is the freshest (the home network's look is from before the stream). Through the
            // platform the row's own account; on this network the first own account that plays something
            if (!friendConsole && (psnLink || lan)) {
                const QVariantMap p = ownPlaying(lan ? 0 : psnLink);
                if (!p.isEmpty()) {
                    game = p.value(QStringLiteral("game")).toString();
                    icon = p.value(QStringLiteral("icon")).toString();
                }
            }
            // Only a title id: the name from the subtitle term lists, else "PS5'te oynuyor"
            if (game.isEmpty()) game = DiscordPresence::gameForTitleId(!titleId.isEmpty() ? titleId : platform->runningTitleId(duid));
            // Streamer mode hides account names: "a friend's PS5" without the name
            if (prefs.value(QStringLiteral("streamerMode")).toBool()) owner.clear();
            discord->setPlaying(game, icon, friendConsole, owner, *startedMs);
        };
        QObject::connect(&bridge, &ui::QmlPlayerBridge::connectionChanged, discord, update);
        QObject::connect(&bridge, &ui::QmlPlayerBridge::ps5Changed, discord, update);
        QObject::connect(&bridge, &ui::QmlPlayerBridge::lanChanged, discord, update);
        QObject::connect(&bridge, &ui::QmlPlayerBridge::preferencesChanged, discord, update);
        QObject::connect(platform, &OvioraServer::changed, discord, update);
        QObject::connect(platform, &OvioraServer::accountChanged, discord, update);
        update();
    }

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    // Options are parsed rather than positional: `--page <id>` selects the screen
    // a test wants to capture.
    //
    // The rest are test switches (G0.3). They put the app into the exact state a
    // design frame shows, so a screenshot of each can be compared with its design
    // reference without anyone clicking. They go to QML as `vyTest`, never to the
    // bridge's saved settings: a test run must not rewrite the user's preferences.
    //   --section <id>          Oviora settings section (with --page set) or stream-menu section
    //   --dialog <streammenu|ps5|audio|save|manage>
    //   --aspect <ratio>        cinema aspect, overriding the source's own
    //   --size <W>x<H>          window size in device-independent pixels
    //   --reduced-motion        every animation at zero duration
    //   --slow-animations <N>   every animation N times slower (motion sampling)
    //   --full-bar <shown|hidden>  fullscreen with the control window held shown / hidden
    //   --full-debug            log every pointer movement the fullscreen controls see
    //   --data-dir <path>       presets and session history from that folder (read before the bridge exists)
    //   --import-1.4.4-data <path>  read-only legacy settings source for first 2.0.0 launch
    //   --motion-probe <name>   switch | seg: start that motion, log its value per frame
    //   --test-connect <lan|psn|psn-arkadas>  connect to the PS5 (own, or a friend's shared one) by itself (test runs)
    //   --test-login <file>     sign in with the account in this file (kullanici=, sifre=) (test runs)
    //   --test-drag <edge:dx[;dy]@s,...>  border drags of this window (WM_SIZING), e.g. right:300@20
    //   --test-chat <friend>    open that friend's message window (only opens it; nothing is sent);
    //                           #<id> opens that group's conversation
    //   --test-groups           made-up friends with presence, groups and a group conversation, only on
    //                           screen while signed out (OvioraServer, MessageCenter); nothing is sent
    //   --test-consoles <n>     with --test-psn-links: n made-up PS5s instead of two (OvioraServer)
    //   --dialog shareconsoles  the first friend's "PS5 paylaş" window (home screen)
    //   --dialog psnlink        the "PSN hesabını bağla" popup alone (no Sony page is loaded)
    //   --test-mask <rect|ellipse>@s[,drag@s|esc@s]  region drawing of "Tüm efektleri hariç tut":
    //                           the draw button, then a drag / Esc sent to the app's own drawing window
    //   --exit-after <ms>       quit the app by itself after N ms, so a test run ends
    //                           through the engine's own teardown instead of a kill
    //   --video-probe <normal|hidden|no-region|covers-only>  native-window diagnosis
    const QStringList args = QCoreApplication::arguments();
    QVariantMap testOptions;
    if(qEnvironmentVariableIsSet("OVIORA_TEST_DISABLE_PAUSED_UI_IDLE"))
        testOptions.insert(QStringLiteral("disablePausedUiIdle"),true);
    QSize testSize;
    // --exit-after: see the switch table. 0 means "run until closed".
    int exitAfterMs = 0;
    QString testConnect;
    QString testDrag;
    int subtitleTest = 0;
    QString testKeys;
    QString testMouse;
    bool testWmMouse = false;
    bool testPadCapture = false;
    QString testVideo;
    QString testIzleApi, testIzleToken, testIzleYid;
    for (int i = 1; i < args.size(); ++i) {
        const QString& a = args.at(i);
        const bool hasValue = i + 1 < args.size();
        if (a == QLatin1String("--trace-ui-stalls")) {
            g_traceUiStalls = true;
        } else if (a == QLatin1String("--test-wm-mouse")) {
            testWmMouse = true;
        } else if (a == QLatin1String("--test-pad-capture")) {
            testPadCapture = true;
        } else if (a == QLatin1String("--test-video") && hasValue) {
            testVideo = args.at(++i);
        } else if (a == QLatin1String("--page") && hasValue) {
            bridge.setInitialPage(args.at(++i));
        } else if (a == QLatin1String("--section") && hasValue) {
            testOptions.insert(QStringLiteral("section"), args.at(++i));   // Oviora settings section
        } else if (a == QLatin1String("--dialog") && hasValue) {
            testOptions.insert(QStringLiteral("dialog"), args.at(++i));
        } else if (a == QLatin1String("--aspect") && hasValue) {
            testOptions.insert(QStringLiteral("aspect"), args.at(++i).toDouble());
        } else if (a == QLatin1String("--full-bar") && hasValue) {
            testOptions.insert(QStringLiteral("fullBar"), args.at(++i));
        } else if (a == QLatin1String("--full-debug")) {
            testOptions.insert(QStringLiteral("fullDebug"), true);
        } else if (a == QLatin1String("--data-dir") && hasValue) {
            ++i;  // handled before the bridge was created
        } else if (a == QLatin1String("--import-1.4.4-data") && hasValue) {
            ++i;  // handled before the bridge was created
        } else if (a == QLatin1String("--size") && hasValue) {
            const QStringList wh = args.at(++i).split(QLatin1Char('x'));
            if (wh.size() == 2) testSize = QSize(wh.at(0).toInt(), wh.at(1).toInt());
        } else if (a == QLatin1String("--motion-probe") && hasValue) {
            testOptions.insert(QStringLiteral("motionProbe"), args.at(++i));
        } else if (a == QLatin1String("--exit-after") && hasValue) {
            exitAfterMs = args.at(++i).toInt();
        } else if (a == QLatin1String("--test-keys") && hasValue) {
            testKeys = args.at(++i);      // e.g. ctrl+o@20,ctrl+o@23,f11@26 (seconds after start)
        } else if (a == QLatin1String("--test-mouse") && hasValue) {
            testMouse = args.at(++i);     // e.g. move:30,0,100@20,left:1@22,left:0@22.5,drag:.3,.5,.6,.5@30
        } else if (a == QLatin1String("--test-mask") && hasValue) {
            testOptions.insert(QStringLiteral("mask"), args.at(++i));   // e.g. ellipse@28,drag@31 (Main.qml)
        } else if (a == QLatin1String("--test-chat") && hasValue) {
            testOptions.insert(QStringLiteral("chat"), args.at(++i));
        } else if (a == QLatin1String("--test-drag") && hasValue) {
            testDrag = args.at(++i);      // e.g. right:300@20,bottom:-150@24,topleft:-200;-40@28 (border drags)
        } else if (a == QLatin1String("--test-hole") && hasValue) {
            // Bağlantıyı kes's black hole this many seconds into a stream, the window grabbed along the way
            testOptions.insert(QStringLiteral("hole"), args.at(++i).toDouble());
        } else if (a == QLatin1String("--test-connect") && hasValue) {
            testConnect = args.at(++i);   // lan | psn: connect to my PS5 by itself (test runs)
        } else if (a == QLatin1String("--test-izle-yerel") && i + 3 < args.size()) {
            testIzleApi = args.at(++i);
            testIzleToken = args.at(++i);
            testIzleYid = args.at(++i);
        } else if (a == QLatin1String("--video-probe") && hasValue) {
            const QString mode = args.at(++i);
            if (mode == QLatin1String("hidden")) g_videoProbe = VideoProbeMode::Hidden;
            else if (mode == QLatin1String("no-region")) g_videoProbe = VideoProbeMode::NoRegion;
            else if (mode == QLatin1String("covers-only")) g_videoProbe = VideoProbeMode::CoversOnly;
            else if (mode != QLatin1String("normal")) {
                oviora::log::error("qml-window", "invalid video probe mode " + mode.toStdString());
                return 2;
            }
            g_videoHiddenByProbe = g_videoProbe == VideoProbeMode::Hidden;
        } else if (a == QLatin1String("--altyazi")) {
            subtitleTest |= 1;            // subtitle translation on for this run (not saved)
        } else if (a == QLatin1String("--altyazi-kayit")) {
            subtitleTest |= 1 | 2;        // ... and the read region images under logs/altyazi
        } else if (a == QLatin1String("--altyazi-deneme")) {
            subtitleTest |= 1 | 4;        // ... and a fake translated line once the stream runs
        } else if (a == QLatin1String("--reduced-motion")) {
            testOptions.insert(QStringLiteral("reducedMotion"), true);
        } else if (a == QLatin1String("--slow-animations") && hasValue) {
            // Qt's own slow mode, the same one its debugging tools use: every
            // animation driven by the unified timer runs N times longer, so a
            // timed screenshot lands on a known point of the curve.
            const qreal factor = args.at(++i).toDouble();
            if (factor > 1.0) {
                QUnifiedTimer::instance()->setSlowModeEnabled(true);
                QUnifiedTimer::instance()->setSlowdownFactor(factor);
                testOptions.insert(QStringLiteral("slowAnimations"), factor);
            }
        }
    }
    if (!testOptions.isEmpty() || testSize.isValid())
        oviora::log::info("qml", std::format("test switches: {} size={}x{}",
                                            QStringList(testOptions.keys()).join(',').toStdString(),
                                            testSize.width(), testSize.height()));
    oviora::log::info("qml-window", std::format("probe mode={}", videoProbeName()));
    if (subtitleTest) subtitles->setTestMode(true, subtitleTest & 2, subtitleTest & 4);
    engine.rootContext()->setContextProperty(QStringLiteral("vyTest"), testOptions);

    engine.loadFromModule("Oviora", "Main");

    if (engine.rootObjects().isEmpty()) {
        oviora::log::error("qml", "no root object: the QML module failed to load");
        return 1;
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!window) {
        oviora::log::error("qml", "root object is not a window");
        return 1;
    }
    if (testSize.isValid()) window->resize(testSize);
    g_mainWindow = window;
    if (!SetWindowSubclass(reinterpret_cast<HWND>(window->winId()), sizingSubclass, 1, 0))
        oviora::log::error("qml-window", "SetWindowSubclass for the 16:9 border drag failed");
    psnWeb->setWindow(window);

    // Fullscreen must own the whole monitor. Without telling the shell, the taskbar
    // stayed above the fullscreen picture on some systems (field report with the
    // control pill showing behind it): the shell decides "fullscreen application"
    // from its own heuristics, and the owned control window defeats them.
    // ITaskbarList2::MarkFullscreenWindow states it explicitly.
    QObject::connect(window, &QWindow::visibilityChanged, &app, [window](QWindow::Visibility visibility) {
        const bool full = visibility == QWindow::FullScreen;
        ITaskbarList2* taskbar = nullptr;
        const HRESULT created = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar));
        HRESULT marked = created;
        if (SUCCEEDED(created) && taskbar) {
            if (SUCCEEDED(taskbar->HrInit()))
                marked = taskbar->MarkFullscreenWindow(reinterpret_cast<HWND>(window->winId()), full ? TRUE : FALSE);
            taskbar->Release();
        }
        oviora::log::info("qml-window", std::format("taskbar fullscreen mark={} hr=0x{:X}", full, unsigned(marked)));
    });

    // Closing the window must run the engine's own teardown, not just end the loop.
    // A test harness that hard-kills the process (Stop-Process -Force) loses every
    // log line still in the file sink's 64 KB buffer - including the ones a test is
    // looking for, because the flush condition (250 ms since the last write) is
    // never reached when the process dies first. Qt's default for a window with no
    // explicit handler is to close it and exit on the last window, which does run
    // teardown; this makes it explicit so a harness can also ask for a clean exit
    // with --exit-after and get the same, complete log either way.
    QObject::connect(window, &QQuickWindow::closing, &app, [window](QQuickCloseEvent*) {
        // The shell may keep the window open (a PS5 stream asks what to do with the console
        // first: Genel -> Oyunu birakinca); its onClosing handler runs before this one and
        // says so in closeHeld.
        if (window->property("closeHeld").toBool()) {
            window->setProperty("closeHeld", false);
            oviora::log::info("qml", "closing: the shell keeps the window open for now");
            return;
        }
        oviora::log::info("qml", "closing: quitting the application loop");
        // Flush before quitting: the file sink is a 64 KB _IOFBF buffer, so a run
        // that ends without this loses its last lines (the exact ones a headless
        // check is looking for). closeFile() at teardown would also flush, but a
        // quit path is not guaranteed to reach it.
        oviora::Logger::instance().flush();
        QCoreApplication::quit();
    }, Qt::DirectConnection);
    // --test-login <file>: a test run signs in with the account in this file (kullanici=, sifre=; the test
    // account) unless it is already signed in as it. The password is read here and passed on, never logged.
    {
        const int at = QCoreApplication::arguments().indexOf(QStringLiteral("--test-login"));
        if (at > 0 && at + 1 < QCoreApplication::arguments().size()) {
            QFile f(QCoreApplication::arguments().at(at + 1));
            QString name, pass;
            if (f.open(QIODevice::ReadOnly | QIODevice::Text))
                for (const QString& line : QString::fromUtf8(f.readAll()).split(QLatin1Char(10))) {
                    const QString l = line.trimmed();
                    if (l.startsWith(QLatin1String("kullanici="))) name = l.mid(10);
                    else if (l.startsWith(QLatin1String("sifre="))) pass = l.mid(6);
                }
            if (!name.isEmpty() && !pass.isEmpty())
                QTimer::singleShot(1500, platform, [platform, name, pass] {
                    if (platform->user() == name) { oviora::log::info("test-login", "already signed in"); return; }
                    oviora::log::info("test-login", "signing in with the test account");
                    platform->login(name, pass);
                });
            else oviora::log::warn("test-login", "no account in the file");
        }
    }
    // --test-connect lan|psn: a test run connects to the first paired PS5 seen on this network,
    // or to my own PS5 over PSN through the Oviora server, without anyone clicking.
    if (!testConnect.isEmpty()) {
        auto* poll = new QTimer(&app);
        auto tries = std::make_shared<int>(0);
        QObject::connect(poll, &QTimer::timeout, &app, [poll, tries, testConnect, &bridge, platform] {
            if (++*tries > 60) { oviora::log::warn("test-connect", "nothing to connect to"); poll->stop(); return; }
            // wake: the "Uyandır" button (wake, wait until ready, connect by itself)
            if (testConnect == QLatin1String("lan") || testConnect == QLatin1String("wake")) {
                for (const auto& v : bridge.lanConsoles()) {
                    const auto c = v.toMap();
                    if (c.value("registered").toBool() && c.value("discovered").toBool()) {
                        oviora::log::info("test-connect", testConnect == QLatin1String("wake") ? "lan wake" : "lan connect");
                        if (testConnect == QLatin1String("wake")) bridge.lanWake(c.value("key").toString());
                        else bridge.lanConnect(c.value("key").toString());
                        poll->stop();
                        return;
                    }
                }
            } else if (testConnect == QLatin1String("psn") || testConnect == QLatin1String("psn-arkadas")) {
                // psn-arkadas: a PS5 a friend shared with this account (the server relays the PSN part)
                const bool friendConsole = testConnect == QLatin1String("psn-arkadas");
                for (const auto& v : platform->available()) {
                    const auto c = v.toMap();
                    if (c.value("mine").toBool() != friendConsole && !c.value("busy").toBool()) {
                        oviora::log::info("test-connect", friendConsole ? "psn connect to a friend's PS5" : "psn connect");
                        platform->connectTo(c.value("host").toString(), c.value("duid").toString());
                        poll->stop();
                        return;
                    }
                }
            }
        });
        poll->start(1000);
    }
    // --test-izle-yerel <apiUrl> <token> <yid>: test-only native viewer joining an İzlet broadcast.
    if (!testIzleApi.isEmpty() && !testIzleToken.isEmpty() && !testIzleYid.isEmpty()) {
        struct LocalWatchState {
            QNetworkAccessManager net;
            QString apiUrl;
            QString token;
            QString yid;
            QString iid;
            qint64 after = 0;
            bool stopped = false;
            bool polling = false;
            std::shared_ptr<oviora::remoteplay::WatchRoute> route;
        };
        auto st = std::make_shared<LocalWatchState>();
        st->apiUrl = testIzleApi;
        st->token = testIzleToken;
        st->yid = testIzleYid;

        auto postApi = [st](const QString& action, QJsonObject body,
                            std::function<void(const QJsonObject&, int, bool)> done) {
            body.insert(QStringLiteral("a"), action);
            body.insert(QStringLiteral("token"), st->token);
            body.insert(QStringLiteral("client"), QStringLiteral("win:test"));
            QNetworkRequest req{QUrl(st->apiUrl)};
            req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
            req.setTransferTimeout(5000);
            QNetworkReply* reply = st->net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
            QObject::connect(reply, &QNetworkReply::finished, &st->net, [reply, done = std::move(done)]() {
                const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const bool ok = (reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 400);
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (done) done(doc.isObject() ? doc.object() : QJsonObject{}, httpStatus, ok);
            });
        };

        auto* pollTimer = new QTimer(&app);
        pollTimer->setSingleShot(true);
        auto runPoll = std::make_shared<std::function<void()>>();
        *runPoll = [st, pollTimer, postApi, runPoll, &app]() {
            if (st->stopped || st->polling || st->iid.isEmpty()) return;
            st->polling = true;
            postApi(QStringLiteral("izle_poll"),
                    QJsonObject{{QStringLiteral("iid"), st->iid}, {QStringLiteral("after"), double(st->after)}},
                    [st, pollTimer, runPoll](const QJsonObject& r, int httpStatus, bool ok) {
                        st->polling = false;
                        if (st->stopped) return;
                        if (!ok) {
                            if (httpStatus == 403 || httpStatus == 404 || httpStatus == 410) {
                                st->stopped = true;
                                if (st->route) st->route->setEnded("Yayın bitti.");
                                return;
                            }
                            pollTimer->start(1000);
                            return;
                        }
                        for (const auto& v : r.value(QStringLiteral("signals")).toArray()) {
                            const QJsonObject sigData = v.toObject().value(QStringLiteral("data")).toObject();
                            if (st->route && !sigData.isEmpty()) {
                                st->route->pushRemote(QJsonDocument(sigData).toJson(QJsonDocument::Compact).toStdString());
                            }
                        }
                        st->after = std::max<qint64>(st->after, qint64(r.value(QStringLiteral("last")).toDouble(double(st->after))));
                        const QString state = r.value(QStringLiteral("state")).toString();
                        if (state == QLatin1String("bitti") || state == QLatin1String("hata")) {
                            QString err = r.value(QStringLiteral("error")).toString();
                            if (err.isEmpty()) err = QStringLiteral("Yayın bitti.");
                            oviora::log::info("test-izle-yerel", std::format("poll ended state={} reason={}",
                                state.toStdString(), err.toStdString()));
                            st->stopped = true;
                            if (st->route) st->route->setEnded(err.toStdString());
                            return;
                        }
                        pollTimer->start(state == QLatin1String("canli") ? 1500 : 50);
                    });
        };
        QObject::connect(pollTimer, &QTimer::timeout, &app, [runPoll]() { (*runPoll)(); });

        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [st, pollTimer, postApi]() {
            if (st->stopped) return;
            st->stopped = true;
            pollTimer->stop();
            if (!st->iid.isEmpty()) {
                postApi(QStringLiteral("izle_leave"), QJsonObject{{QStringLiteral("iid"), st->iid}}, nullptr);
            }
        });

        QTimer::singleShot(300, &app, [&app, &bridge, st, postApi, runPoll]() {
            oviora::log::info("test-izle-yerel", "joining broadcast");
            postApi(QStringLiteral("izle_join"), QJsonObject{{QStringLiteral("yid"), st->yid}},
                    [&app, &bridge, st, postApi, runPoll](const QJsonObject& r, int httpStatus, bool ok) {
                        if (!ok || r.value(QStringLiteral("iid")).toString().isEmpty()) {
                            oviora::log::error("test-izle-yerel", std::format("izle_join failed http={} err={}",
                                httpStatus, r.value(QStringLiteral("error")).toString().toStdString()));
                            return;
                        }
                        st->iid = r.value(QStringLiteral("iid")).toString();
                        const QString codec = r.value(QStringLiteral("codec")).toString(QStringLiteral("h264"));
                        const QString owner = r.value(QStringLiteral("owner")).toString();
                        const QString console = r.value(QStringLiteral("console")).toString();
                        const QString title = (!owner.isEmpty() && !console.isEmpty())
                            ? QStringLiteral("%1 · %2").arg(owner, console)
                            : (!owner.isEmpty() ? owner : QStringLiteral("Canlı Yayın"));

                        auto route = std::make_shared<oviora::remoteplay::WatchRoute>();
                        route->codec = codec.toStdString();
                        for (const auto& iceItem : r.value(QStringLiteral("ice")).toArray()) {
                            if (iceItem.isString()) {
                                route->iceServers.push_back(iceItem.toString().toStdString());
                            } else if (iceItem.isObject()) {
                                const QJsonObject obj = iceItem.toObject();
                                const QJsonValue urlsVal = obj.value(QStringLiteral("urls"));
                                if (urlsVal.isString()) {
                                    route->iceServers.push_back(urlsVal.toString().toStdString());
                                } else if (urlsVal.isArray()) {
                                    for (const auto& u : urlsVal.toArray()) {
                                        if (u.isString()) route->iceServers.push_back(u.toString().toStdString());
                                    }
                                }
                            }
                        }
                        std::weak_ptr<LocalWatchState> weakSt = st;
                        route->sendSignal = [&app, weakSt, postApi](std::string json) {
                            QMetaObject::invokeMethod(&app, [weakSt, postApi, json = std::move(json)]() {
                                auto locked = weakSt.lock();
                                if (!locked || locked->stopped || locked->iid.isEmpty()) return;
                                const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
                                if (!doc.isObject()) return;
                                postApi(QStringLiteral("izle_signal"),
                                        QJsonObject{{QStringLiteral("iid"), locked->iid}, {QStringLiteral("data"), doc.object()}},
                                        nullptr);
                            }, Qt::QueuedConnection);
                        };
                        st->route = route;
                        oviora::log::info("test-izle-yerel", std::format("joined codec={} title={}",
                            codec.toStdString(), title.toStdString()));
                        if (!bridge.connectWatch(route, title)) {
                            oviora::log::error("test-izle-yerel", "connectWatch returned false");
                            st->stopped = true;
                            return;
                        }
                        (*runPoll)();
                    });
        });
    }
    // --test-drag: a border drag of this window without the mouse. The window gets what the
    // system's sizing loop sends it (WM_ENTERSIZEMOVE, WM_SIZING with the moved edge, the new
    // rect, WM_EXITSIZEMOVE); nothing is sent to the desktop or any other window.
    for (const QString& item : testDrag.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        // edge:dx[;dy]@seconds (a corner takes dx;dy)
        const QString spec = item.section(QLatin1Char('@'), 0, 0).trimmed();
        const int atMs = int(item.section(QLatin1Char('@'), 1, 1).toDouble() * 1000);
        const QString edgeName = spec.section(QLatin1Char(':'), 0, 0);
        const QStringList d = spec.section(QLatin1Char(':'), 1, 1).split(QLatin1Char(';'));
        static const QHash<QString, int> edges{{"left", WMSZ_LEFT}, {"right", WMSZ_RIGHT}, {"top", WMSZ_TOP}, {"bottom", WMSZ_BOTTOM},
            {"topleft", WMSZ_TOPLEFT}, {"topright", WMSZ_TOPRIGHT}, {"bottomleft", WMSZ_BOTTOMLEFT}, {"bottomright", WMSZ_BOTTOMRIGHT}};
        if (!edges.contains(edgeName) || atMs <= 0 || d.isEmpty()) continue;
        const int edge = edges.value(edgeName);
        const int dx = d.at(0).toInt(), dy = d.size() > 1 ? d.at(1).toInt() : d.at(0).toInt();
        QTimer::singleShot(atMs, &app, [window, edge, edgeName, dx, dy] {
            const HWND hwnd = reinterpret_cast<HWND>(window->winId());
            RECT r{};
            GetWindowRect(hwnd, &r);
            const RECT before = r;
            if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) r.left -= dx;
            if (edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT) r.right += dx;
            if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) r.top -= dy;
            if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT) r.bottom += dy;
            const RECT proposed = r;
            SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
            SendMessageW(hwnd, WM_SIZING, WPARAM(edge), reinterpret_cast<LPARAM>(&r));
            SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
            RECT client{};
            GetClientRect(hwnd, &client);
            oviora::log::info("test-drag", std::format("{} {},{}: window {}x{} -> proposed {}x{} -> sized {}x{}; client {}x{} (aspect {:.4f})",
                edgeName.toStdString(), dx, dy, before.right - before.left, before.bottom - before.top,
                proposed.right - proposed.left, proposed.bottom - proposed.top, r.right - r.left, r.bottom - r.top,
                client.right, client.bottom, client.bottom ? double(client.right) / client.bottom : 0.0));
        });
    }
    // --test-keys: key presses through Qt's own input path into this window only (the way
    // QTest does), so shortcuts are exercised as with a real keyboard; nothing reaches the desktop.
    for (const QString& item : testKeys.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString keyName = item.section(QLatin1Char('@'), 0, 0).trimmed();
        const int atMs = int(item.section(QLatin1Char('@'), 1, 1).toDouble() * 1000);
        const QKeySequence seq = QKeySequence::fromString(keyName, QKeySequence::PortableText);
        if (seq.isEmpty() || atMs <= 0) continue;
        const QKeyCombination combo = seq[0];
        QTimer::singleShot(atMs, &app, [window, combo, keyName] {
            const QWindow* focus = QGuiApplication::focusWindow();
            oviora::log::info("test-keys", std::format("press {} active={} focusWindow={} focusItem={}", keyName.toStdString(), window->isActive(),
                focus ? focus->objectName().toStdString() + "/" + focus->metaObject()->className() : std::string("none"),
                window->activeFocusItem() ? window->activeFocusItem()->metaObject()->className() : "none"));
            // Qt-internal focus only (as a user's click would give it): the desktop's foreground
            // window is left alone, a test never steals focus from what the user is doing.
            if (QGuiApplication::focusWindow() != window) {
                QWindowSystemInterface::handleFocusWindowChanged(window, Qt::OtherFocusReason);
                QWindowSystemInterface::flushWindowSystemEvents();
            }
            QWindowSystemInterface::handleKeyEvent(window, QEvent::KeyPress, int(combo.key()), combo.keyboardModifiers());
            QWindowSystemInterface::handleKeyEvent(window, QEvent::KeyRelease, int(combo.key()), combo.keyboardModifiers());
        });
    }
    // --test-mouse: mouse aim and the mouse touchpad without the real mouse. Raw input packets
    // go straight to the bridge as WM_INPUT would bring them (move:dx,dy,count every 4 ms;
    // left|right|middle:1|0 a button), pointer events through Qt's own input path into this
    // window only (drag:x0,y0,x1,y1 with the left button, rclick:x,y; 0..1 across the window).
    // The real cursor is never clipped, hidden or moved, and the desktop gets nothing.
    if (!testMouse.isEmpty()) bridge.setMouseAimTest(true);
    {
        // Items are kind:numbers@seconds, separated by ','; the numbers inside use ',' too.
        static const QRegularExpression itemRe(QStringLiteral(R"(([a-z]+):([-0-9.,]+)@([0-9.]+))"));
        for (auto it = itemRe.globalMatch(testMouse); it.hasNext();) {
            const auto m = it.next();
            const QString kind = m.captured(1);
            QList<double> v;
            for (const QString& n : m.captured(2).split(QLatin1Char(','), Qt::SkipEmptyParts)) v << n.toDouble();
            const int atMs = int(m.captured(3).toDouble() * 1000);
            if (atMs <= 0) continue;
            auto focus = [window] {
                if (QGuiApplication::focusWindow() != window) {
                    QWindowSystemInterface::handleFocusWindowChanged(window, Qt::OtherFocusReason);
                    QWindowSystemInterface::flushWindowSystemEvents();
                }
            };
            auto pointer = [window](double x, double y, Qt::MouseButtons buttons, Qt::MouseButton button, QEvent::Type type) {
                const QPointF local(x * window->width(), y * window->height());
                QWindowSystemInterface::handleMouseEvent(window, local, window->mapToGlobal(local), buttons, button, type);
                QWindowSystemInterface::flushWindowSystemEvents();
            };
            QTimer::singleShot(atMs, &app, [&app, &bridge, window, focus, pointer, kind, v, item = m.captured(0)] {
                oviora::log::info("test-mouse", std::format("{} captured={} focusWindow={}", item.toStdString(), bridge.mouseAimCaptured(),
                    QGuiApplication::focusWindow() == window));
                focus();
                if (kind == QLatin1String("move") && v.size() >= 2) {
                    const int count = v.size() >= 3 ? std::max(1, int(v[2])) : 1;
                    for (int n = 0; n < count; ++n)
                        QTimer::singleShot(n * 4, &app, [&bridge, dx = int(v[0]), dy = int(v[1])] { bridge.testRawMouse(dx, dy, 0); });
                } else if ((kind == QLatin1String("left") || kind == QLatin1String("right") || kind == QLatin1String("middle")) && !v.isEmpty()) {
                    const bool down = v[0] != 0;
                    const unsigned flags = kind == QLatin1String("left") ? (down ? RI_MOUSE_LEFT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_UP)
                        : kind == QLatin1String("right") ? (down ? RI_MOUSE_RIGHT_BUTTON_DOWN : RI_MOUSE_RIGHT_BUTTON_UP)
                        : (down ? RI_MOUSE_MIDDLE_BUTTON_DOWN : RI_MOUSE_MIDDLE_BUTTON_UP);
                    bridge.testRawMouse(0, 0, flags);
                } else if (kind == QLatin1String("drag") && v.size() >= 4) {
                    pointer(v[0], v[1], Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress);
                    for (int n = 1; n <= 10; ++n)
                        QTimer::singleShot(n * 30, &app, [pointer, v, n] {
                            const double t = n / 10.0;
                            pointer(v[0] + (v[2] - v[0]) * t, v[1] + (v[3] - v[1]) * t, Qt::LeftButton, Qt::NoButton, QEvent::MouseMove);
                        });
                    QTimer::singleShot(360, &app, [pointer, v] { pointer(v[2], v[3], Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease); });
                } else if (kind == QLatin1String("rclick") && v.size() >= 2) {
                    pointer(v[0], v[1], Qt::RightButton, Qt::RightButton, QEvent::MouseButtonPress);
                    QTimer::singleShot(150, &app, [pointer, v] { pointer(v[0], v[1], Qt::NoButton, Qt::RightButton, QEvent::MouseButtonRelease); });
                }
            });
        }
    }
    if (exitAfterMs > 0) {
        // Self-terminating test run: the same teardown path as a user closing the
        // window, so the log a harness reads is complete either way.
        QTimer::singleShot(exitAfterMs, &app, [window] {
            oviora::log::info("qml", "exit-after elapsed: closing the window");
            // A test run ends without the shell's questions (rest mode on closing), unless
            // OVIORA_TEST_CLOSE_ASKS=1 tests exactly those
            if (!qEnvironmentVariableIsSet("OVIORA_TEST_CLOSE_ASKS")) window->setProperty("closeWithoutAsking", true);
            window->close();
        });
    }

    // Focus changes are diagnostic only: they never pause playback, change the
    // selected effects, or reduce submission cadence. Keep the actual throughput
    // and Windows process throttle state alongside field reports of background lag.
    QObject::connect(window, &QWindow::activeChanged, &app, [window, &controller] {
        const auto s=controller.snapshot();
        PROCESS_POWER_THROTTLING_STATE throttle{};
        throttle.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        const bool known=GetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&throttle,sizeof(throttle));
        const DWORD error=known?0:GetLastError();
        oviora::log::info("app-focus",std::format(
            "active={} running={} transport={} sourceFps={:.2f} submitFps={:.2f} nr={} sr={} fg={} powerKnown={} control=0x{:X} state=0x{:X} err={}",
            window->isActive(),s.running,int(s.transport),s.nominalSourceFps,s.submissionFps.value_or(-1),
            s.nrActive,s.srActive,s.fgActive,known,throttle.ControlMask,throttle.StateMask,error));
    });

    // Reparent the native window under the QML window and keep it in step. The
    // host item is looked up by objectName so the QML side owns the layout and
    // this side only follows it.
    // The host item is followed per rendered frame rather than on a timer or through
    // its own change signals. afterAnimating fires on the GUI thread before every
    // frame Qt produces, so anything that moves the host (layout, a resize, the
    // page switch, a cinema height animation, a popover scaling in) is picked up in
    // the same frame, and an idle scene renders no frames and costs nothing. Change
    // signals alone were not enough: the first layout pass can leave the host at its
    // default 0x0 with no change to observe, which is how the engine once built a
    // swapchain from a 1x1 window. The native calls only run when a value changed.
    auto follow = [window, &bridge] {
        if (!g_video) return;
        const auto f0 = std::chrono::steady_clock::now();
        syncVideoCursor(window->property("fullscreenCursorHidden").toBool());
        // Only the pre-open hook attaches the popup and converts it to WS_CHILD.
        // GetParent() is null for an unattached WS_POPUP, so reparenting here on
        // every animation frame can starve input and timers before any source opens.
        if (!bridge.hasSource()) {
            // The window stays up for the presenter, but empty: the QML stage and its
            // "正在连接" pill show instead of the last source's frozen frame.
            if (bridge.openingSource()) { blankVideo("opening"); return; }
            if (IsWindowVisible(g_video)) {
                oviora::log::info("qml-window", "follow: hide video because source snapshot is inactive");
                ShowWindow(g_video, SW_HIDE);
            }
            if (g_traceUiStalls) {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
                ++g_uiStalls.followCount;
                if (ms > g_uiStalls.followMaxMs) g_uiStalls.followMaxMs = ms;
            }
            return;
        }
        static QPointer<QQuickItem> host;
        if (!host) {
            host = window->findChild<QQuickItem*>(QStringLiteral("videoHost"));
            // Report the search: "the log is silent" and "the lookup failed" look
            // identical otherwise.
            oviora::log::info("qml", std::format("video host lookup: found={} contentChildren={}",
                                                host != nullptr,
                                                window->contentItem() ? window->contentItem()->childItems().size() : -1));
            if (!host) return;
        }
        // Report the resolved geometry once at each distinct size, so the log
        // says what the video window was actually given.
        static int lastW = -1, lastH = -1;
        const int w = int(host->width()), h = int(host->height());
        if (w != lastW || h != lastH) {
            lastW = w; lastH = h;
            oviora::log::info("qml", std::format("video host geometry {}x{} at {},{}",
                                                w, h, int(host->x()), int(host->y())));
        }
        if (syncVideoGeometry(window, host))
            syncVideoCovers(window, host, sceneInsetFraction(window));
        if (g_traceUiStalls) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
            ++g_uiStalls.followCount;
            if (ms > g_uiStalls.followMaxMs) g_uiStalls.followMaxMs = ms;
            if (ms >= 2.0) oviora::log::info("ui-stall-follow", std::format("follow ms={:.2f}", ms));
        }
    };
    if (g_traceUiStalls) {
        struct TraceNativeFilter final : QAbstractNativeEventFilter {
            bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override {
                if (type != "windows_generic_MSG" || !message) return false;
                const auto* msg = static_cast<const MSG*>(message);
                if (msg->message == WM_MOUSEMOVE) {
                    const auto now = std::chrono::steady_clock::now();
                    if (g_uiStalls.lastWmMouse.time_since_epoch().count() != 0) {
                        const double gap = std::chrono::duration<double, std::milli>(now - g_uiStalls.lastWmMouse).count();
                        if (gap > g_uiStalls.wmMouseMaxGapMs) g_uiStalls.wmMouseMaxGapMs = gap;
                    }
                    g_uiStalls.lastWmMouse = now;
                    ++g_uiStalls.wmMouseCount;
                } else if (msg->message == WM_SETCURSOR) {
                    ++g_uiStalls.wmSetCursorCount;
                }
                return false;
            }
        };
        static TraceNativeFilter traceNativeFilter;
        app.installNativeEventFilter(&traceNativeFilter);

        auto* traceDump = new QTimer(&app);
        QObject::connect(traceDump, &QTimer::timeout, &app, [] {
            oviora::log::info("ui-stall-summary", std::format(
                "notifyCount={} totalMs={:.2f} maxMs={:.2f} >=4ms={} >=16ms={} >=50ms={} followCount={} followMaxMs={:.2f} wmMouse={} wmMouseMaxGapMs={:.2f} wmSetCursor={}",
                g_uiStalls.notifyCount, g_uiStalls.notifyTotalMs, g_uiStalls.notifyMaxMs,
                g_uiStalls.over4Ms, g_uiStalls.over16Ms, g_uiStalls.over50Ms,
                g_uiStalls.followCount, g_uiStalls.followMaxMs,
                g_uiStalls.wmMouseCount, g_uiStalls.wmMouseMaxGapMs, g_uiStalls.wmSetCursorCount));
            g_uiStalls = {};
        });
        traceDump->start(2000);

        static std::atomic<int> loggedFrames{0};
        QObject::connect(window, &QQuickWindow::beforeSynchronizing, &app, [window] {
            if (loggedFrames.fetch_add(1) < 30) {
                oviora::log::info("ui-trace-frame", std::format(
                    "sync n={} visible={} active={}",
                    loggedFrames.load(), window->isVisible(), window->isActive()));
            }
        }, Qt::DirectConnection);
    }
    if (testWmMouse) {
        auto* mouseSim = new QTimer(&app);
        mouseSim->setTimerType(Qt::PreciseTimer);
        auto step = std::make_shared<int>(0);
        QObject::connect(mouseSim, &QTimer::timeout, &app, [window, step] {
            if (!window || !window->isVisible()) return;
            const HWND mainHwnd = reinterpret_cast<HWND>(window->winId());
            const HWND target = mainHwnd;
            for (int k = 0; k < 5; ++k) {
                const int x = 200 + ((*step * 7) % 400);
                const int y = 200 + ((*step * 3) % 250);
                ++(*step);
                PostMessageW(target, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
                PostMessageW(target, WM_SETCURSOR, WPARAM(target), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            }
        });
        QTimer::singleShot(2000, &app, [mouseSim] { mouseSim->start(5); });
    }
    if (!testVideo.isEmpty()) {
        QTimer::singleShot(1500, &app, [&bridge, testVideo] { bridge.openTestVideo(testVideo); });
    }
    if (testPadCapture) {
        QTimer::singleShot(1500, &app, [&bridge] { bridge.padCapture(true); });
    }
    // Hold-to-compare (V by default) is the bridge's application event filter:
    // the key is rebindable (设置 → 快捷键) and can be switched off (显示 →
    // 按住 V 查看原画). A second hard-wired V filter here used to override both.
    QObject::connect(window, &QQuickWindow::afterAnimating, &app, follow);
    QObject::connect(&bridge, &ui::QmlPlayerBridge::snapshotChanged, &app, follow);
    // The first frame may come before the page layout settles; one pass after the
    // event loop starts covers a scene that then never animates.
    QTimer::singleShot(0, &app, follow);
    // How often the Qt windows present. OBS game capture locks onto one swapchain and only
    // moves to another after 16 presents in a row from it, so a UI that keeps presenting
    // during playback keeps OBS on the UI instead of the video (field report 2026-10-01).
    {
        static std::atomic<int> mainFrames{0}, barFrames{0};
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [] { ++mainFrames; }, Qt::DirectConnection);
        if (auto* bar = window->findChild<QQuickWindow*>(QStringLiteral("fullscreenBar")))
            QObject::connect(bar, &QQuickWindow::frameSwapped, &app, [] { ++barFrames; }, Qt::DirectConnection);
        auto* frameLog = new QTimer(&app);
        QObject::connect(frameLog, &QTimer::timeout, &app, [] {
            const int m = mainFrames.exchange(0), b = barFrames.exchange(0);
            if (m || b) oviora::log::info("qml-frames", std::format("ui presents per 10 s: main={} controlBar={}", m, b));
        });
        frameLog->start(10000);
    }
    // A GPU reset takes the interface's own device too (logs 56): Qt cleans up the scene
    // graph and builds it again on a new device, but Qt 6.8.3's curve-rendered Shapes come
    // back empty, so every icon was missing. Tell the icons to rebuild their paths.
    {
        const auto watch = [&app, &bridge](QQuickWindow* w, const QString& name) {
            auto lost = std::make_shared<std::atomic<bool>>(false);
            QObject::connect(w, &QQuickWindow::sceneGraphInvalidated, &app, [lost] { lost->store(true); }, Qt::DirectConnection);
            QObject::connect(w, &QQuickWindow::sceneGraphInitialized, &bridge, [lost, &bridge, name] {
                if (lost->exchange(false)) bridge.noteUiDeviceRecreated(name);
            }, Qt::QueuedConnection);
        };
        watch(window, QStringLiteral("main"));
        if (auto* bar = window->findChild<QQuickWindow*>(QStringLiteral("fullscreenBar"))) watch(bar, QStringLiteral("controlBar"));
    }
    // Test only: OVIORA_TEST_DPI_FLIP=<ms> sends the window the WM_DPICHANGED a move to a
    // 150 % screen would, then back to 100 % three seconds later, so the screen-change
    // crash (field report 2026-10-01, two monitors) can be reproduced on one monitor.
    if (const auto flip = qEnvironmentVariableIntValue("OVIORA_TEST_DPI_FLIP"); flip > 0) {
        const auto send = [window](UINT dpi) {
            const HWND hwnd = reinterpret_cast<HWND>(window->winId());
            RECT r{};
            GetWindowRect(hwnd, &r);
            oviora::log::info("dpi-test", std::format("WM_DPICHANGED dpi={}", dpi));
            SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&r));
        };
        QTimer::singleShot(flip, &app, [send] { send(144); });
        QTimer::singleShot(flip + 3000, &app, [send] { send(96); });
    }
    // Test only: OVIORA_TEST_UI_DEVICE_REMOVE=<ms> removes the interface's own D3D12 device
    // on its render thread, as a GPU reset does (logs 56: "Graphics device lost, cleaning up
    // scenegraph and releasing RHI"), so the interface's recovery can be checked without one.
    if (const auto remove = qEnvironmentVariableIntValue("OVIORA_TEST_UI_DEVICE_REMOVE"); remove > 0) {
        QTimer::singleShot(remove, &app, [window] {
            auto pending = std::make_shared<QMetaObject::Connection>();
            *pending = QObject::connect(window, &QQuickWindow::beforeRendering, window, [window, pending] {
                QObject::disconnect(*pending);
                auto* device = window->rendererInterface()
                    ? static_cast<ID3D12Device*>(window->rendererInterface()->getResource(window, QSGRendererInterface::DeviceResource)) : nullptr;
                ID3D12Device5* device5 = nullptr;
                const bool ok = device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5)));
                oviora::log::warn("ui-device-test", std::format("removing the interface D3D12 device ok={}", ok));
                if (ok) { device5->RemoveDevice(); device5->Release(); }
            }, Qt::DirectConnection);
            window->update();
        });
    }

    // Before every open, place the native window and force the pending layout to
    // be applied so the client size the presenter reads is the real one.
    bridge.setPreOpenHook([window, &bridge] {
        auto* host = window->findChild<QQuickItem*>(QStringLiteral("videoHost"));
        oviora::log::info("qml", std::format("pre-open: host={} size={}x{} contentChildren={}",
                                            host != nullptr,
                                            host ? int(host->width()) : -1,
                                            host ? int(host->height()) : -1,
                                            window->contentItem() ? window->contentItem()->childItems().size() : -1));
        // Parent first: a WS_CHILD window without a parent is not realized, so its
        // rect stays 0x0 and any SetWindowPos is discarded. Reparenting here, not
        // on a later timer tick, is what lets the geometry below take effect.
        if (GetParent(g_video) == nullptr) {
            const HWND parent = reinterpret_cast<HWND>(window->winId());
            SetParent(g_video, parent);
            // SetParent alone leaves the popup style in place; a child style is what
            // makes the window clip to its parent and behave as part of the layout.
            const LONG_PTR style = GetWindowLongPtrW(g_video, GWL_STYLE);
            SetWindowLongPtrW(g_video, GWL_STYLE,
                              (style & ~(WS_POPUP)) | WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
        }
        const bool validHost = host && syncVideoGeometry(window, host);
        if (!validHost) {
            oviora::log::error("qml-window", "pre-open: video host has no valid client rectangle");
            return;
        }
        const BOOL wasVisible = IsWindowVisible(g_video);
        ShowWindow(g_video, SW_SHOWNA);
        oviora::log::info("qml-window", std::format(
            "pre-open: show video wasVisible={} nowVisible={} sourceSnapshot={}",
            wasVisible != 0, IsWindowVisible(g_video) != 0, bridge.hasSource()));
        if (g_videoHiddenByProbe) ShowWindow(g_video, SW_HIDE);
        UpdateWindow(g_video);
        // Report the native window's own client rect, not the QML item's: the
        // presenter builds its swapchain from this, and the two sizes can differ.
        // The size the engine will build its swapchain from. Reporting it is what
        // makes a regression here visible instead of a 1x1 surface with no clue.
        RECT client{};
        GetClientRect(g_video, &client);
        oviora::log::info("qml", std::format("video window {}x{} parent={}",
                                            client.right, client.bottom,
                                            GetParent(g_video) != nullptr));
    });

    // A file on the command line opens immediately. This exists so playback can
    // be verified end to end by a test rather than by a person clicking around;
    // it is not a user-facing feature.
    const int rc = app.exec();
    // The hooks capture the bridge, which is destroyed before the app (and its child, the server)
    if (platform) platform->dropHooks();
    bridge.setAccountCall({});
    if (rc == 42) {
        restartProgram = QCoreApplication::applicationFilePath();
        // Preserve the selected profile, not transient capture/open/test flags.
        const auto args = QCoreApplication::arguments();
        for (int i = 1; i + 1 < args.size(); ++i) {
            if (args[i] == QStringLiteral("--data-dir")) {
                restartArgs << args[i] << args[i + 1];
                ++i;
            }
        }
        if (app.property("ovioraOverlayCompatRestart").toBool())
            restartArgs << QStringLiteral("--overlay-compat-restart");
        restartArgs << QStringLiteral("--page") << QStringLiteral("set");
    }
    if (g_video) { DestroyWindow(g_video); g_video = nullptr; }
    qInstallMessageHandler(nullptr);  // its logger may already be gone while the statics unwind
    return rc;
}

int main(int argc, char** argv) {
    // A child for the AMD HIP self-test (AmdHipPreflight.h): no Qt, no window and
    // no player state, only the HIP calls and their result lines on the pipe.
    if (argc >= 2 && std::strcmp(argv[1], "--amd-hip-preflight") == 0) {
        int count = 0;
        wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
        const int code = wide ? oviora::pipeline::amdhip::preflightMain(count, wide) : 2;
        if (wide) LocalFree(wide);
        return code;
    }
    {
        std::wstring self(32768, L'\0');
        self.resize(GetModuleFileNameW(nullptr, self.data(), DWORD(self.size())));
        oviora::pipeline::amdhip::setPreflightExecutable(self);
    }
    QString program;
    QStringList arguments;
    const int result = runApplication(argc, argv, program, arguments);
    if (result != 42) return result;
    // Player, QML engine and QApplication have all been destroyed.
    if (QProcess::startDetached(program, arguments)) return 0;
    MessageBoxW(nullptr, L"Ayarlar kaydedildi ama Oviora kendiliğinden yeniden başlatılamadı. Lütfen Oviora'yı yeniden aç.\nSettings saved. Automatic restart failed; please reopen Oviora.",
                L"Oviora", MB_OK | MB_ICONERROR);
    return 1;
}
