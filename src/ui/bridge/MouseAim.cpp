// QML bridge: mouse aim and the mouse touchpad on the PS5 stream (the old Oviora's MouseAim and
// StreamSession's mouse touch, Tuşlar → Fareyle nişan alma / Fareyle dokunmatik yüzey).
//
// Mouse aim: while the picture is shown with nothing over it and the window has the focus, the
// pointer is captured (held still in the middle of the window, hidden) and the mouse's raw
// movement (WM_INPUT on the main window) turns the right stick; the mouse buttons press the
// buttons chosen for them. The toggle key (F8) lets the pointer go or takes it back; Esc lets it
// go; the stream menu (Ctrl+O), a dialog, Alt+Tab or the end of the stream let it go too. Merged
// into the controller tick (bridge/GameInput.cpp) like keyboard play.
//
// Stick model (as the old app): pointer speed (px/s, smoothed over a few ms) -> deflection.
// Sensitivity sets the speed that gives full deflection, the acceleration bends the curve
// (small moves finer, fast flicks stronger) and a fixed anti-dead zone skips the dead zone games
// put around the stick's centre.
#include "../QmlPlayerBridge_p.h"
#include "oviora/remoteplay/Types.h"
#include <QMouseEvent>

namespace oviora::ui {
namespace {
using Clock = std::chrono::steady_clock;
using remoteplay::ControllerState;
constexpr double kSmoothTau = 0.012;          // s, pointer speed smoothing
constexpr double kStopAfter = 0.045;          // s without movement: stick back to the centre
constexpr double kMinSpeed = 12.0;            // px/s; below this the stick rests
constexpr double kAntiDeadZone = 0.18;        // games ignore roughly the inner 10-20 % of the stick
constexpr double kFullSpeedAt50 = 1200.0;     // px/s for full deflection at sensitivity 50
constexpr auto kPressGuard = std::chrono::milliseconds(200); // a click that brings the window back doesn't fire
constexpr const char* kActionNames[] = {"none", "R2", "L2", "R1", "L1", "R3", "L3", "Square", "Cross", "Circle", "Triangle"};

double curve(double speed, int sensitivity, int accel) {
    if (speed < kMinSpeed) return 0.0;
    const double full = kFullSpeedAt50 * 50.0 / std::clamp(sensitivity, 1, 100);
    const double norm = std::min(1.0, speed / full);
    const double p = 1.0 + std::clamp(accel, 0, 100) / 100.0;
    return kAntiDeadZone + (1.0 - kAntiDeadZone) * std::pow(norm, p);
}
// The window's client centre as a 1x1 screen rectangle: the captured pointer stays there.
bool centreRect(HWND hwnd, RECT& out) {
    RECT r{};
    if (!hwnd || !GetClientRect(hwnd, &r)) return false;
    POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    if (!ClientToScreen(hwnd, &c)) return false;
    out = {c.x, c.y, c.x + 1, c.y + 1};
    return true;
}
}

bool QmlPlayerBridge::mouseAimCaptured() const { return impl_->mouseAim.captured; }
bool QmlPlayerBridge::mouseAimOn() const { return impl_->mouseAim.enabled; }

void QmlPlayerBridge::loadMouseAim() {
    auto& i = *impl_;
    auto& m = i.mouseAim;
    const bool pcInput = i.prefBool("keyboardMouse", true);
    const auto map = i.prefs.value("mouseAim").toMap();
    m.enabled = map.value("enabled", false).toBool() && pcInput;
    m.sensitivity = std::clamp(map.value("sensitivity", 50).toInt(), 1, 100);
    m.accel = std::clamp(map.value("accel", 30).toInt(), 0, 100);
    m.invertY = map.value("invert_y", false).toBool();
    m.actions[0] = std::clamp(map.value("left", int(Impl::MouseAim::R2)).toInt(), 0, Impl::MouseAim::ActionCount - 1);
    m.actions[1] = std::clamp(map.value("right", int(Impl::MouseAim::L2)).toInt(), 0, Impl::MouseAim::ActionCount - 1);
    m.actions[2] = std::clamp(map.value("middle", int(Impl::MouseAim::R3)).toInt(), 0, Impl::MouseAim::ActionCount - 1);
    m.toggleKey = map.value("toggle_key", int(Qt::Key_F8)).toInt();
    m.touch = i.prefBool("mouseTouch", true) && pcInput;
    if (!m.touch) { m.touchId = -1; m.touchClick = false; }
    // A setting changed while captured: the buttons start again from nothing.
    if (m.captured) { std::fill(std::begin(m.down), std::end(m.down), false); m.buttons = 0; m.l2 = m.r2 = 0; }
    oviora::log::info("mouse-aim", std::format("settings enabled={} sensitivity={} accel={} invertY={} left={} right={} middle={} toggleKey={} touchpad={}",
        m.enabled, m.sensitivity, m.accel, m.invertY, kActionNames[m.actions[0]], kActionNames[m.actions[1]], kActionNames[m.actions[2]],
        keyName(m.toggleKey).toStdString(), m.touch));
    updateMouseAim();
    emit mouseAimChanged();
}

// Capture when everything allows it, let go as soon as anything doesn't.
void QmlPlayerBridge::updateMouseAim() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    auto& m = i.mouseAim;
    if (!m.enabled && !m.captured) return;
    const bool ticking = i.controllerTimer && i.controllerTimer->isActive();
    const auto s = ticking ? i.engine.snapshot() : engine::PlayerSnapshot{};
    const bool streaming = ticking && s.remotePlay && s.running;
    if (streaming && s.sessionId != m.session) { m.session = s.sessionId; m.wanted = true; }
    QWindow* focus = QGuiApplication::focusWindow();
    const bool hostFocused = focus && i.videoWindow && GetAncestor(i.videoWindow, GA_ROOT) == reinterpret_cast<HWND>(focus->winId());
    const char* why = !m.enabled ? "setting off" : !streaming ? "no stream picture" : !i.gameKeysAllowed ? "menu or page over the picture"
        : !hostFocused ? "window not focused" : !m.wanted ? "released by key" : nullptr;
    if (!why && !m.captured) mouseAimCapture(focus);
    else if (why && m.captured) mouseAimRelease(why);
#endif
}

void QmlPlayerBridge::mouseAimCapture(QWindow* window) {
    auto& i = *impl_;
    auto& m = i.mouseAim;
    m.captured = true;
    m.window = window;
    m.hwnd = reinterpret_cast<HWND>(window->winId());
    m.accX = m.accY = m.velX = m.velY = 0;
    m.absKnown = false;
    m.lastTick = m.lastMotion = m.capturedAt = Clock::now();
    std::fill(std::begin(m.down), std::end(m.down), false);
    m.buttons = 0; m.l2 = m.r2 = 0; m.rightX = m.rightY = 0;
    m.touchId = -1; m.touchClick = false;
    // Raw mouse input to this window while it is in the foreground (no RIDEV_INPUTSINK).
    i.rawMouseFilter.target = m.hwnd;
    if (!i.rawMouseFilter.received) {
        i.rawMouseFilter.received = [this](const RAWMOUSE& raw) { mouseAimRaw(raw); };
        if (qApp) qApp->installNativeEventFilter(&i.rawMouseFilter);
    }
    RAWINPUTDEVICE rid{0x01, 0x02, 0, m.hwnd}; // generic desktop / mouse
    const bool registered = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
    RECT c{};
    bool clipped = false;
    if (!m.test && centreRect(m.hwnd, c)) { clipped = ClipCursor(&c) != FALSE; SetCursor(nullptr); }
    oviora::log::info("mouse-aim", std::format("captured rawInput={}{} clip={}", registered, registered ? "" : std::format(" err={}", GetLastError()),
        m.test ? "skipped (test)" : clipped ? "centre" : "failed"));
    emit mouseAimChanged();
}

void QmlPlayerBridge::mouseAimRelease(const char* why) {
    auto& i = *impl_;
    auto& m = i.mouseAim;
    if (!m.captured) return;
    m.captured = false;
    if (!m.test) ClipCursor(nullptr);
    RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_REMOVE, nullptr};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    i.rawMouseFilter.target = nullptr;
    m.window = nullptr; m.hwnd = nullptr;
    m.accX = m.accY = m.velX = m.velY = 0;
    std::fill(std::begin(m.down), std::end(m.down), false);
    m.buttons = 0; m.l2 = m.r2 = 0; m.rightX = m.rightY = 0;
    oviora::log::info("mouse-aim", std::format("released ({}) after {} raw packets", why, m.rawPackets));
    m.rawPackets = 0;
    emit mouseAimChanged();
}

void QmlPlayerBridge::mouseAimRaw(const RAWMOUSE& raw) {
    auto& m = impl_->mouseAim;
    if (!m.captured) return;
    ++m.rawPackets;
    const auto now = Clock::now();
    long dx = 0, dy = 0;
    if (raw.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // Remote desktop and tablets report positions (0..65535 over the screen): their change.
        const bool virt = raw.usFlags & MOUSE_VIRTUAL_DESKTOP;
        const int w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), h = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        if (m.absKnown) { dx = long(double(raw.lLastX - m.absX) * w / 65535.0); dy = long(double(raw.lLastY - m.absY) * h / 65535.0); }
        m.absX = raw.lLastX; m.absY = raw.lLastY; m.absKnown = true;
    } else { dx = raw.lLastX; dy = raw.lLastY; }
    if (dx || dy) { m.accX += dx; m.accY += dy; m.lastMotion = now; }
    static constexpr USHORT kDown[3] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN};
    static constexpr USHORT kUp[3] = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP};
    for (int b = 0; b < 3; ++b) {
        const bool press = raw.usButtonFlags & kDown[b], release = raw.usButtonFlags & kUp[b];
        if (!press && !release) continue;
        if (press && now - m.capturedAt < kPressGuard) continue;
        if (press == m.down[b]) continue;
        m.down[b] = press;
        uint32_t bit = 0;
        switch (m.actions[b]) {
        case Impl::MouseAim::R2: m.r2 = press ? 255 : 0; break;
        case Impl::MouseAim::L2: m.l2 = press ? 255 : 0; break;
        case Impl::MouseAim::R1: bit = ControllerState::R1; break;
        case Impl::MouseAim::L1: bit = ControllerState::L1; break;
        case Impl::MouseAim::R3: bit = ControllerState::R3; break;
        case Impl::MouseAim::L3: bit = ControllerState::L3; break;
        case Impl::MouseAim::Square: bit = ControllerState::Square; break;
        case Impl::MouseAim::Cross: bit = ControllerState::Cross; break;
        case Impl::MouseAim::Circle: bit = ControllerState::Circle; break;
        case Impl::MouseAim::Triangle: bit = ControllerState::Triangle; break;
        default: break;
        }
        if (bit) m.buttons = press ? (m.buttons | bit) : (m.buttons & ~bit);
        static constexpr const char* kNames[] = {"left", "right", "middle"};
        oviora::log::info("mouse-aim", std::format("{} button {} -> {}", kNames[b], press ? "down" : "up", kActionNames[m.actions[b]]));
    }
}

// Once per controller tick: the stick from the movement since the last one, merged into the
// state with the pad and the keyboard (the stronger deflection of each axis wins), and the
// mouse touchpad's finger and click.
void QmlPlayerBridge::mouseAimTick(ControllerState& state) {
    auto& i = *impl_;
    auto& m = i.mouseAim;
    if (m.captured) {
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - m.lastTick).count();
        if (dt > 0) {
            m.lastTick = now;
            const double rawX = m.accX / dt, rawY = m.accY / dt;
            m.accX = m.accY = 0;
            const double a = 1.0 - std::exp(-dt / kSmoothTau);
            m.velX += (rawX - m.velX) * a; m.velY += (rawY - m.velY) * a;
            if (std::chrono::duration<double>(now - m.lastMotion).count() > kStopAfter) m.velX = m.velY = 0;
            const double speed = std::hypot(m.velX, m.velY);
            const double d = curve(speed, m.sensitivity, m.accel);
            if (d <= 0) { m.rightX = m.rightY = 0; }
            else {
                m.rightX = int16_t(std::clamp(m.velX / speed * d * 32767.0, -32767.0, 32767.0));
                m.rightY = int16_t(std::clamp(m.velY / speed * d * (m.invertY ? -1.0 : 1.0) * 32767.0, -32767.0, 32767.0));
            }
        }
        // Keep the pointer held: Windows drops the clip on some focus and display changes.
        if (!m.test) {
            RECT want{}, have{};
            if (centreRect(m.hwnd, want) && GetClipCursor(&have) && !EqualRect(&want, &have)) ClipCursor(&want);
        }
        state.inputActive = true;
        state.buttons |= m.buttons;
        state.l2 = std::max(state.l2, m.l2); state.r2 = std::max(state.r2, m.r2);
        auto stronger = [](int16_t& v, int16_t mouse) { if (std::abs(int(mouse)) > std::abs(int(v))) v = mouse; };
        stronger(state.rightX, m.rightX); stronger(state.rightY, m.rightY);
        const auto now2 = Clock::now();
        const bool buttonsChanged = m.buttons != m.loggedButtons || m.l2 != m.loggedL2 || m.r2 != m.loggedR2;
        const bool stickChanged = m.rightX != m.loggedX || m.rightY != m.loggedY;
        // Every button change, the stick at most every 100 ms and always when it comes back to rest.
        if (buttonsChanged || (stickChanged && (now2 - m.loggedAt > std::chrono::milliseconds(100) || (!m.rightX && !m.rightY)))) {
            oviora::log::info("mouse-aim", std::format("stick right={},{} buttons=0x{:x} l2={} r2={} speed={:.0f}px/s raw={}",
                m.rightX, m.rightY, m.buttons, m.l2, m.r2, std::hypot(m.velX, m.velY), m.rawPackets));
            m.loggedButtons = m.buttons; m.loggedL2 = m.l2; m.loggedR2 = m.r2; m.loggedX = m.rightX; m.loggedY = m.rightY; m.loggedAt = now2;
        }
    }
    if (m.touchId >= 0 || m.touchClick) {
        state.inputActive = true;
        if (m.touchClick) state.buttons |= ControllerState::Touchpad;
        if (m.touchId >= 0) {
            // The pad's own fingers first; the mouse takes a free slot.
            for (auto& t : state.touches) if (t.id < 0) { t.id = m.touchId; t.x = m.touchX; t.y = m.touchY; break; }
        }
    }
}

// Window events: while captured the pointer's Qt events never reach the interface (the clicks
// go to the PS5 through raw input); the toggle key and Esc always belong to mouse aim.
bool QmlPlayerBridge::mouseAimEvent(QObject* watched, QEvent* event) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    auto& m = i.mouseAim;
    const auto type = event->type();
    switch (type) {
    case QEvent::MouseMove: case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease: case QEvent::MouseButtonDblClick:
    case QEvent::Wheel: case QEvent::HoverMove: case QEvent::HoverEnter: case QEvent::HoverLeave:
        if (m.captured && watched == m.window.data()) return true;
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseMove) {
            // The mouse touchpad: only over the picture itself (the video window's region has
            // holes where interface controls float over it).
            if (!m.touch || m.captured || !qobject_cast<QWindow*>(watched) || !i.videoWindow) return false;
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->source() != Qt::MouseEventNotSynthesized) return false;
            const bool held = m.touchId >= 0 || m.touchClick;
            // The window's own position (device independent pixels) in screen pixels: the event's,
            // not the real cursor's, so an injected test event lands where it says.
            auto* w = static_cast<QWindow*>(watched);
            const QPointF local = me->scenePosition() * w->devicePixelRatio();
            POINT p{0, 0};
            ClientToScreen(reinterpret_cast<HWND>(w->winId()), &p);
            p.x += LONG(std::lround(local.x())); p.y += LONG(std::lround(local.y()));
            RECT client{};
            GetClientRect(i.videoWindow, &client);
            POINT v = p;
            ScreenToClient(i.videoWindow, &v);
            bool inside = IsWindowVisible(i.videoWindow) && PtInRect(&client, v);
            if (inside) {
                HRGN region = CreateRectRgn(0, 0, 0, 0);
                if (GetWindowRgn(i.videoWindow, region) != ERROR) inside = PtInRegion(region, v.x, v.y) != FALSE;
                DeleteObject(region);
            }
            if (!inside && !held && type != QEvent::MouseButtonRelease) return false;
            const double x = client.right > 0 ? double(v.x) / client.right : 0, y = client.bottom > 0 ? double(v.y) / client.bottom : 0;
            const int phase = type == QEvent::MouseButtonPress ? 0 : type == QEvent::MouseMove ? 1 : 2;
            const int button = phase == 1 ? int(me->buttons()) : int(me->button());
            mouseTouch(phase, x, y, button);
        }
        return false;
    case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::ShortcutOverride: {
        if (!m.enabled) return false;
        const bool ticking = i.controllerTimer && i.controllerTimer->isActive();
        if (!ticking || !i.gameKeysAllowed) return false;
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
        const bool toggle = m.toggleKey && key->key() == m.toggleKey;
        const bool esc = key->key() == Qt::Key_Escape && m.captured;
        if (!toggle && !esc) return false;
        if (type == QEvent::ShortcutOverride) { event->accept(); return true; }
        if (type == QEvent::KeyPress && !key->isAutoRepeat() && qobject_cast<QWindow*>(watched)) {
            m.wanted = esc ? false : !m.captured;
            oviora::log::info("mouse-aim", std::format("{} pressed: pointer {}", esc ? "Esc" : keyName(m.toggleKey).toStdString(), m.wanted ? "wanted" : "let go"));
            updateMouseAim();
        }
        return true; // never reaches the PS5 or a shortcut
    }
    default:
        return false;
    }
#else
    Q_UNUSED(watched); Q_UNUSED(event);
    return false;
#endif
}

bool QmlPlayerBridge::mouseTouch(int phase, double x, double y, int button) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    auto& m = i.mouseAim;
    const bool allowed = m.touch && !m.captured && i.gameKeysAllowed && i.controllerTimer && i.controllerTimer->isActive();
    if (!allowed) {
        if (m.touchId >= 0 || m.touchClick) { m.touchId = -1; m.touchClick = false; oviora::log::info("mouse-touch", "touch ended (not allowed now)"); }
        return false;
    }
    const auto tx = uint16_t(std::clamp(x, 0.0, 1.0) * 1919), ty = uint16_t(std::clamp(y, 0.0, 1.0) * 1079);
    if (phase == 0) {
        // The left button drags a finger; any other clicks the touchpad.
        if (button != Qt::LeftButton && !m.touchClick) { m.touchClick = true; oviora::log::info("mouse-touch", "touchpad click down"); }
    } else if (phase == 1) {
        if (button == Qt::LeftButton) {
            if (m.touchId < 0) {
                m.touchId = int8_t(m.nextTouchId++ & 0x7f);
                oviora::log::info("mouse-touch", std::format("finger down id={} at {},{}", m.touchId, tx, ty));
            }
            m.touchX = tx; m.touchY = ty;
        }
    } else {
        if (button == Qt::LeftButton && m.touchId >= 0) {
            oviora::log::info("mouse-touch", std::format("finger up id={} at {},{}", m.touchId, m.touchX, m.touchY));
            m.touchId = -1;
        }
        if (m.touchClick) { m.touchClick = false; oviora::log::info("mouse-touch", "touchpad click up"); }
    }
    return true;
#else
    Q_UNUSED(phase); Q_UNUSED(x); Q_UNUSED(y); Q_UNUSED(button);
    return false;
#endif
}

void QmlPlayerBridge::testRawMouse(int dx, int dy, unsigned buttonFlags) {
    RAWMOUSE raw{};
    raw.usFlags = MOUSE_MOVE_RELATIVE;
    raw.usButtonFlags = USHORT(buttonFlags);
    raw.lLastX = dx; raw.lLastY = dy;
    // The first packet of a run that arrives uncaptured says so; the rest are dropped silently.
    static bool toldIgnored = false;
    if (!impl_->mouseAim.captured) {
        if (!std::exchange(toldIgnored, true))
            oviora::log::info("mouse-aim", std::format("test raw input {},{} flags=0x{:x} ignored: not captured", dx, dy, buttonFlags));
        return;
    }
    toldIgnored = false;
    mouseAimRaw(raw);
}
void QmlPlayerBridge::setMouseAimTest(bool on) {
    impl_->mouseAim.test = on;
    oviora::log::info("mouse-aim", std::format("test mode={} (the real cursor is never clipped or hidden)", on));
}
} // namespace oviora::ui
