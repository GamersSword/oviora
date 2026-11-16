// Oviora: draws the translated subtitle over the PS5 picture, as the old Oviora's StreamView.qml
// did: one tight dark cover per original English row, one tight background per Turkish line,
// centred on the original. A layered child of the native video window (like the file subtitles,
// apps/oviora/ui/SubtitleOverlay.cpp): the D3D12 picture is never read or touched, menus cut out
// of the video window cut out of this one too.
#pragma once

#include <QRectF>
#include <QString>
#include <QVariantList>

#include <windows.h>

#include "oviora/engine/PreviewView.h"

namespace oviora::subtitle {

struct OverlayState
{
    QString text;            // Turkish, empty = none
    QString source;          // the English (showSource)
    QRectF region;           // normalized frame rect that is read
    QRectF textBox;          // normalized frame rect of the original subtitle (invalid: none)
    int lines = 0;
    QVariantList rows;       // QRectF per original row
    double fontRel = 1.0;
    int boxOpacity = 92;
    bool coverOriginal = true;
    bool showSource = false;
    bool showRegion = false;
    bool selecting = false;
    QRectF drag;             // selection being drawn (normalized frame), invalid when none
    QString selectHint;      // the two-line hint while selecting
};

class Overlay
{
public:
    explicit Overlay(HWND videoWindow);
    ~Overlay();
    // Draws (or hides) for the current window size and picture placement. Returns whether it shows.
    bool update(const OverlayState &state, const engine::PreviewView &view, double imageW, double imageH);
    void hide();

private:
    HWND parent = nullptr;
    HWND window = nullptr;
    ULONG_PTR gdiplusToken = 0;
    QString lastSignature;
    QString loggedSize;
    HBITMAP bitmap = nullptr;
    void *bits = nullptr;
    int bitmapW = 0, bitmapH = 0;
};

} // namespace oviora::subtitle
