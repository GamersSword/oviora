#include "TranslationOverlay.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <objidl.h>
// Built with NOMINMAX: GDI+ headers use unqualified min/max
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include <QImage>
#include <QStringList>

#include "oviora/Log.h"

#include <format>

#pragma comment(lib, "gdiplus.lib")

namespace oviora::subtitle {
namespace {

LRESULT CALLBACK overlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

std::wstring wide(const QString &s) { return s.toStdWString(); }

Gdiplus::RectF rf(const QRectF &r) { return Gdiplus::RectF(float(r.x()), float(r.y()), float(r.width()), float(r.height())); }

// Whole device pixels: edges and glyph origins between pixels are spread over two of them and
// read as soft (the overlay is drawn 1:1 into the video window's physical pixels)
QRectF snapped(const QRectF &r)
{
    const double l = std::round(r.left()), t = std::round(r.top());
    return QRectF(l, t, std::max(1.0, std::round(r.right()) - l), std::max(1.0, std::round(r.bottom()) - t));
}

void roundedRect(Gdiplus::GraphicsPath &path, const QRectF &r, double radius)
{
    const float x = float(r.x()), y = float(r.y()), w = float(r.width()), h = float(r.height());
    const float d = float(std::min({radius * 2, r.width(), r.height()}));
    if (d < 1) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

double measure(Gdiplus::Graphics &g, const Gdiplus::Font &font, const QString &text)
{
    const std::wstring w = wide(text);
    Gdiplus::RectF out;
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    g.MeasureString(w.c_str(), int(w.size()), &font, Gdiplus::PointF(0, 0), &format, &out);
    return out.Width;
}

} // namespace

Overlay::Overlay(HWND videoWindow) : parent(videoWindow)
{
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr);
}

Overlay::~Overlay()
{
    if (window)
        DestroyWindow(window);
    if (bitmap)
        DeleteObject(bitmap);
    if (gdiplusToken)
        Gdiplus::GdiplusShutdown(gdiplusToken);
}

void Overlay::hide()
{
    if (window && IsWindowVisible(window))
        ShowWindow(window, SW_HIDE);
    lastSignature.clear();
}

bool Overlay::update(const OverlayState &s, const engine::PreviewView &view, double imageW, double imageH)
{
    if (!parent || !IsWindow(parent))
        return false;
    const bool want = !s.text.isEmpty() || s.showRegion || s.selecting;
    if (!want) {
        hide();
        return false;
    }
    if (!window) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = overlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"OvioraTranslatedSubtitle";
        RegisterClassW(&wc);
        window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_CHILD,
                                 0, 0, 1, 1, parent, nullptr, wc.hInstance, nullptr);
        if (!window)
            return false;
    }
    RECT client{};
    GetClientRect(parent, &client);
    const int W = client.right, H = client.bottom;
    if (W < 8 || H < 8) {
        hide();
        return false;
    }
    if (imageW <= 0 || imageH <= 0) {
        imageW = 1920;
        imageH = 1080;
    }

    QString signature = QStringLiteral("%1x%2|%3x%4|%5,%6,%7,%8,%9|").arg(W).arg(H).arg(imageW).arg(imageH)
                            .arg(view.zoom).arg(view.centerX).arg(view.centerY).arg(view.mode).arg(view.quarterTurns());
    signature += s.text + QLatin1Char('|') + s.source + QLatin1Char('|');
    for (const QRectF &r : {s.region, s.textBox, s.drag})
        signature += QStringLiteral("%1,%2,%3,%4;").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
    for (const QVariant &v : s.rows) {
        const QRectF r = v.toRectF();
        signature += QStringLiteral("%1,%2,%3,%4;").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
    }
    signature += QStringLiteral("%1|%2|%3|%4%5%6%7").arg(s.lines).arg(s.fontRel).arg(s.boxOpacity)
                     .arg(s.coverOriginal).arg(s.showSource).arg(s.showRegion).arg(s.selecting);
    if (signature == lastSignature) {
        if (!IsWindowVisible(window))
            ShowWindow(window, SW_SHOWNOACTIVATE);
        return true;
    }

    RECT current{};
    GetClientRect(window, &current);
    if (current.right != W || current.bottom != H)
        SetWindowPos(window, HWND_TOP, 0, 0, W, H, SWP_NOACTIVATE);

    // Where the picture sits in the window (PreviewView: the presenter's own mapping)
    const auto [rw, rh] = view.renderedSize(float(W), float(H), float(imageW), float(imageH));
    auto toView = [&, rw = rw, rh = rh](const QRectF &r) {
        auto point = [&](double u, double v) {
            const auto [du, dv] = view.displayUv(float(u), float(v));
            return QPointF(W * 0.5 + (du - view.centerX) * rw, H * 0.5 + (dv - view.centerY) * rh);
        };
        return QRectF(point(r.left(), r.top()), point(r.right(), r.bottom())).normalized();
    };
    const QRectF video = toView(QRectF(0, 0, 1, 1));
    const QRectF regionRect = toView(s.region);
    const bool hasBox = s.textBox.width() > 0 && s.textBox.height() > 0;
    const QRectF anchor = hasBox ? toView(s.textBox) : regionRect;
    // Line box height (ascender to descender) * 0.85 matched GTA V's subtitle size in a live test
    double fontPx = hasBox ? anchor.height() / std::max(1, s.lines) * 0.85 : video.height() * 0.024;
    fontPx = std::clamp(fontPx, video.height() * 0.015, std::max(video.height() * 0.015, video.height() * 0.032));
    // A whole pixel size: hinting fits the outlines to the pixel grid best at those
    fontPx = std::round(std::max(10.0, fontPx * s.fontRel));
    const double pad = fontPx * 0.35;

    if (!bitmap || bitmapW != W || bitmapH != H) {
        if (bitmap)
            DeleteObject(bitmap);
        bitmap = nullptr;
        bits = nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = W;
        info.bmiHeader.biHeight = -H;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!bitmap || !bits) {
            if (bitmap)
                DeleteObject(bitmap);
            bitmap = nullptr;
            return false;
        }
        bitmapW = W;
        bitmapH = H;
    }
    memset(bits, 0, size_t(W) * size_t(H) * 4);
    {
        using namespace Gdiplus;
        Bitmap canvas(W, H, W * 4, PixelFormat32bppPARGB, static_cast<BYTE *>(bits));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // Whole-pixel coordinates are pixel edges, so snapped boxes have hard edges
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        FontFamily family(L"Segoe UI");
        const BYTE boxAlpha = BYTE(std::clamp(int(std::lround(s.boxOpacity * 2.55)), 0, 255));
        SolidBrush boxBrush(Color(boxAlpha, 0, 0, 0));

        // While a box is drawn over the subtitle (Ctrl+B): the picture dimmed, the old region dashed
        if (s.selecting) {
            SolidBrush dim(Color(0x6e, 0, 0, 0));
            g.FillRectangle(&dim, 0, 0, W, H);
            if (s.drag.isValid() && s.drag.width() > 0) {
                const QRectF d = toView(s.drag);
                SolidBrush light(Color(0x20, 255, 255, 255));
                g.FillRectangle(&light, rf(d));
                Pen yellow(Color(255, 255, 210, 0), 2);
                g.DrawRectangle(&yellow, rf(d));
            } else {
                Pen yellow(Color(0xa0, 255, 210, 0), 1);
                g.DrawRectangle(&yellow, rf(regionRect));
            }
            const float hintPx = float(std::max(14.0, video.height() * 0.028));
            Font hintFont(&family, hintPx, FontStyleRegular, UnitPixel);
            StringFormat center;
            center.SetAlignment(StringAlignmentCenter);
            const std::wstring hint = wide(s.selectHint);
            SolidBrush white(Color(255, 255, 255, 255));
            SolidBrush shade(Color(200, 0, 0, 0));
            const RectF hintRect(0, float(video.y() + video.height() * 0.08), float(W), hintPx * 3.2f);
            RectF shadowRect = hintRect;
            shadowRect.Offset(1.5f, 1.5f);
            g.DrawString(hint.c_str(), int(hint.size()), &hintFont, shadowRect, &center, &shade);
            g.DrawString(hint.c_str(), int(hint.size()), &hintFont, hintRect, &center, &white);
        } else if (s.showRegion) {
            // The configured region while editing it (Ctrl+R)
            Pen yellow(Color(255, 255, 210, 0), 2);
            g.DrawRectangle(&yellow, rf(regionRect));
        }

        if (!s.text.isEmpty() && !s.selecting) {
            Font font(&family, float(fontPx), FontStyleRegular, UnitPixel);
            // Covers over the original rows (one tight box per English row)
            if (s.coverOriginal) {
                QList<QRectF> covers;
                for (const QVariant &v : s.rows)
                    covers << v.toRectF();
                if (covers.isEmpty() && hasBox)
                    covers << s.textBox;
                for (const QRectF &c : covers) {
                    const QRectF r = toView(c);
                    GraphicsPath path;
                    roundedRect(path, snapped(QRectF(r.x() - pad, r.y() - pad * 0.5, r.width() + 2 * pad, r.height() + pad)), pad * 0.5);
                    g.FillPath(&boxBrush, &path);
                }
            }
            // On a (nearly) opaque box the Turkish is drawn by GDI with ClearType, the way Windows
            // draws its own text: hinted, on whole pixels, sub-pixel sharp. GDI+'s grayscale
            // anti-aliasing (what the overlay used) left the letters visibly soft next to the
            // system's text. A see-through box keeps GDI+ with the drop shadow.
            const bool clearType = boxAlpha >= 200;
            HDC textDc = nullptr;
            HFONT gdiFont = nullptr;
            HGDIOBJ oldFont = nullptr;
            if (clearType) {
                textDc = CreateCompatibleDC(nullptr);
                gdiFont = CreateFontW(-int(fontPx), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                oldFont = SelectObject(textDc, gdiFont);
            }
            auto textWidth = [&](const QString &t) {
                if (!clearType)
                    return measure(g, font, t);
                const std::wstring w = wide(t);
                SIZE size{};
                GetTextExtentPoint32W(textDc, w.c_str(), int(w.size()), &size);
                return double(size.cx);
            };
            // Greedy word wrap so every Turkish line gets its own background
            const double wrapWidth = std::max(anchor.width(), video.width() * 0.35);
            QStringList lines;
            QString cur;
            for (const QString &w : s.text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                const QString t = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
                if (cur.isEmpty() || textWidth(t) <= wrapWidth)
                    cur = t;
                else {
                    lines << cur;
                    cur = w;
                }
            }
            if (!cur.isEmpty())
                lines << cur;
            const double lineH = std::round(fontPx * 1.3);
            const double blockH = lines.size() * lineH;
            // Centred on the original; a taller translation grows upwards from the original's bottom
            const double blockTop = std::round(blockH <= anchor.height() + 2 * pad ? anchor.y() + anchor.height() / 2 - blockH / 2
                                                                                   : anchor.y() + anchor.height() + pad - blockH);
            StringFormat plain(StringFormat::GenericTypographic());
            plain.SetFormatFlags(plain.GetFormatFlags() | StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
            // The letter cell (ascent + descent) centred in the line box, the baseline on a whole pixel
            const double em = family.GetEmHeight(FontStyleRegular);
            const double ascentPx = fontPx * family.GetCellAscent(FontStyleRegular) / em;
            const double cellPx = ascentPx + fontPx * family.GetCellDescent(FontStyleRegular) / em;
            SolidBrush white(Color(255, 255, 255, 255));
            SolidBrush raised(Color(0xc8, 0, 0, 0));
            // A whole-pixel drop shadow: a fractional one smeared the lower edge of every letter
            const float shadow = float(std::max(1.0, std::round(fontPx / 24.0)));
            struct Placed { QRectF box; double x, baseline; };
            QList<Placed> placed;
            for (int i = 0; i < lines.size(); i++) {
                const double tw = std::ceil(textWidth(lines.at(i)));
                const double boxW = std::ceil(tw + 2 * pad);
                const QRectF box = snapped(QRectF(anchor.center().x() - boxW / 2, blockTop + i * lineH, boxW, lineH));
                GraphicsPath path;
                roundedRect(path, box, pad * 0.5);
                g.FillPath(&boxBrush, &path);
                const double baseline = std::round(box.y() + (box.height() - cellPx) / 2 + ascentPx);
                const double x = box.x() + std::round((box.width() - tw) / 2);
                placed << Placed { box, x, baseline };
                if (clearType)
                    continue;
                const std::wstring line = wide(lines.at(i));
                const PointF at(float(x), float(baseline - ascentPx));
                g.DrawString(line.c_str(), int(line.size()), &font, PointF(at.X, at.Y + shadow), &plain, &raised);
                g.DrawString(line.c_str(), int(line.size()), &font, at, &plain, &white);
            }
            if (clearType) {
                // White on opaque black gives each colour channel its own coverage. Over the box:
                // colour = coverage (premultiplied white), alpha = box + (1 - box) * coverage
                g.Flush(FlushIntentionSync);
                SetTextColor(textDc, RGB(255, 255, 255));
                SetBkMode(textDc, TRANSPARENT);
                SetTextAlign(textDc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
                for (int i = 0; i < placed.size(); i++) {
                    const QRect box = placed.at(i).box.toRect().intersected(QRect(0, 0, W, H));
                    if (box.isEmpty())
                        continue;
                    BITMAPINFO lineInfo{};
                    lineInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    lineInfo.bmiHeader.biWidth = box.width();
                    lineInfo.bmiHeader.biHeight = -box.height();
                    lineInfo.bmiHeader.biPlanes = 1;
                    lineInfo.bmiHeader.biBitCount = 32;
                    lineInfo.bmiHeader.biCompression = BI_RGB;
                    void *lineBits = nullptr;
                    HBITMAP lineBitmap = CreateDIBSection(textDc, &lineInfo, DIB_RGB_COLORS, &lineBits, nullptr, 0);
                    if (!lineBitmap || !lineBits) {
                        if (lineBitmap)
                            DeleteObject(lineBitmap);
                        continue;
                    }
                    memset(lineBits, 0, size_t(box.width()) * box.height() * 4);
                    HGDIOBJ oldBitmap = SelectObject(textDc, lineBitmap);
                    const std::wstring line = wide(lines.at(i));
                    TextOutW(textDc, int(placed.at(i).x) - box.x(), int(placed.at(i).baseline) - box.y(), line.c_str(), int(line.size()));
                    GdiFlush();
                    for (int y = 0; y < box.height(); y++) {
                        const uint8_t *src = static_cast<const uint8_t *>(lineBits) + size_t(y) * box.width() * 4;
                        uint8_t *dst = static_cast<uint8_t *>(bits) + (size_t(box.y() + y) * W + box.x()) * 4;
                        for (int x = 0; x < box.width(); x++) {
                            const uint8_t cb = src[4 * x], cg = src[4 * x + 1], cr = src[4 * x + 2];
                            const int cover = std::max({ cb, cg, cr });
                            if (!cover)
                                continue;
                            uint8_t *d = dst + 4 * x;
                            d[0] = std::max(d[0], cb);
                            d[1] = std::max(d[1], cg);
                            d[2] = std::max(d[2], cr);
                            d[3] = uint8_t(d[3] + (255 - d[3]) * cover / 255);
                        }
                    }
                    SelectObject(textDc, oldBitmap);
                    DeleteObject(lineBitmap);
                }
            }
            if (textDc) {
                SelectObject(textDc, oldFont);
                DeleteObject(gdiFont);
                DeleteDC(textDc);
            }
            // The English above it, small (Görünüm → İngilizcesini de göster)
            if (s.showSource && !s.source.isEmpty()) {
                const double srcPx = fontPx * 0.55;
                Font srcFont(&family, float(srcPx), FontStyleRegular, UnitPixel);
                StringFormat wrap;
                wrap.SetAlignment(StringAlignmentCenter);
                wrap.SetLineAlignment(StringAlignmentFar);
                const std::wstring src = wide(s.source);
                RectF measured;
                g.MeasureString(src.c_str(), int(src.size()), &srcFont, RectF(0, 0, float(anchor.width()), 10000.f), &wrap, &measured);
                const RectF where(float(anchor.x()), float(blockTop - 4 - measured.Height), float(anchor.width()), measured.Height);
                GraphicsPath textPath;
                textPath.AddString(src.c_str(), int(src.size()), &family, FontStyleRegular, float(srcPx), where, &wrap);
                Pen outline(Color(255, 0, 0, 0), float(std::max(1.5, srcPx / 7.0)));
                outline.SetLineJoin(LineJoinRound);
                g.DrawPath(&outline, &textPath);
                SolidBrush grey(Color(255, 0xdc, 0xdc, 0xdc));
                g.FillPath(&grey, &textPath);
            }
        }
        g.Flush(FlushIntentionSync);
    }
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
    if (!ok) {
        lastSignature.clear();
        return false;
    }
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    lastSignature = signature;
    if (!s.text.isEmpty()) {
        // Once per size: the bitmap is the window's own physical pixels (no stretching by the system)
        const QString sizeKey = QStringLiteral("%1x%2").arg(W).arg(H);
        if (sizeKey != loggedSize) {
            loggedSize = sizeKey;
            RECT wr{};
            GetWindowRect(window, &wr);
            const auto awareness = GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(window));
            oviora::log::info("altyazi", std::format("overlay bitmap {}x{} window {}x{} video client {}x{} dpi={} awareness={} font={:.1f}px",
                W, H, wr.right - wr.left, wr.bottom - wr.top, client.right, client.bottom, GetDpiForWindow(window), int(awareness), fontPx));
        }
        // Test runs: OVIORA_TEST_OVERLAY_DUMP=<png> saves the drawn overlay (premultiplied BGRA as drawn)
        static const QString dump = qEnvironmentVariable("OVIORA_TEST_OVERLAY_DUMP");
        if (!dump.isEmpty()) {
            QImage(static_cast<const uchar *>(bits), W, H, W * 4, QImage::Format_ARGB32_Premultiplied).save(dump);
        }
    }
    return true;
}

} // namespace oviora::subtitle
