// QML bridge: View responsibilities.
#include "../QmlPlayerBridge_p.h"

namespace oviora::ui {
// --- NR protection regions drawn on the picture (#8) ------------------------
namespace {
// Outline of the region being drawn, on a layered child of the video window
// (the swapchain is never read or touched), as 1.4.4's ProtectionOverlay.
void drawProtectionOutline(HWND overlay, HWND video, RECT box, bool ellipse) {
    RECT r{}; GetClientRect(video, &r);
    if (r.right < 1 || r.bottom < 1) return;
    SetWindowPos(overlay, HWND_TOP, 0, 0, r.right, r.bottom, SWP_NOACTIVATE);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = r.right;
    info.bmiHeader.biHeight = -r.bottom; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen); void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen); return; }
    auto previous = SelectObject(memory, bitmap);
    memset(bits, 0, size_t(r.right) * size_t(r.bottom) * 4);
    {
        Gdiplus::Bitmap canvas(r.right, r.bottom, r.right * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Gdiplus::Color(255, 255, 138, 61), 2.0f);
        Gdiplus::SolidBrush fill(Gdiplus::Color(40, 255, 138, 61));
        const Gdiplus::Rect rect(box.left, box.top, std::max(1L, box.right - box.left), std::max(1L, box.bottom - box.top));
        if (ellipse) { g.FillEllipse(&fill, rect); g.DrawEllipse(&pen, rect); }
        else { g.FillRectangle(&fill, rect); g.DrawRectangle(&pen, rect); }
    }
    SIZE size{r.right, r.bottom}; POINT origin{}; BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (UpdateLayeredWindow(overlay, screen, nullptr, &size, memory, &origin, 0, &blend, ULW_ALPHA))
        ShowWindow(overlay, SW_SHOWNOACTIVATE);
    SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
}
}

QVariantMap QmlPlayerBridge::maskState(bool allEffects) const {
    const auto* node=impl_->chain.firstOf(allEffects?engine::EffectType::Exclusion:engine::EffectType::Protection);
    const auto value=node?node->protection:engine::ProtectionSettings{};
    QVariantList regions;
    for(int index=0;index<int(value.regions.size());++index){
        const auto& q=value.regions[size_t(index)];
        if(!q.empty())regions<<QVariantMap{{"index",index},{"left",q.left},{"top",q.top},
            {"right",q.right},{"bottom",q.bottom},{"ellipse",q.ellipse}};
    }
    const auto& first=value.regions[0];
    return {{"enabled",node&&node->enabled},{"feather",value.featherPixels},{"regions",regions},
        {"left",first.left},{"top",first.top},{"right",first.right},{"bottom",first.bottom},
        {"drawShape",impl_->protExclusion==allEffects?impl_->protDraw:QString{}}};
}
QVariantMap QmlPlayerBridge::exclusionState() const {return maskState(true);}
QVariantList QmlPlayerBridge::protectionRegions() const {return maskState(false).value("regions").toList();}
QString QmlPlayerBridge::protectionDrawShape() const { return impl_->protDraw; }

bool QmlPlayerBridge::editProtection(const std::function<bool(engine::ChainNode&)>& edit,bool allEffects) {
    const auto type=allEffects?engine::EffectType::Exclusion:engine::EffectType::Protection;
    if(allEffects&&!impl_->chain.firstOf(type)&&addEffect(QStringLiteral("exclusion"))<0)return false;
    for (uint32_t i = 0; i < impl_->chain.nodeCount; ++i) {
        auto& node = impl_->chain.nodes[i];
        if (node.type != type) continue;
        const auto before = node; const bool wasValid = chainValid();
        if (!edit(node)) return false;
        if (node == before) return true;
        const bool accepted = impl_->revalidate(true);
        if (!accepted) { const auto error = impl_->validation.message; node = before; impl_->revalidate(); emit notice(uiText(error), true); }
        emit settingsChanged();
        if(before.enabled!=node.enabled)emitChainChanged();
        else emitParameterChanged(&QmlPlayerBridge::protectionChanged,wasValid);
        return accepted;
    }
    emit notice(tr("处理链中没有 NR 保护节点，未更改设置"), true);
    return false;
}

bool QmlPlayerBridge::beginProtectionDraw(const QString& shape) {return beginMaskDraw(false,shape);}
bool QmlPlayerBridge::beginMaskDraw(bool allEffects,const QString& shape) {
    auto& i = *impl_;
    if (shape.isEmpty()) {
        if (i.protOverlay) ShowWindow(i.protOverlay, SW_HIDE);
        if (i.drawOverlay) i.drawOverlay->end();
        if (!i.protDraw.isEmpty()) { i.protDraw.clear(); emit protectionDrawChanged();emit protectionChanged(); }
        return true;
    }
    if (shape != QLatin1String("rect") && shape != QLatin1String("ellipse")) return false;
    if (!i.snapshot.running || !i.videoWindow) { emit notice(tr("请先打开视频，再在画面上绘制保护区域"), true); return false; }
    bool space = false;
    const auto* node=i.chain.firstOf(allEffects?engine::EffectType::Exclusion:engine::EffectType::Protection);
    const auto parameters=node?node->protection:engine::ProtectionSettings{};
    for(const auto& q:parameters.regions)space|=q.empty();
    if (!space) { emit notice(tr("最多 4 个保护区域，请先删除一个"), true); return false; }
    // The drag is taken by a see-through window over the picture (the stream menu closes while
    // the user draws; Main.qml opens it again on the same tab when the drawing ends).
    if (!i.drawOverlay) i.drawOverlay = std::make_unique<RegionDrawOverlay>();
    const bool ellipse = shape == QLatin1String("ellipse");
    RegionDrawOverlay::Callbacks callbacks;
    callbacks.finished = [this](double x0, double y0, double x1, double y1) {
        if (!commitProtectionDraw(x0, y0, x1, y1)) oviora::log::info("ui-protection", "drawing ended without a region (too small or refused)");
    };
    callbacks.cancelled = [this] {
        oviora::log::info("ui-protection", "drawing cancelled");
        beginProtectionDraw(QString());
    };
    if (!i.drawOverlay->begin(i.videoWindow, ellipse,
            ellipse ? tr("Görüntünün üzerinde sürükleyerek bir daire çizin · Esc: vazgeç")
                    : tr("Görüntünün üzerinde sürükleyerek bir dikdörtgen çizin · Esc: vazgeç"), std::move(callbacks))) {
        emit notice(tr("Çizim penceresi açılamadı"), true);
        return false;
    }
    i.protDraw = shape;i.protExclusion=allEffects;
    emit protectionDrawChanged();emit protectionChanged();
    return true;
}
void QmlPlayerBridge::testMaskDrag(double x0, double y0, double x1, double y1) {
    if (!impl_->drawOverlay || !impl_->drawOverlay->active()) { oviora::log::warn("ui-protection", "test drag: no drawing in progress"); return; }
    oviora::log::info("ui-protection", std::format("test drag {:.2f},{:.2f} -> {:.2f},{:.2f} (messages to this app's own drawing window)", x0, y0, x1, y1));
    impl_->drawOverlay->simulateDrag(x0, y0, x1, y1);
}
void QmlPlayerBridge::testMaskEscape() {
    if (!impl_->drawOverlay || !impl_->drawOverlay->active()) { oviora::log::warn("ui-protection", "test Esc: no drawing in progress"); return; }
    oviora::log::info("ui-protection", "test Esc (message to this app's own drawing window)");
    impl_->drawOverlay->simulateEscape();
}

void QmlPlayerBridge::updateProtectionDraw(double x0, double y0, double x1, double y1) {
    auto& i = *impl_;
    if (i.protDraw.isEmpty() || !i.videoWindow) return;
    if (!i.protOverlay) { ensureGdiplus(); i.protOverlay = createSubtitleOverlay(i.videoWindow); }
    if (!i.protOverlay) return;
    RECT client{}; GetClientRect(i.videoWindow, &client);
    const auto px = [&](double f, LONG extent) { return LONG(std::lround(std::clamp(f, 0.0, 1.0) * extent)); };
    const RECT box{px(std::min(x0, x1), client.right), px(std::min(y0, y1), client.bottom),
                   px(std::max(x0, x1), client.right), px(std::max(y0, y1), client.bottom)};
    drawProtectionOutline(i.protOverlay, i.videoWindow, box, i.protDraw == QLatin1String("ellipse"));
}

bool QmlPlayerBridge::commitProtectionDraw(double x0, double y0, double x1, double y1) {
    auto& i = *impl_;
    const bool allEffects=i.protExclusion;
    const bool ellipse = i.protDraw == QLatin1String("ellipse");
    const bool armed = !i.protDraw.isEmpty();
    beginProtectionDraw(QString());
    if (!armed || !i.videoWindow) return false;
    RECT client{}; GetClientRect(i.videoWindow, &client);
    if (client.right < 1 || client.bottom < 1) return false;
    const double w = client.right, h = client.bottom;
    // Too small to be intended (1.4.4: at least 3 px each way).
    if (std::abs(x1 - x0) * w < 3 || std::abs(y1 - y0) * h < 3) return false;
    // Host pixels -> source-normalized, through the current zoom/pan (1.4.4).
    const auto e = i.snapshot.metrics.resolution.output;
    const auto view = i.engine.previewView();
    const auto a = view.sourcePoint(float(x0 * w), float(y0 * h), float(w), float(h), float(e.width), float(e.height));
    const auto b = view.sourcePoint(float(x1 * w), float(y1 * h), float(w), float(h), float(e.width), float(e.height));
    engine::ProtectionRect q{std::clamp(std::min(a.first, b.first), 0.0f, 1.0f), std::clamp(std::min(a.second, b.second), 0.0f, 1.0f),
                             std::clamp(std::max(a.first, b.first), 0.0f, 1.0f), std::clamp(std::max(a.second, b.second), 0.0f, 1.0f), ellipse};
    if (q.empty()) return false;
    const bool ok = editProtection([&](engine::ChainNode& node) {
        for (auto& slot : node.protection.regions)
            if (slot.empty()) { slot = q; node.enabled = true; node.protection.enabled = true; return true; }
        emit notice(tr("最多 4 个保护区域，请先删除一个"), true);
        return false;
    },allEffects);
    oviora::log::info("ui-protection", std::format("{} accepted={} bounds={:.3f},{:.3f},{:.3f},{:.3f} allEffects={} source-normalized",
                                                  ellipse ? "ellipse" : "rectangle", ok, q.left, q.top, q.right, q.bottom,allEffects));
    return ok;
}

bool QmlPlayerBridge::setMaskEnabled(bool allEffects,bool enabled) {
    return editProtection([&](engine::ChainNode& node){node.enabled=enabled;node.protection.enabled=enabled;return true;},allEffects);
}
bool QmlPlayerBridge::setMaskFeather(bool allEffects,double pixels) {
    if(!std::isfinite(pixels)||pixels<0||pixels>64)return false;
    return editProtection([&](engine::ChainNode& node){node.protection.featherPixels=float(pixels);return true;},allEffects);
}
bool QmlPlayerBridge::clearMaskRegions(bool allEffects) {
    return editProtection([](engine::ChainNode& node){node.protection.regions={};return true;},allEffects);
}
bool QmlPlayerBridge::removeMaskRegion(bool allEffects,int index) {
    if(index<0||index>=int(engine::ProtectionSettings{}.regions.size()))return false;
    return editProtection([&](engine::ChainNode& node){
        auto& regions=node.protection.regions;
        for(size_t i=size_t(index);i+1<regions.size();++i)regions[i]=regions[i+1];
        regions.back()={};return true;
    },allEffects);
}

bool QmlPlayerBridge::removeProtectionRegion(int index) {
    if (index < 0 || index >= int(engine::ProtectionSettings{}.regions.size())) return false;
    return editProtection([&](engine::ChainNode& node) {
        // Keep the remaining regions packed in order.
        auto& regions = node.protection.regions;
        for (int k = index; k + 1 < int(regions.size()); ++k) regions[size_t(k)] = regions[size_t(k + 1)];
        regions.back() = {};
        return true;
    });
}
bool QmlPlayerBridge::clearProtectionRegions() {
    return editProtection([](engine::ChainNode& node) { node.protection.regions = {}; return true; });
}
bool QmlPlayerBridge::setProtectionFeather(double pixels) {
    if (!std::isfinite(pixels) || pixels < 0 || pixels > 64) return false;
    return editProtection([&](engine::ChainNode& node) { node.protection.featherPixels = float(pixels); return true; });
}


// --- design gap B8-B11: compare and aspect ------------------------------------
int QmlPlayerBridge::compareMode() const { return impl_->compareMode; }
void QmlPlayerBridge::setCompareMode(int value) {
    if (value < 0 || value > 2 || impl_->compareMode == value) return;
    impl_->compareMode = value;
    if (!impl_->holdKeyDown) impl_->engine.comparison(value, impl_->compareBase, float(impl_->compareSplit));
    oviora::log::info("ui-compare", std::format("mode={} base={} split={:.3f}", value, impl_->compareBase, impl_->compareSplit));
    // Comparison shows real frames only (a generated frame has no original to set beside
    // it), so frame generation pauses while it is on. A field user left split compare on
    // for 24 minutes and reported "补帧无效" (logs9, 2026-10-01): say so.
    if (value != 0 && impl_->snapshot.running && impl_->snapshot.applied.multiplier > 1)
        emit notice(tr("对比模式下补帧暂停（对比只能用真实帧），关闭对比后自动恢复"), false);
    emit compareChanged();
}
bool QmlPlayerBridge::compareBase() const { return impl_->compareBase; }
void QmlPlayerBridge::setCompareBase(bool value) {
    if (impl_->compareBase == value) return;
    impl_->compareBase = value;
    if (!impl_->holdKeyDown) impl_->engine.comparison(impl_->compareMode, value, float(impl_->compareSplit));
    emit compareChanged();
}
double QmlPlayerBridge::compareSplit() const { return impl_->compareSplit; }
void QmlPlayerBridge::setCompareSplitAt(double fx) {
    // 1.4.4 AppShell: the split is a fraction of the displayed picture, which is
    // letterboxed and may be zoomed/panned inside the host.
    auto& i = *impl_;
    if (!i.videoWindow || !std::isfinite(fx)) return;
    RECT r{}; GetClientRect(i.videoWindow, &r);
    if (r.right < 1 || r.bottom < 1) return;
    const auto e = i.snapshot.metrics.resolution.output;
    const auto view = i.engine.previewView();
    float contentWidth = float(r.right);
    if (e.width && e.height) contentWidth = view.renderedSize(float(r.right),float(r.bottom),float(e.width),float(e.height)).first;
    const float left = r.right * .5f - contentWidth * view.centerX;
    i.compareSplit = std::clamp((float(fx) * r.right - left) / std::max(1.0f, contentWidth), 0.0f, 1.0f);
    if (!i.holdKeyDown) i.engine.comparison(i.compareMode, i.compareBase, float(i.compareSplit));
    emit compareChanged();
}
int QmlPlayerBridge::aspectMode() const { return impl_->aspectMode; }
void QmlPlayerBridge::setAspectMode(int value) {
    if (value < 0 || value > 6 || impl_->aspectMode == value) return;
    impl_->aspectMode = value;
    impl_->prefs[QStringLiteral("aspect")] = value;
    if (!impl_->savePrefs()) oviora::log::warn("ui-prefs", "aspect mode not saved");
    applyAspect(true);
    emit compareChanged();
}
double QmlPlayerBridge::viewZoom() const { return impl_->viewZoom; }
void QmlPlayerBridge::setViewZoom(double value) {
    if (!std::isfinite(value)) return;
    const double clamped = std::clamp(value, 1.0, 5.0);
    if (std::abs(impl_->viewZoom - clamped) < 1e-6) return;
    impl_->viewZoom = clamped;
    impl_->prefs[QStringLiteral("viewZoom")] = clamped;
    if (!impl_->savePrefs()) oviora::log::warn("ui-prefs", "view zoom not saved");
    applyAspect(true);
    emit compareChanged();
}
void QmlPlayerBridge::applyAspect(bool force) {
    auto& i = *impl_;
    const int mode = i.aspectMode;
    if (!i.videoWindow) return;
    RECT r{}; GetClientRect(i.videoWindow, &r);
    const auto e = i.snapshot.metrics.resolution.output;
    const QSize client(r.right, r.bottom), image(int(e.width), int(e.height));
    const double dar=sourceAspect();
    if (!force && mode == i.aspectApplied && client == i.aspectClient && image == i.aspectImage && dar == i.aspectDar) return;
    i.aspectClient = client; i.aspectImage = image; i.aspectApplied = mode; i.aspectDar = dar;
    auto view=force?engine::PreviewView{}:i.engine.previewView();
    view.mode=mode;
    view.zoom=mode==2?float(i.viewZoom):1.0f;
    view.displayAspect=dar>0&&image.height()>0&&std::abs(dar-double(image.width())/image.height())>.001?dar:0;
    i.engine.previewView(view);
    oviora::log::info("ui-aspect", std::format("mode={} zoom={:.3f} dar={:.4f} client={}x{} image={}x{}",mode,view.zoom,dar,
        client.width(),client.height(),image.width(),image.height()));
}

// --- design gap D18 / C15 / E20 / F21 / F24 ------------------------------------
double QmlPlayerBridge::audioSkewMs() const {
    const auto& skew = impl_->snapshot.captureAudio.skewMs;
    return audioSkewKnown() ? *skew : 0.0;
}
bool QmlPlayerBridge::audioSkewKnown() const {
    const auto& skew = impl_->snapshot.captureAudio.skewMs;
    return audioSyncLive() && skew.has_value() && std::isfinite(*skew) && std::abs(*skew) < 5000.0;
}

// The live source in use, for the page headers (sourceKind / sourceTitle).
void QmlPlayerBridge::rememberSource(const QString& kind, const QString& label) {
    impl_->liveKind = kind;
    impl_->liveLabel = label;
}

bool QmlPlayerBridge::resetNrLayer(int index) {
    auto& c = impl_->chain;
    if (c.mode != engine::ChainMode::List || index < 0 || index >= int(c.nodeCount) ||
        c.nodes[size_t(index)].type != engine::EffectType::NrEnhance) return false;
    auto& node = c.nodes[size_t(index)];
    const auto before = node;
    const engine::NrLayerSettings defaults;
    // The model and residual parameters, the size and the temporal option go
    // back to their defaults; the shared runtime and the on/off state stay.
    node.nr.model = defaults.model; node.nr.residual = defaults.residual;
    node.nr.temporal = defaults.temporal; node.nr.sizePolicy = defaults.sizePolicy;
    if (node == before) return true;
    if (!impl_->revalidate(true)) {
        const auto error = impl_->validation.message; node = before; impl_->revalidate();
        emit notice(uiText(error), true); return false;
    }
    emit settingsChanged(); emitChainChanged();
    return true;
}

QVariantList QmlPlayerBridge::frameTimes() const {
    QVariantList out;
    for (const double v : impl_->frameTimes) out << v;
    return out;
}


// Forced SDR display of an HDR stream: a display setting the engine carries.
bool QmlPlayerBridge::captureForceSdr() const { return settings().forceSdrPreview; }
void QmlPlayerBridge::setCaptureForceSdr(bool value) {
    auto s = settings();
    if (s.forceSdrPreview == value) return;
    s.forceSdrPreview = value;
    if(impl_->commit(std::move(s))){impl_->prefs["captureForceSdr"]=value;if(!impl_->savePrefs())emit notice(tr("偏好保存失败，已保留原设置"),true);}
    emit settingsChanged();
}

} // namespace oviora::ui
