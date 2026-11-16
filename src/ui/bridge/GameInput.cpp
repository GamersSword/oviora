// QML bridge: game input. The old Oviora's controller and keyboard settings on the PS5 stream:
// the pad's stream menu combination, rumble and haptics strength, the pad's button mapping and
// keyboard play with its key mapping. One controller tick merges the pad and the keyboard and
// sends them on the PS5 session's controller path (RemotePlaySessionSource::controller).
#include "../QmlPlayerBridge_p.h"

namespace oviora::ui {
namespace {
// Old Chiaki button values (KeysPage and the keyMap preference use them): the 16 digital
// buttons are the ControllerState bits, then L2, R2 and the eight stick directions.
constexpr uint32_t kL2 = 1u << 16, kR2 = 1u << 17;
constexpr uint32_t kLeftRight = 1u << 18, kLeftLeft = 1u << 19, kLeftUp = 1u << 20, kLeftDown = 1u << 21;
constexpr uint32_t kRightRight = 1u << 22, kRightLeft = 1u << 23, kRightUp = 1u << 24, kRightDown = 1u << 25;
// The keys the old app starts with (Settings::GetControllerMapping), in button order.
constexpr std::pair<uint32_t, int> kDefaultKeys[] = {
    {1u << 0, Qt::Key_Return}, {1u << 1, Qt::Key_Backspace}, {1u << 2, Qt::Key_Backslash}, {1u << 3, Qt::Key_C},
    {1u << 4, Qt::Key_Left}, {1u << 5, Qt::Key_Right}, {1u << 6, Qt::Key_Up}, {1u << 7, Qt::Key_Down},
    {1u << 8, Qt::Key_2}, {1u << 9, Qt::Key_3}, {1u << 10, Qt::Key_5}, {1u << 11, Qt::Key_6},
    {1u << 12, Qt::Key_O}, {1u << 13, Qt::Key_F}, {1u << 14, Qt::Key_T}, {1u << 15, Qt::Key_Escape},
    {kL2, Qt::Key_1}, {kR2, Qt::Key_4},
    {kLeftRight, Qt::Key_BracketRight}, {kLeftLeft, Qt::Key_BracketLeft}, {kLeftUp, Qt::Key_Insert}, {kLeftDown, Qt::Key_Delete},
    {kRightRight, Qt::Key_Equal}, {kRightLeft, Qt::Key_Minus}, {kRightUp, Qt::Key_PageUp}, {kRightDown, Qt::Key_PageDown}};
std::string hex(uint32_t v) { return std::format("0x{:x}", v); }
}

void QmlPlayerBridge::loadGameInput() {
    auto& i = *impl_;
    auto& g = i.gameInput;
    g.menuCombo = i.prefBool("padMenuCombo", true);
    const QVariantList combo = i.prefs.contains("padMenuButtons") ? i.prefs.value("padMenuButtons").toList() : QVariantList{9, 10, 11, 12};
    g.menuButtons = 0;
    for (const auto& v : combo) { const int n = v.toInt(); if (n >= 1 && n <= 16) g.menuButtons |= 1u << (n - 1); }
    for (int k = 0; k < 16; ++k) g.padSource[size_t(k)] = uint8_t(k);
    const auto map = i.prefs.value("padButtonMap").toMap();
    for (auto it = map.begin(); it != map.end(); ++it) {
        bool ok = false; const int out = it.key().toInt(&ok); const int in = it.value().toInt();
        if (ok && out >= 0 && out < 16 && in >= 0 && in < 16) g.padSource[size_t(out)] = uint8_t(in);
    }
    g.padRemapped = false;
    for (int k = 0; k < 16; ++k) g.padRemapped = g.padRemapped || g.padSource[size_t(k)] != k;
    static constexpr float kRumble[] = {0.f, .2f, .5f, 1.f, 2.f, 5.f}; // Kapalı … Çok güçlü (old intensities)
    g.rumble = kRumble[std::clamp(i.prefInt("rumbleStrength", 3), 0, 5)];
    g.haptics = float(std::clamp(i.prefInt("hapticStrength", 100), 0, 200)) / 100.f;
    g.keyboard = i.prefBool("keyboardMouse", true) && i.prefBool("keyboardPlay", true);
    g.keys.clear();
    const auto custom = i.prefs.value("keyMap").toMap();
    // As the old app inverted its map: a key given to two buttons works for the later one.
    for (const auto& [action, key] : kDefaultKeys) {
        const QString id = QString::number(action);
        const int code = custom.contains(id) ? custom.value(id).toInt() : key;
        if (code > 0) g.keys.insert(code, action);
    }
#ifdef OVIORA_ENABLE_REMOTEPLAY
    i.controller.setFeedbackScale(g.rumble, g.haptics);
#endif
    if (!g.keyboard) releaseGameKeys();
    loadMouseAim();
    oviora::log::info("game-input", std::format("menuCombo={} buttons={} padRemapped={} rumble={:.1f} haptics={:.2f} keyboard={} keys={} customKeys={}",
        g.menuCombo, hex(g.menuButtons), g.padRemapped, g.rumble, g.haptics, g.keyboard, g.keys.size(), custom.size()));
}

bool QmlPlayerBridge::gameKeysAllowed() const { return impl_->gameKeysAllowed; }
void QmlPlayerBridge::setGameKeysAllowed(bool on) {
    if (impl_->gameKeysAllowed == on) return;
    impl_->gameKeysAllowed = on;
    if (!on) releaseGameKeys();
    updateMouseAim();
    emit gameKeysAllowedChanged();
}
int QmlPlayerBridge::padPressed() const { return impl_->padPressed; }
QString QmlPlayerBridge::keyName(int key) const {
    if (key <= 0) return {};
    return QKeySequence(key).toString(QKeySequence::PortableText);
}
// Every key the keyboard holds on the PS5 is let go (focus loss, a menu over the picture).
void QmlPlayerBridge::releaseGameKeys() {
    auto& i = *impl_;
    if (i.mouseAim.touchId >= 0 || i.mouseAim.touchClick) {
        i.mouseAim.touchId = -1; i.mouseAim.touchClick = false;
        oviora::log::info("mouse-touch", "touch ended (input released)");
    }
    if (!i.keysHeld && i.keysDown.isEmpty()) return;
    i.keysDown.clear(); i.keysHeld = 0;
    oviora::log::info("remoteplay-keyboard", "released all keys");
}

// One controller tick (8 ms) while a PS5 session runs.
void QmlPlayerBridge::pollController() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& j = *impl_;
    const auto& g = j.gameInput;
    // Oviora: the pad keeps playing while another program is in front (Alt+Tab), unless
    // "Pencere arkadayken de dinle" is turned off (Ayarlar > Kumandalar; on by default)
    // The keyboard stays tied to the focus (a key held at Alt+Tab never sees its release)
    const bool focused = QGuiApplication::focusWindow() != nullptr;
    const bool padBackground = j.prefBool("padBackground", true);
    const bool padReads = focused || padBackground;
    {
        static int told = -1;
        const int now = (focused ? 1 : 0) | (padBackground ? 2 : 0);
        if (now != told) {
            told = now;
            oviora::log::info("remoteplay-input", std::format("controller polling: window focused={} padBackground={} -> pad read={}",
                focused, padBackground, padReads));
        }
    }
    auto state = j.controller.poll(padReads);
    uint32_t raw = state.buttons & 0xffffu;
    if (int(raw) != j.padPressed) { j.padPressed = int(raw); emit padPressedChanged(); }
    // The stream menu combination: opens/closes the menu on the press and its buttons stay
    // away from the PS5 until all of them are let go.
    if (g.menuCombo && g.menuButtons) {
        if (!j.menuComboHeld && (raw & g.menuButtons) == g.menuButtons) {
            j.menuComboHeld = true;
            oviora::log::info("remoteplay-input", std::format("stream menu combination {} pressed", hex(g.menuButtons)));
            emit streamMenuRequested();
        } else if (j.menuComboHeld && !(raw & g.menuButtons)) j.menuComboHeld = false;
        if (j.menuComboHeld) raw &= ~g.menuButtons;
    } else j.menuComboHeld = false;
    // The mapping dialog is picking a button: nothing goes to the game meanwhile.
    if (j.padCaptureOn) raw = 0;
    if (g.padRemapped && raw) {
        uint32_t out = 0;
        for (int k = 0; k < 16; ++k) if (raw & (1u << g.padSource[size_t(k)])) out |= 1u << k;
        raw = out;
    }
    state.buttons = raw;
    // Keyboard play: the keys held, and a key pressed and let go since the last tick.
    if (!focused) releaseGameKeys();
    const uint32_t kb = (j.keysHeld | j.keysTapped);
    const uint32_t tapped = std::exchange(j.keysTapped, 0);
    if (focused && g.keyboard && j.gameKeysAllowed) state.inputActive = true;
    if (kb) {
        state.buttons |= kb & 0xffffu;
        if (kb & kL2) state.l2 = 255;
        if (kb & kR2) state.r2 = 255;
        auto axis = [kb](int16_t& v, uint32_t plus, uint32_t minus) {
            const int d = ((kb & plus) ? 1 : 0) - ((kb & minus) ? 1 : 0);
            if (d) v = int16_t(d * 0x7fff);
        };
        axis(state.leftX, kLeftRight, kLeftLeft); axis(state.leftY, kLeftDown, kLeftUp);
        axis(state.rightX, kRightRight, kRightLeft); axis(state.rightY, kRightDown, kRightUp);
    }
    if (kb != j.keysSent) {
        oviora::log::info("remoteplay-keyboard", std::format("controller state actions={} buttons={} left={},{} right={},{} l2={} r2={}{}",
            hex(kb), hex(state.buttons), state.leftX, state.leftY, state.rightX, state.rightY, state.l2, state.r2, tapped ? " (tap)" : ""));
        j.keysSent = kb;
    }
    // Mouse aim and the mouse touchpad (bridge/MouseAim.cpp).
    updateMouseAim();
    mouseAimTick(state);
    j.engine.remotePlayController(std::move(state));
    j.controller.feedback(j.engine.remotePlayFeedback(), focused);
#endif
}

void QmlPlayerBridge::padCapture(bool on) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (i.padCaptureOn == on) return;
    i.padCaptureOn = on;
    oviora::log::info("remoteplay-input", std::format("button mapping capture={}", on));
    if (on) {
        if (!i.acquirePad(Impl::PadMapping)) oviora::log::warn("remoteplay-input", "SDL gamepad initialization failed");
        if (!i.padCaptureTimer) {
            i.padCaptureTimer = new QTimer(this);
            connect(i.padCaptureTimer, &QTimer::timeout, this, [this] {
                auto& j = *impl_;
                // A running stream reads the pad itself (pollController) and updates padPressed.
                if (j.controllerTimer && j.controllerTimer->isActive()) return;
                const bool was = j.controller.connected();
                const auto s = j.controller.poll(QGuiApplication::focusWindow() != nullptr);
                const int raw = int(s.buttons & 0xffffu);
                if (raw != j.padPressed) { j.padPressed = raw; emit padPressedChanged(); }
                if (was != j.controller.connected()) emit ps5Changed();
            });
        }
        i.padCaptureTimer->start(16);
    } else {
        if (i.padCaptureTimer) i.padCaptureTimer->stop();
        i.releasePad(Impl::PadMapping);
        if (i.padPressed) { i.padPressed = 0; emit padPressedChanged(); }
        emit ps5Changed();
    }
#else
    Q_UNUSED(on);
#endif
}

// Keyboard play (old StreamSession::HandleKeyboardEvent). While the picture is shown and a
// PS5 session takes input, a mapped key goes to the PS5 and no app shortcut sees it. Never
// taken: Ctrl/Alt/Win combinations (Ctrl+O), F11, and Esc in fullscreen (leaves fullscreen).
bool QmlPlayerBridge::gameKeyEvent(QObject* watched, QEvent* event) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    const auto type = event->type();
    if (type != QEvent::KeyPress && type != QEvent::KeyRelease && type != QEvent::ShortcutOverride) return false;
    auto* key = static_cast<QKeyEvent*>(event);
    const bool atWindow = qobject_cast<QWindow*>(watched) != nullptr;
    if (type == QEvent::KeyRelease) {
        // A key that pressed a PS5 button always lets it go, whatever happened since.
        const auto it = i.keysDown.find(key->key());
        if (it == i.keysDown.end()) return false;
        if (!atWindow) return true;
        if (key->isAutoRepeat()) return true;
        const uint32_t action = it.value();
        i.keysDown.erase(it);
        uint32_t still = 0;
        for (auto other = i.keysDown.cbegin(); other != i.keysDown.cend(); ++other) still |= other.value();
        i.keysHeld = still;
        oviora::log::info("remoteplay-keyboard", std::format("key {} up action={}", keyName(key->key()).toStdString(), hex(action)));
        return true;
    }
    const auto& g = i.gameInput;
    if (!g.keyboard || !i.gameKeysAllowed || !i.controllerTimer || !i.controllerTimer->isActive()) return false;
    if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
    if (key->key() == Qt::Key_F11) return false;
    const auto found = g.keys.constFind(key->key());
    if (found == g.keys.cend()) return false;
    const QObject* focus = QGuiApplication::focusObject();
    if (focus && (focus->inherits("QQuickTextInput") || focus->inherits("QQuickTextEdit"))) return false;
    if (key->key() == Qt::Key_Escape) {
        const QWindow* window = QGuiApplication::focusWindow();
        if (window && window->visibility() == QWindow::FullScreen) return false;
    }
    if (!remotePlayActive()) return false;
    if (type == QEvent::ShortcutOverride) { event->accept(); return true; }
    if (!atWindow) return true;
    if (!key->isAutoRepeat() && !i.keysDown.contains(key->key())) {
        i.keysDown.insert(key->key(), found.value());
        i.keysHeld |= found.value();
        i.keysTapped |= found.value();
        oviora::log::info("remoteplay-keyboard", std::format("key {} down action={}", keyName(key->key()).toStdString(), hex(found.value())));
    }
    return true;
#else
    Q_UNUSED(watched); Q_UNUSED(event);
    return false;
#endif
}
} // namespace oviora::ui
