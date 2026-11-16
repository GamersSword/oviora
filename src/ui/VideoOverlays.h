#pragma once
// Oviora: native layered windows over the PS5 picture (children of the D3D12 video window, which a
// Qt Quick item cannot be drawn over with transparency):
//  - the stream statistics panel (Ses ve ağ → İstatistikler, the stream menu's tile), half
//    transparent so the picture shows through, ClearType text like the subtitle overlay;
//  - the region drawing of "Tüm efektleri hariç tut" / NR protection: a see-through window
//    over the whole picture that takes the mouse drag and shows the shape outline.
#include <windows.h>

#include <QString>
#include <QRect>
#include <QRgb>

#include <functional>
#include <vector>

namespace oviora::ui {

struct StatsRow {
    QString label;
    QString value;
    QRgb tone = 0;          // 0: the default value colour
};
struct StatsGroup {
    QString title;
    std::vector<StatsRow> rows;
};

class StatsOverlay {
public:
    StatsOverlay() = default;
    ~StatsOverlay();
    StatsOverlay(const StatsOverlay&) = delete;
    StatsOverlay& operator=(const StatsOverlay&) = delete;
    // Draws the panel in the top-right corner of the video window. Returns the panel's rect in
    // the video window's client pixels (empty when nothing is shown).
    QRect update(HWND video, const std::vector<StatsGroup>& groups);
    // Keeps the drawn panel in the corner when the video window changes size (called every UI tick;
    // only moves it, no redraw). Returns the panel's rect, as update().
    QRect follow(HWND video);
    void hide();
    HWND window() const { return window_; }

private:
    HWND window_ = nullptr;
    HWND parent_ = nullptr;
    QString signature_;
    QRect shown_;
    int contentWidth_ = 0;     // grows only while shown: numbers changing do not make the panel jitter
};

// Region drawing on the picture. Coordinates given to the callbacks are 0..1 of the video
// window's client area (what QmlPlayerBridge::commitProtectionDraw takes).
class RegionDrawOverlay {
public:
    struct Callbacks {
        std::function<void(double, double, double, double)> moved;      // drag in progress
        std::function<void(double, double, double, double)> finished;   // button released
        std::function<void()> cancelled;                                 // Esc / right click
    };
    RegionDrawOverlay() = default;
    ~RegionDrawOverlay();
    RegionDrawOverlay(const RegionDrawOverlay&) = delete;
    RegionDrawOverlay& operator=(const RegionDrawOverlay&) = delete;
    // Covers the video window and starts taking the mouse. hint: the line shown at the top.
    bool begin(HWND video, bool ellipse, const QString& hint, Callbacks callbacks);
    void end();
    bool active() const { return window_ && IsWindowVisible(window_); }
    HWND window() const { return window_; }
    // Test runs: the same messages a real drag sends, posted to this app's own window only.
    void simulateDrag(double x0, double y0, double x1, double y1);
    void simulateEscape();

    // Window procedure state (public for the static procedure)
    void redraw();
    void mouse(UINT msg, int x, int y);
    void key(WPARAM vk);

private:
    HWND window_ = nullptr;
    HWND parent_ = nullptr;
    bool ellipse_ = false;
    QString hint_;
    Callbacks callbacks_;
    bool dragging_ = false;
    POINT from_{}, to_{};
};

} // namespace oviora::ui
