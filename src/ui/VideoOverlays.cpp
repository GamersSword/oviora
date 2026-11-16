#include "VideoOverlays.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <windowsx.h>
#include <objidl.h>
// Built with NOMINMAX: GDI+ headers use unqualified min/max
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include <QCoreApplication>
#include <QDir>
#include <QImage>

#include "oviora/Log.h"

#pragma comment(lib, "gdiplus.lib")

namespace oviora::ui {
void ensureGdiplus();   // QmlPlayerBridge (Subtitles.cpp): GDI+ started once for the process

namespace {
// The old Oviora's colours (qml/Oviora/Tema.qml)
constexpr QRgb kSurface = 0xFF17151E, kLineStrong = 0xFF36323F, kLine = 0xFF26232F;
constexpr QRgb kText = 0xFFF1EEF6, kMuted = 0xFFA49DB0, kAccentText = 0xFFA996FF, kAccent = 0xFF7B5CFF;
// Panel ground: Tema.surface at about two thirds, so the picture shows through
constexpr BYTE kPanelAlpha = 0xAC;

LRESULT CALLBACK statsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_PAINT) { PAINTSTRUCT ps{}; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

// The app's own Figtree (qml/Oviora/fonts), private to this process, for GDI's ClearType text.
// Falls back to Segoe UI when it cannot be loaded.
const wchar_t* bodyFace()
{
    static const wchar_t* face = [] {
        const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("qml/Oviora/fonts/Figtree.ttf"));
        const int added = AddFontResourceExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), FR_PRIVATE, nullptr);
        const wchar_t* chosen = added > 0 ? L"Figtree" : L"Segoe UI";
        if (added > 0) {
            // GDI silently substitutes an unknown face: check what it really picked
            HDC dc = CreateCompatibleDC(nullptr);
            HFONT f = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Figtree");
            HGDIOBJ old = SelectObject(dc, f);
            wchar_t got[LF_FACESIZE]{};
            GetTextFaceW(dc, LF_FACESIZE, got);
            SelectObject(dc, old); DeleteObject(f); DeleteDC(dc);
            if (_wcsicmp(got, L"Figtree") != 0) chosen = L"Segoe UI";
        }
        oviora::log::info("stats-overlay", std::format("text face {} (Figtree loaded={})", QString::fromWCharArray(chosen).toStdString(), added > 0));
        return chosen;
    }();
    return face;
}

HFONT makeFont(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, bodyFace());
}

int textWidth(HDC dc, HFONT font, const QString& text)
{
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    const std::wstring w = text.toStdWString();
    GetTextExtentPoint32W(dc, w.c_str(), int(w.size()), &size);
    SelectObject(dc, old);
    return size.cx;
}

void roundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius)
{
    const float d = std::min({radius * 2, w, h});
    if (d < 1) { path.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

Gdiplus::Color gdiColor(QRgb c, BYTE alpha) { return Gdiplus::Color(alpha, BYTE(qRed(c)), BYTE(qGreen(c)), BYTE(qBlue(c))); }

// ClearType text over a (half) transparent premultiplied BGRA picture: GDI draws each item white
// on black, which gives one coverage per colour channel; that coverage then mixes the item's
// colour over what is drawn already (as TranslationOverlay does for the subtitles).
struct TextItem {
    QString text;
    HFONT font = nullptr;
    int x = 0, baseline = 0;    // left end (or right end with alignRight)
    bool alignRight = false;
    QRgb color = kText;
};
void drawTexts(void* bits, int W, int H, const std::vector<TextItem>& items)
{
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = W;
    info.bmiHeader.biHeight = -H;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* cover = nullptr;
    HBITMAP coverage = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &cover, nullptr, 0);
    if (!coverage || !cover) { if (coverage) DeleteObject(coverage); DeleteDC(dc); return; }
    HGDIOBJ oldBitmap = SelectObject(dc, coverage);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    for (const TextItem& item : items) {
        if (item.text.isEmpty()) continue;
        std::memset(cover, 0, size_t(W) * size_t(H) * 4);
        HGDIOBJ oldFont = SelectObject(dc, item.font);
        const std::wstring w = item.text.toStdWString();
        SetTextAlign(dc, TA_BASELINE | (item.alignRight ? TA_RIGHT : TA_LEFT) | TA_NOUPDATECP);
        TextOutW(dc, item.x, item.baseline, w.c_str(), int(w.size()));
        SelectObject(dc, oldFont);
        GdiFlush();
        const int cr = qRed(item.color), cg = qGreen(item.color), cb = qBlue(item.color);
        auto* dst = static_cast<uint8_t*>(bits);
        const auto* src = static_cast<const uint8_t*>(cover);
        for (size_t i = 0, n = size_t(W) * size_t(H); i < n; ++i) {
            const uint8_t* s = src + 4 * i;
            const int kb = s[0], kg = s[1], kr = s[2];
            const int k = std::max({kb, kg, kr});
            if (!k) continue;
            uint8_t* d = dst + 4 * i;
            d[0] = uint8_t((d[0] * (255 - kb) + cb * kb) / 255);
            d[1] = uint8_t((d[1] * (255 - kg) + cg * kg) / 255);
            d[2] = uint8_t((d[2] * (255 - kr) + cr * kr) / 255);
            d[3] = uint8_t(d[3] + (255 - d[3]) * k / 255);
            // Premultiplied: no channel above alpha
            d[0] = std::min(d[0], d[3]); d[1] = std::min(d[1], d[3]); d[2] = std::min(d[2], d[3]);
        }
    }
    SelectObject(dc, oldBitmap);
    DeleteObject(coverage);
    DeleteDC(dc);
}

bool present(HWND window, HBITMAP bitmap, int W, int H)
{
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    SIZE size{W, H};
    POINT origin{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const bool ok = UpdateLayeredWindow(window, screen, nullptr, &size, memory, &origin, 0, &blend, ULW_ALPHA) != FALSE;
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
}

HBITMAP makeCanvas(int W, int H, void** bits)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = W;
    info.bmiHeader.biHeight = -H;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (bitmap && *bits) std::memset(*bits, 0, size_t(W) * size_t(H) * 4);
    return bitmap;
}
} // namespace

// --- statistics panel -----------------------------------------------------------------------------
StatsOverlay::~StatsOverlay()
{
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
}

void StatsOverlay::hide()
{
    if (window_ && IsWindowVisible(window_)) ShowWindow(window_, SW_HIDE);
    signature_.clear();
    shown_ = {};
    contentWidth_ = 0;
}

QRect StatsOverlay::follow(HWND video)
{
    if (!window_ || video != parent_ || shown_.isEmpty() || !IsWindowVisible(window_)) return shown_;
    RECT client{};
    GetClientRect(video, &client);
    const double s = std::max(1.0, GetDpiForWindow(video) / 96.0);
    const int margin = int(std::lround(16 * s));
    const int x = client.right - shown_.width() - margin;
    if (x == shown_.x()) return shown_;
    if (client.right < shown_.width() + margin) { hide(); return {}; }
    SetWindowPos(window_, HWND_TOP, x, margin, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
    shown_.moveTo(x, margin);
    return shown_;
}

QRect StatsOverlay::update(HWND video, const std::vector<StatsGroup>& groups)
{
    if (!video || !IsWindow(video) || groups.empty()) { hide(); return {}; }
    if (window_ && parent_ != video) { DestroyWindow(window_); window_ = nullptr; }
    if (!window_) {
        ensureGdiplus();
        WNDCLASSW wc{};
        wc.lpfnWndProc = statsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"OvioraStreamStats";
        RegisterClassW(&wc);
        window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_CHILD,
                                  0, 0, 1, 1, video, nullptr, wc.hInstance, nullptr);
        parent_ = video;
        oviora::log::info("stats-overlay", std::format("window created ok={} err={}", window_ != nullptr, window_ ? 0 : GetLastError()));
        if (!window_) return {};
    }
    RECT client{};
    GetClientRect(video, &client);
    const double s = std::max(1.0, GetDpiForWindow(video) / 96.0);
    auto px = [s](double v) { return int(std::lround(v * s)); };

    QString signature = QStringLiteral("%1x%2@%3|").arg(client.right).arg(client.bottom).arg(s);
    for (const auto& g : groups) {
        signature += g.title + QLatin1Char('#');
        for (const auto& r : g.rows) signature += r.label + QLatin1Char('=') + r.value + QString::number(r.tone, 16) + QLatin1Char(';');
    }
    if (signature == signature_ && IsWindowVisible(window_)) return shown_;

    const int titlePx = px(11.5), labelPx = px(13), valuePx = px(13);
    HFONT titleFont = makeFont(titlePx, FW_SEMIBOLD);
    HFONT labelFont = makeFont(labelPx, FW_NORMAL);
    HFONT valueFont = makeFont(valuePx, FW_SEMIBOLD);
    HDC measure = CreateCompatibleDC(nullptr);
    const int pad = px(14), gap = px(20), rowH = px(19), titleH = px(20), groupGap = px(9);
    int contentW = px(212), height = pad;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& g = groups[gi];
        if (gi) height += groupGap * 2 + 1;
        contentW = std::max(contentW, textWidth(measure, titleFont, g.title.toUpper()));
        height += titleH;
        for (const auto& r : g.rows) {
            contentW = std::max(contentW, textWidth(measure, labelFont, r.label) + gap + textWidth(measure, valueFont, r.value));
            height += rowH;
        }
    }
    height += pad - px(3);
    DeleteDC(measure);
    contentW = std::max(contentW, contentWidth_);
    contentWidth_ = contentW;
    const int W = contentW + 2 * pad, H = height;
    const int margin = px(16);
    if (client.right < W + margin || client.bottom < H / 3) {
        DeleteObject(titleFont); DeleteObject(labelFont); DeleteObject(valueFont);
        hide();
        return {};
    }

    void* bits = nullptr;
    HBITMAP bitmap = makeCanvas(W, H, &bits);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        DeleteObject(titleFont); DeleteObject(labelFont); DeleteObject(valueFont);
        return {};
    }
    std::vector<TextItem> texts;
    {
        using namespace Gdiplus;
        Bitmap canvas(W, H, W * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        GraphicsPath panel;
        roundedRect(panel, 0.5f, 0.5f, float(W) - 1.0f, float(H) - 1.0f, float(px(12)));
        SolidBrush ground(gdiColor(kSurface, kPanelAlpha));
        g.FillPath(&ground, &panel);
        Pen border(gdiColor(kLineStrong, 0xD0), 1.0f);
        g.DrawPath(&border, &panel);
        Pen separator(gdiColor(kLine, 0xE0), 1.0f);
        int y = pad;
        // Text baselines: the letter cell centred in each row
        const int titleBase = px(14), rowBase = px(14);
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const auto& grp = groups[gi];
            if (gi) {
                y += groupGap;
                g.DrawLine(&separator, float(pad), float(y) + 0.5f, float(W - pad), float(y) + 0.5f);
                y += 1 + groupGap;
            }
            // Group title: a small accent bar and the name in capitals (VEyebrow)
            SolidBrush bar(gdiColor(kAccent, 0xFF));
            g.FillRectangle(&bar, float(pad), float(y + px(4)), float(std::max(2, px(3))), float(px(11)));
            texts.push_back({grp.title.toUpper(), titleFont, pad + px(9), y + titleBase, false, kAccentText});
            y += titleH;
            for (const auto& r : grp.rows) {
                texts.push_back({r.label, labelFont, pad, y + rowBase, false, kMuted});
                texts.push_back({r.value, valueFont, W - pad, y + rowBase, true, r.tone ? r.tone : kText});
                y += rowH;
            }
        }
        g.Flush(FlushIntentionSync);
    }
    drawTexts(bits, W, H, texts);
    DeleteObject(titleFont); DeleteObject(labelFont); DeleteObject(valueFont);

    const int x = client.right - W - margin, y = margin;
    SetWindowPos(window_, HWND_TOP, x, y, W, H, SWP_NOACTIVATE);
    const bool ok = present(window_, bitmap, W, H);
    // Test runs: OVIORA_TEST_STATS_DUMP=<png> saves the drawn panel (premultiplied BGRA)
    static const QString dump = qEnvironmentVariable("OVIORA_TEST_STATS_DUMP");
    if (ok && !dump.isEmpty())
        QImage(static_cast<const uchar*>(bits), W, H, W * 4, QImage::Format_ARGB32_Premultiplied).copy().save(dump);
    DeleteObject(bitmap);
    if (!ok) { signature_.clear(); return {}; }
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    if (shown_.size() != QSize(W, H))
        oviora::log::info("stats-overlay", std::format("panel {}x{} at {},{} in video {}x{} dpi={} groups={}", W, H, x, y,
                                                       client.right, client.bottom, GetDpiForWindow(video), groups.size()));
    signature_ = signature;
    shown_ = QRect(x, y, W, H);
    return shown_;
}

// --- region drawing -------------------------------------------------------------------------------
namespace {
LRESULT CALLBACK drawProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<RegionDrawOverlay*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST: return HTCLIENT;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_PAINT: { PAINTSTRUCT ps{}; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_LBUTTONDOWN: case WM_MOUSEMOVE: case WM_LBUTTONUP: case WM_RBUTTONUP:
        if (self) self->mouse(msg, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_KEYDOWN:
        if (self) self->key(wp);
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}

RegionDrawOverlay::~RegionDrawOverlay()
{
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
}

bool RegionDrawOverlay::begin(HWND video, bool ellipse, const QString& hint, Callbacks callbacks)
{
    if (!video || !IsWindow(video)) return false;
    if (window_ && parent_ != video) { DestroyWindow(window_); window_ = nullptr; }
    if (!window_) {
        ensureGdiplus();
        WNDCLASSW wc{};
        wc.lpfnWndProc = drawProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"OvioraRegionDraw";
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        RegisterClassW(&wc);
        window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_CHILD, 0, 0, 1, 1, video, nullptr,
                                  wc.hInstance, nullptr);
        parent_ = video;
        if (!window_) {
            oviora::log::warn("ui-protection", std::format("drawing window refused err={}", GetLastError()));
            return false;
        }
        SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    ellipse_ = ellipse;
    hint_ = hint;
    callbacks_ = std::move(callbacks);
    dragging_ = false;
    RECT client{};
    GetClientRect(video, &client);
    SetWindowPos(window_, HWND_TOP, 0, 0, client.right, client.bottom, SWP_NOACTIVATE);
    redraw();
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    // Esc reaches this window (the Qt window would take it otherwise)
    SetFocus(window_);
    oviora::log::info("ui-protection", std::format("drawing window over the picture {}x{} shape={}", client.right, client.bottom,
                                                   ellipse ? "ellipse" : "rectangle"));
    return true;
}

void RegionDrawOverlay::end()
{
    if (!window_) return;
    if (GetCapture() == window_) ReleaseCapture();
    const bool hadFocus = GetFocus() == window_;
    ShowWindow(window_, SW_HIDE);
    dragging_ = false;
    callbacks_ = {};
    if (hadFocus && parent_) SetFocus(GetAncestor(parent_, GA_ROOT));
}

void RegionDrawOverlay::redraw()
{
    if (!window_) return;
    RECT r{};
    GetClientRect(parent_, &r);
    const int W = r.right, H = r.bottom;
    if (W < 2 || H < 2) return;
    void* bits = nullptr;
    HBITMAP bitmap = makeCanvas(W, H, &bits);
    if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); return; }
    const double s = std::max(1.0, GetDpiForWindow(parent_) / 96.0);
    {
        using namespace Gdiplus;
        Bitmap canvas(W, H, W * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // The picture dimmed a little: every pixel has some alpha, so the whole picture takes the mouse
        SolidBrush dim(Color(0x46, 0, 0, 0));
        g.FillRectangle(&dim, 0, 0, W, H);
        if (dragging_) {
            const RectF box(float(std::min(from_.x, to_.x)), float(std::min(from_.y, to_.y)),
                            float(std::max<LONG>(1, std::abs(to_.x - from_.x))), float(std::max<LONG>(1, std::abs(to_.y - from_.y))));
            SolidBrush fill(gdiColor(kAccent, 0x38));
            Pen pen(gdiColor(kAccentText, 0xFF), float(std::max(2.0, 2.0 * s)));
            if (ellipse_) { g.FillEllipse(&fill, box); g.DrawEllipse(&pen, box); }
            else { g.FillRectangle(&fill, box); g.DrawRectangle(&pen, box); }
        }
        // The hint on a dark pill at the top
        FontFamily family(L"Segoe UI");
        Font font(&family, float(15 * s), FontStyleRegular, UnitPixel);
        const std::wstring hint = hint_.toStdWString();
        RectF measured;
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        g.MeasureString(hint.c_str(), int(hint.size()), &font, PointF(0, 0), &format, &measured);
        const float pw = measured.Width + float(36 * s), ph = measured.Height + float(16 * s);
        const float px = (W - pw) / 2, py = float(24 * s);
        GraphicsPath pill;
        roundedRect(pill, px, py, pw, ph, ph / 2);
        SolidBrush ground(gdiColor(kSurface, 0xE6));
        g.FillPath(&ground, &pill);
        Pen edge(gdiColor(kLineStrong, 0xFF), 1.0f);
        g.DrawPath(&edge, &pill);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        SolidBrush white(gdiColor(kText, 0xFF));
        g.DrawString(hint.c_str(), int(hint.size()), &font, RectF(px, py + float(8 * s), pw, measured.Height), &format, &white);
        g.Flush(FlushIntentionSync);
    }
    present(window_, bitmap, W, H);
    static const QString dump = qEnvironmentVariable("OVIORA_TEST_DRAW_DUMP");
    if (!dump.isEmpty() && dragging_)
        QImage(static_cast<const uchar*>(bits), W, H, W * 4, QImage::Format_ARGB32_Premultiplied).copy().save(dump);
    DeleteObject(bitmap);
}

void RegionDrawOverlay::mouse(UINT msg, int x, int y)
{
    RECT r{};
    GetClientRect(window_, &r);
    if (r.right < 1 || r.bottom < 1) return;
    x = std::clamp(x, 0, int(r.right));
    y = std::clamp(y, 0, int(r.bottom));
    const double w = r.right, h = r.bottom;
    if (msg == WM_RBUTTONUP) {
        if (callbacks_.cancelled) { auto cancelled = callbacks_.cancelled; cancelled(); }
        return;
    }
    if (msg == WM_LBUTTONDOWN) {
        dragging_ = true;
        from_ = to_ = POINT{x, y};
        SetCapture(window_);
        redraw();
        return;
    }
    if (!dragging_) return;
    to_ = POINT{x, y};
    if (msg == WM_MOUSEMOVE) {
        redraw();
        if (callbacks_.moved) callbacks_.moved(from_.x / w, from_.y / h, to_.x / w, to_.y / h);
        return;
    }
    if (msg == WM_LBUTTONUP) {
        redraw();
        dragging_ = false;
        if (GetCapture() == window_) ReleaseCapture();
        if (callbacks_.finished) { auto finished = callbacks_.finished; finished(from_.x / w, from_.y / h, to_.x / w, to_.y / h); }
    }
}

void RegionDrawOverlay::key(WPARAM vk)
{
    if (vk == VK_ESCAPE && callbacks_.cancelled) { auto cancelled = callbacks_.cancelled; cancelled(); }
}

void RegionDrawOverlay::simulateDrag(double x0, double y0, double x1, double y1)
{
    if (!window_) return;
    RECT r{};
    GetClientRect(window_, &r);
    auto at = [&](double fx, double fy) {
        return MAKELPARAM(int(std::lround(std::clamp(fx, 0.0, 1.0) * r.right)), int(std::lround(std::clamp(fy, 0.0, 1.0) * r.bottom)));
    };
    PostMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, at(x0, y0));
    for (int n = 1; n <= 8; ++n) {
        const double t = n / 8.0;
        PostMessageW(window_, WM_MOUSEMOVE, MK_LBUTTON, at(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t));
    }
    PostMessageW(window_, WM_LBUTTONUP, 0, at(x1, y1));
}

void RegionDrawOverlay::simulateEscape()
{
    if (window_) PostMessageW(window_, WM_KEYDOWN, VK_ESCAPE, 0);
}

} // namespace oviora::ui
