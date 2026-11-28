import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Templates as T
import Oviora
import "FocusNav.js" as Nav

// Old Oviora's "Tuşlar" page (settings/KeysPage.qml), ported as it was. Keyboard as a controller:
// every PlayStation button with its key, changed by pressing a new key. Wired to the new app's
// preferences (bridge/GameInput.cpp): keyboardMouse, keyboardPlay and keyMap (old button value ->
// Qt key, only the keys changed from the defaults). Kept but hidden until the new app has them:
// the mouse as touchpad and mouse aiming.
SettingsPage {
    id: page
    maxContentWidth: 1180
    signal confirmRequested(string title, string text, var action)

    // ── Mapping state ──
    // Every button with its key: the defaults with the saved changes over them
    readonly property var customKeys: oviora.preferences.keyMap || ({})
    readonly property var controllerMapping: {
        const out = [];
        const order = Object.keys(defaults).map(Number).sort((a, b) => a - b);
        for (let i = 0; i < order.length; i++) {
            const b = order[i];
            const custom = customKeys[String(b)];
            const code = custom !== undefined ? Number(custom) : defaults[b];
            out.push({ buttonValue: b, keyCode: code, keyName: oviora.keyName(code) });
        }
        return out;
    }
    readonly property var byButton: {
        const m = {};
        const list = page.controllerMapping;
        for (let i = 0; i < list.length; i++)
            m[list[i].buttonValue] = list[i];
        return m;
    }
    // The keys the app starts with (Settings::GetControllerMapping)
    readonly property var defaults: ({
        1: Qt.Key_Return, 2: Qt.Key_Backspace, 4: Qt.Key_Backslash, 8: Qt.Key_C,
        16: Qt.Key_Left, 32: Qt.Key_Right, 64: Qt.Key_Up, 128: Qt.Key_Down,
        256: Qt.Key_2, 512: Qt.Key_3, 1024: Qt.Key_5, 2048: Qt.Key_6,
        4096: Qt.Key_O, 8192: Qt.Key_F, 16384: Qt.Key_T, 32768: Qt.Key_Escape,
        65536: Qt.Key_1, 131072: Qt.Key_4,
        262144: Qt.Key_BracketRight, 524288: Qt.Key_BracketLeft, 1048576: Qt.Key_Insert, 2097152: Qt.Key_Delete,
        4194304: Qt.Key_Equal, 8388608: Qt.Key_Minus, 16777216: Qt.Key_PageUp, 33554432: Qt.Key_PageDown
    })
    // button value: [glyph, name in the list, full name for messages, hint]
    readonly property var info: ({
        64: ["dpad-up", qsTr("Yukarı"), qsTr("yön yukarı"), ""],
        128: ["dpad-down", qsTr("Aşağı"), qsTr("yön aşağı"), ""],
        16: ["dpad-left", qsTr("Sol"), qsTr("yön sol"), ""],
        32: ["dpad-right", qsTr("Sağ"), qsTr("yön sağ"), ""],
        1: ["cross", qsTr("Çarpı"), qsTr("Çarpı"), qsTr("Menülerde onaylar")],
        2: ["circle", qsTr("Daire"), qsTr("Daire"), qsTr("Menülerde geri döner")],
        4: ["square", qsTr("Kare"), qsTr("Kare"), ""],
        8: ["triangle", qsTr("Üçgen"), qsTr("Üçgen"), ""],
        256: ["l1", qsTr("L1"), qsTr("L1"), qsTr("Sol omuz tuşu")],
        65536: ["l2", qsTr("L2"), qsTr("L2"), qsTr("Sol tetik, tam basılır")],
        512: ["r1", qsTr("R1"), qsTr("R1"), qsTr("Sağ omuz tuşu")],
        131072: ["r2", qsTr("R2"), qsTr("R2"), qsTr("Sağ tetik, tam basılır")],
        1048576: ["ls-up", qsTr("Yukarı itme"), qsTr("sol çubuk yukarı"), ""],
        2097152: ["ls-down", qsTr("Aşağı itme"), qsTr("sol çubuk aşağı"), ""],
        524288: ["ls-left", qsTr("Sola itme"), qsTr("sol çubuk sol"), ""],
        262144: ["ls-right", qsTr("Sağa itme"), qsTr("sol çubuk sağ"), ""],
        1024: ["l3", qsTr("L3"), qsTr("L3"), qsTr("Sol çubuğa basmak")],
        16777216: ["rs-up", qsTr("Yukarı itme"), qsTr("sağ çubuk yukarı"), ""],
        33554432: ["rs-down", qsTr("Aşağı itme"), qsTr("sağ çubuk aşağı"), ""],
        8388608: ["rs-left", qsTr("Sola itme"), qsTr("sağ çubuk sol"), ""],
        4194304: ["rs-right", qsTr("Sağa itme"), qsTr("sağ çubuk sağ"), ""],
        2048: ["r3", qsTr("R3"), qsTr("R3"), qsTr("Sağ çubuğa basmak")],
        4096: ["options", qsTr("Options"), qsTr("Options"), qsTr("Oyunun menüsünü açar")],
        8192: ["create", qsTr("Create"), qsTr("Create"), qsTr("Ekran görüntüsü ve paylaşım")],
        16384: ["touchpad", qsTr("Dokunmatik yüzey"), qsTr("dokunmatik yüzey"), qsTr("Yüzeye basmak")],
        32768: ["ps", qsTr("PS düğmesi"), qsTr("PS düğmesi"), qsTr("Konsolun ana menüsü")]
    })

    property int capturing: -1          // button waiting for a key
    property var pending: null          // {button, key, other}: the key is already used by "other"

    function keyCode(button) { return byButton[button] ? byButton[button].keyCode : 0; }
    function keyName(button) { return byButton[button] ? byButton[button].keyName : ""; }
    function fullName(button) { return info[button] ? info[button][2] : ""; }
    function usersOf(code, except) {
        const out = [];
        const list = page.controllerMapping;
        for (let i = 0; i < list.length; i++)
            if (list[i].keyCode === code && list[i].buttonValue !== except)
                out.push(list[i].buttonValue);
        return out;
    }
    readonly property int duplicateCount: {
        const seen = {};
        let n = 0;
        const list = page.controllerMapping;
        for (let i = 0; i < list.length; i++) {
            const c = list[i].keyCode;
            seen[c] = (seen[c] || 0) + 1;
            if (seen[c] === 2)
                n++;
        }
        return n;
    }
    readonly property int changedCount: {
        let n = 0;
        const list = page.controllerMapping;
        for (let i = 0; i < list.length; i++)
            if (defaults[list[i].buttonValue] !== undefined && list[i].keyCode !== defaults[list[i].buttonValue])
                n++;
        return n;
    }

    // How a key is written on Turkish keyboards
    function pretty(name) {
        switch (name) {
        case "Return": return "Enter";
        case "Enter": return qsTr("Enter (sayısal)");
        case "Up": return "↑";
        case "Down": return "↓";
        case "Left": return "←";
        case "Right": return "→";
        case "Space": return qsTr("Boşluk");
        case "PgUp": return "Page Up";
        case "PgDown": return "Page Down";
        case "Ins": return "Insert";
        case "Del": return "Delete";
        case "Backspace": return "Backspace";
        case "Esc": return "Esc";
        case "CapsLock": return "Caps Lock";
        case "": return qsTr("Yok");
        }
        return name;
    }
    function prettyCode(code) {
        const users = usersOf(code, -1);
        return users.length ? pretty(keyName(users[0])) : "";
    }

    function change(button, key) {
        const map = Object.assign({}, oviora.preferences.keyMap || {});
        if (key === defaults[button])
            delete map[String(button)];
        else
            map[String(button)] = key;
        oviora.setPreference("keyMap", map);
    }
    function startCapture(button) {
        pending = null;
        capturing = button;
    }
    function cancelCapture() {
        capturing = -1;
    }
    function captured(button, key) {
        capturing = -1;
        if (key === keyCode(button))
            return;
        const others = usersOf(key, button);
        if (others.length > 0) {
            pending = { button: button, key: key, other: others[0], oldKey: keyCode(button) };
            return;
        }
        change(button, key);
    }
    function resetButton(button) {
        const def = defaults[button];
        if (def === undefined || def === keyCode(button))
            return;
        captured(button, def);
    }
    function swapPending() {
        if (!pending)
            return;
        const p = pending;
        pending = null;
        change(p.button, p.key);
        change(p.other, p.oldKey);
    }
    function assignPending() {
        if (!pending)
            return;
        const p = pending;
        pending = null;
        change(p.button, p.key);
    }

    // ── One button ──
    component KeyRow: Rectangle {
        id: keyRow
        required property int button
        readonly property bool isSettingRow: true
        readonly property bool isCapturing: page.capturing === button
        readonly property bool isPending: page.pending !== null && page.pending.button === button
        readonly property int code: page.keyCode(button)
        readonly property bool isDefault: page.defaults[button] === code
        readonly property var clashes: page.usersOf(code, button)
        readonly property bool current: changeButton.activeFocus || resetButton.activeFocus || catcher.activeFocus

        Layout.fillWidth: true
        implicitHeight: main.implicitHeight + 16 + (pendingBox.visible ? pendingBox.implicitHeight + 10 : 0)
        radius: 12
        color: isCapturing || isPending ? Tema.surface2 : (hover.hovered ? Tema.surface2 : "transparent")
        border.width: isCapturing ? 1 : 0
        border.color: Tema.accent

        HoverHandler { id: hover }

        // light bar
        Rectangle {
            anchors {
                left: parent.left
                leftMargin: -14
                top: parent.top
                topMargin: 14
            }
            width: 4
            height: 24
            radius: 2
            color: Tema.accent
            opacity: keyRow.current || keyRow.isCapturing ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        // While waiting for a key every key press lands here
        Item {
            id: catcher
            readonly property bool keyCatcher: true
            focus: false
            Keys.onPressed: (event) => {
                event.accepted = true;
                if (event.isAutoRepeat)
                    return;
                if (event.key === Qt.Key_Escape) {
                    page.cancelCapture();
                    changeButton.forceActiveFocus(Qt.TabFocusReason);
                    return;
                }
                if (!event.key || event.key === Qt.Key_unknown)
                    return;
                page.captured(keyRow.button, event.key);
                if (keyRow.isPending)
                    swapButton.forceActiveFocus(Qt.TabFocusReason);
                else
                    changeButton.forceActiveFocus(Qt.TabFocusReason);
            }
            Keys.onReleased: (event) => { event.accepted = true; }
            Keys.onShortcutOverride: (event) => { event.accepted = true; }
            onActiveFocusChanged: if (!activeFocus && keyRow.isCapturing) page.cancelCapture()
        }
        Connections {
            target: page
            function onCapturingChanged() {
                if (page.capturing === keyRow.button)
                    catcher.forceActiveFocus(Qt.OtherFocusReason);
            }
        }

        RowLayout {
            id: main
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                leftMargin: 8
                rightMargin: 8
                topMargin: 8
            }
            spacing: 14

            ButtonGlyph {
                kind: page.info[keyRow.button][0]
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: page.info[keyRow.button][1]
                    elide: Text.ElideRight
                    font.family: Tema.body
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Tema.text
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: keyRow.clashes.length > 0
                        ? qsTr("Aynı tuş %1 için de atanmış").arg(page.fullName(keyRow.clashes[0]))
                        : page.info[keyRow.button][3]
                    elide: Text.ElideRight
                    font.family: Tema.body
                    font.pixelSize: 12
                    color: keyRow.clashes.length > 0 ? Tema.warm : Tema.muted
                }
            }

            // the key, or the request for one
            Item {
                Layout.preferredWidth: keyRow.isCapturing ? waitLabel.implicitWidth : cap.implicitWidth
                Layout.preferredHeight: 36
                KeyCap {
                    id: cap
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    visible: !keyRow.isCapturing
                    text: keyRow.isPending ? page.pretty(oviora.keyName(page.pending.key)) : page.pretty(page.keyName(keyRow.button))
                    warn: keyRow.clashes.length > 0 || keyRow.isPending
                }
                Label {
                    textFormat: Text.PlainText
                    id: waitLabel
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    visible: keyRow.isCapturing
                    text: qsTr("Bir tuşa bas…")
                    font.family: Tema.body
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Tema.accent
                    SequentialAnimation on opacity {
                        running: keyRow.isCapturing && waitLabel.visible
                        loops: Animation.Infinite
                        NumberAnimation { from: 1; to: 0.45; duration: 650; easing.type: Easing.InOutSine }
                        NumberAnimation { from: 0.45; to: 1; duration: 650; easing.type: Easing.InOutSine }
                        onStopped: waitLabel.opacity = 1
                    }
                }
            }

            PillButton {
                id: changeButton
                compact: true
                kind: keyRow.isCapturing ? "quiet" : "secondary"
                Layout.preferredWidth: 96
                text: keyRow.isCapturing ? qsTr("Vazgeç") : qsTr("Değiştir")
                onClicked: {
                    if (keyRow.isCapturing) {
                        page.cancelCapture();
                    } else {
                        page.startCapture(keyRow.button);
                    }
                }
                Keys.onRightPressed: if (resetButton.visible) resetButton.forceActiveFocus(Qt.TabFocusReason)
            }

            // back to the starting key
            T.AbstractButton {
                id: resetButton
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                opacity: keyRow.isDefault ? 0 : 1
                enabled: !keyRow.isDefault
                onClicked: {
                    page.resetButton(keyRow.button);
                    changeButton.forceActiveFocus(Qt.TabFocusReason);
                }
                Keys.onLeftPressed: changeButton.forceActiveFocus(Qt.TabFocusReason)
                Keys.onPressed: (event) => {
                    if (Nav.handleUpDown(resetButton, event))
                        return;
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                        resetButton.clicked();
                        event.accepted = true;
                    }
                }
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Varsayılana döndür (%1)").arg(page.pretty(page.prettyDefault(keyRow.button)))
                background: Rectangle {
                    radius: 18
                    color: resetButton.hovered ? Tema.surface2 : "transparent"
                    border.width: resetButton.activeFocus ? 2 : 0
                    border.color: Tema.accent
                }
                contentItem: Label {
                    textFormat: Text.PlainText
                    text: "↺"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 19
                    color: resetButton.hovered ? Tema.text : Tema.muted
                }
            }
        }

        // The key is already someone else's: swap, keep both, or leave it
        Rectangle {
            id: pendingBox
            visible: keyRow.isPending
            anchors {
                left: parent.left
                right: parent.right
                top: main.bottom
                leftMargin: 8
                rightMargin: 8
                topMargin: 10
            }
            implicitHeight: pendingCol.implicitHeight + 24
            radius: 10
            color: "#2A2018"
            border.color: "#5A3F22"
            ColumnLayout {
                id: pendingCol
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: 14
                    rightMargin: 14
                }
                spacing: 10
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: keyRow.isPending
                        ? qsTr("%1 tuşu şu an %2 için kullanılıyor. Takas edersen %2 tuşu %3 olur.")
                            .arg(page.pretty(oviora.keyName(page.pending.key)))
                            .arg(page.fullName(page.pending.other))
                            .arg(page.pretty(page.keyName(keyRow.button)))
                        : ""
                    font.family: Tema.body
                    font.pixelSize: 13
                    color: Tema.warm
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    PillButton {
                        id: swapButton
                        compact: true
                        kind: "primary"
                        text: qsTr("Takas et")
                        onClicked: {
                            page.swapPending();
                            changeButton.forceActiveFocus(Qt.TabFocusReason);
                        }
                        Keys.onRightPressed: keepButton.forceActiveFocus(Qt.TabFocusReason)
                    }
                    PillButton {
                        id: keepButton
                        compact: true
                        text: qsTr("İkisinde de kalsın")
                        onClicked: {
                            page.assignPending();
                            changeButton.forceActiveFocus(Qt.TabFocusReason);
                        }
                        Keys.onLeftPressed: swapButton.forceActiveFocus(Qt.TabFocusReason)
                        Keys.onRightPressed: dropButton.forceActiveFocus(Qt.TabFocusReason)
                    }
                    PillButton {
                        id: dropButton
                        compact: true
                        kind: "quiet"
                        text: qsTr("Vazgeç")
                        onClicked: {
                            page.pending = null;
                            changeButton.forceActiveFocus(Qt.TabFocusReason);
                        }
                        Keys.onLeftPressed: keepButton.forceActiveFocus(Qt.TabFocusReason)
                    }
                }
            }
        }
    }

    function prettyDefault(button) {
        // QKeySequence names of the default keys, as the backend writes them
        const names = {
            1: "Return", 2: "Backspace", 4: "\\", 8: "C", 16: "Left", 32: "Right", 64: "Up", 128: "Down",
            256: "2", 512: "3", 1024: "5", 2048: "6", 4096: "O", 8192: "F", 16384: "T", 32768: "Esc",
            65536: "1", 131072: "4", 262144: "]", 524288: "[", 1048576: "Ins", 2097152: "Del",
            4194304: "=", 8388608: "-", 16777216: "PgUp", 33554432: "PgDown"
        };
        return names[button] || "";
    }

    // ── Page ──
    SettingsCard {
        title: qsTr("Klavyeyle oynama")
        description: qsTr("Kumandan yokken oyunu klavyeyle oynayabilirsin. Her PlayStation tuşunun karşısında, onun yerine basacağın klavye tuşu var.")

        SettingRow {
            title: qsTr("Klavye ve fare")
            help: qsTr("Kapalıyken yayında klavye, fareyle dokunmatik yüzey ve fareyle nişan konsola hiçbir şey göndermez. Yayın menüsünden (Ctrl+O) de açıp kapatabilirsin.")
            Toggle {
                checked: oviora.preferences.keyboardMouse !== false
                onToggled: oviora.setPreference("keyboardMouse", checked)
            }
        }
        SettingRow {
            enabled: oviora.preferences.keyboardMouse !== false
            title: qsTr("Klavyeyi kumanda gibi kullan")
            help: qsTr("Kapalıyken yayında klavye tuşları konsola gitmez.")
            Toggle {
                checked: oviora.preferences.keyboardPlay !== false
                onToggled: oviora.setPreference("keyboardPlay", checked)
            }
        }
        // Old mouseTouchEnabled (on by default): a left drag over the picture is a finger on the
        // touchpad, another button clicks it (bridge/MouseAim.cpp)
        SettingRow {
            enabled: oviora.preferences.keyboardMouse !== false
            title: qsTr("Fareyle dokunmatik yüzey")
            help: qsTr("Fareye tıklayıp sürükleyince kumandanın dokunmatik yüzeyine dokunmuş gibi olur.")
            Toggle {
                checked: oviora.preferences.mouseTouch !== false
                onToggled: oviora.setPreference("mouseTouch", checked)
            }
        }
        SettingRow {
            title: qsTr("Tuşları değiştirmek")
            help: page.changedCount > 0
                ? qsTr("%1 tuşu değiştirdin. Bir tuşu değiştirmek için yanındaki Değiştir'e bas, sonra klavyede istediğin tuşa bas. Esc vazgeçer.").arg(page.changedCount)
                : qsTr("Bir tuşu değiştirmek için yanındaki Değiştir'e bas, sonra klavyede istediğin tuşa bas. Esc vazgeçer.")
            PillButton {
                text: qsTr("Tümünü varsayılana döndür")
                enabled: page.changedCount > 0
                onClicked: page.confirmRequested(qsTr("Tuşları sıfırla"),
                    qsTr("Bütün tuşlar ilk hallerine dönsün mü?"),
                    () => { page.pending = null; page.capturing = -1; oviora.setPreference("keyMap", {}); })
            }
        }
    }

    // ── Mouse aim: the mouse turns the right stick ──
    // Old mouse_aim/* (bridge/MouseAim.cpp); the stored map over the old defaults
    readonly property var mouseAimDefaults: ({ enabled: false, sensitivity: 50, accel: 30, invert_y: false, left: 1, right: 2, middle: 5,
                                               x1: 0, x2: 0, wheel_up: 0, wheel_down: 0, ads: 100, deadzone: 18, toggle_key: Qt.Key_F8 })
    readonly property var mouseAim: Object.assign({}, mouseAimDefaults, oviora.preferences.mouseAim || {})
    readonly property var mouseAimActions: [qsTr("Hiçbiri"), "R2", "L2", "R1", "L1", "R3", "L3", qsTr("Kare"), qsTr("Çarpı"), qsTr("Daire"), qsTr("Üçgen"),
                                            qsTr("Yukarı"), qsTr("Aşağı"), qsTr("Sol"), qsTr("Sağ"), qsTr("Dokunmatik yüzey"), "Options"]
    // Ready-made layouts (the buttons only; sensitivity and the rest stay): R2 1, L2 2, R1 3, L1 4, R3 5, L3 6, Kare 7,
    // Çarpı 8, Daire 9, Üçgen 10, Yukarı 11, Aşağı 12, Sol 13, Sağ 14
    readonly property var mouseAimLayouts: [
        [qsTr("Nişancı"), { left: 1, right: 2, middle: 5, x1: 4, x2: 3, wheel_up: 10, wheel_down: 10, ads: 70, deadzone: 18 }],
        [qsTr("Aksiyon / TPS"), { left: 1, right: 2, middle: 5, x1: 7, x2: 9, wheel_up: 11, wheel_down: 12, ads: 80, deadzone: 16 }],
        [qsTr("Yarış"), { left: 1, right: 2, middle: 0, x1: 4, x2: 3, wheel_up: 0, wheel_down: 0, ads: 100, deadzone: 12 }]
    ]
    function applyLayout(values) {
        const next = Object.assign({}, page.mouseAim, values)
        oviora.setPreference("mouseAim", next)
    }
    readonly property var mouseAimToggleKeys: [Qt.Key_F6, Qt.Key_F7, Qt.Key_F8, Qt.Key_F9, Qt.Key_F10, Qt.Key_ScrollLock, Qt.Key_Pause]
    readonly property var mouseAimToggleNames: ["F6", "F7", "F8", "F9", "F10", "Scroll Lock", "Pause"]
    function setAim(key, value) {
        const next = Object.assign({}, page.mouseAim)
        next[key] = value
        oviora.setPreference("mouseAim", next)
    }

    SettingsCard {
        enabled: oviora.preferences.keyboardMouse !== false
        title: qsTr("Fareyle nişan alma")
        description: qsTr("Nişancı oyunları için: yayında fareyi oynatınca sağ çubuk döner, kamera fareyi izler. İmleç pencerede tutulur ve görünmez.")

        SettingRow {
            title: qsTr("Fareyle nişan al")
            help: qsTr("Açıkken yayın başlayınca imleç yakalanır. %1 imleci serbest bırakır ya da geri yakalar; menü açılınca da serbest kalır.")
                .arg(page.mouseAimToggleNames[Math.max(0, page.mouseAimToggleKeys.indexOf(page.mouseAim.toggle_key))])
            Toggle {
                checked: !!page.mouseAim.enabled
                onToggled: page.setAim("enabled", checked)
            }
        }
        SettingRow {
            title: qsTr("Hassasiyet")
            help: qsTr("Yüksek değerde fareyi az oynatınca kamera hızlı döner.")
            enabled: !!page.mouseAim.enabled
            ValueSlider {
                implicitWidth: 320
                valueWidth: 52
                from: 1
                to: 100
                stepSize: 1
                value: page.mouseAim.sensitivity
                onMoved: page.setAim("sensitivity", Math.round(value))
            }
        }
        SettingRow {
            title: qsTr("Nişan alırken hassasiyet")
            help: qsTr("L2 basılıyken (nişan alırken) hassasiyet bu orana iner; uzak hedeflere ince ayar için.")
            enabled: !!page.mouseAim.enabled
            ValueSlider {
                implicitWidth: 320
                valueWidth: 52
                from: 20
                to: 100
                stepSize: 5
                value: page.mouseAim.ads
                valueText: "%" + Math.round(value)
                onMoved: page.setAim("ads", Math.round(value))
            }
        }
        SettingRow {
            title: qsTr("Ölü bölge telafisi")
            help: qsTr("Oyunlar çubuğun ortasındaki küçük hareketleri yok sayar. Fare yavaşken kamera hiç dönmüyorsa artır, çok seğiriyorsa azalt.")
            enabled: !!page.mouseAim.enabled
            ValueSlider {
                implicitWidth: 320
                valueWidth: 52
                from: 0
                to: 35
                stepSize: 1
                value: page.mouseAim.deadzone
                valueText: "%" + Math.round(value)
                onMoved: page.setAim("deadzone", Math.round(value))
            }
        }
        SettingRow {
            title: qsTr("Hızlanma")
            help: qsTr("Artırınca yavaş hareketler daha ince, hızlı savuruşlar daha güçlü olur. 0'da hız ile dönüş doğru orantılı.")
            enabled: !!page.mouseAim.enabled
            ValueSlider {
                implicitWidth: 320
                valueWidth: 52
                from: 0
                to: 100
                stepSize: 5
                value: page.mouseAim.accel
                onMoved: page.setAim("accel", Math.round(value))
            }
        }
        SettingRow {
            title: qsTr("Dikeyi ters çevir")
            help: qsTr("Fareyi yukarı itince kamera aşağı baksın.")
            enabled: !!page.mouseAim.enabled
            Toggle {
                checked: !!page.mouseAim.invert_y
                onToggled: page.setAim("invert_y", checked)
            }
        }
        SettingRow {
            title: qsTr("Fare tuşları")
            help: qsTr("Farenin tuşları, yan tuşları ve tekerleği hangi kumanda tuşu olsun. Hazır düzenlerden biriyle başlayabilirsin.")
            enabled: !!page.mouseAim.enabled
            stacked: true
            ColumnLayout {
            spacing: 12
            RowLayout {
                spacing: 8
                Repeater {
                    model: page.mouseAimLayouts
                    delegate: PillButton {
                        compact: true
                        required property var modelData
                        text: modelData[0]
                        onClicked: page.applyLayout(modelData[1])
                    }
                }
            }
            GridLayout {
                columns: 4
                columnSpacing: 12
                rowSpacing: 8
                Repeater {
                    model: [["left", qsTr("Sol tık")], ["right", qsTr("Sağ tık")], ["middle", qsTr("Orta tık")], ["x1", qsTr("Yan tuş (geri)")],
                            ["x2", qsTr("Yan tuş (ileri)")], ["wheel_up", qsTr("Tekerlek yukarı")], ["wheel_down", qsTr("Tekerlek aşağı")]]
                    delegate: ColumnLayout {
                        required property var modelData
                        spacing: 4
                        Label {
                            textFormat: Text.PlainText
                            text: modelData[1]
                            font.family: Tema.body
                            font.pixelSize: 13
                            color: Tema.muted
                        }
                        Dropdown {
                            implicitWidth: 150
                            model: page.mouseAimActions
                            currentIndex: page.mouseAim[modelData[0]] || 0
                            onActivated: (index) => page.setAim(modelData[0], index)
                        }
                    }
                }
            }
            }
        }
        SettingRow {
            title: qsTr("İmleci bırakma tuşu")
            help: qsTr("Yayında bu tuş konsola gitmez.")
            enabled: !!page.mouseAim.enabled
            Dropdown {
                implicitWidth: 180
                model: page.mouseAimToggleNames
                currentIndex: Math.max(0, page.mouseAimToggleKeys.indexOf(page.mouseAim.toggle_key))
                onActivated: (index) => page.setAim("toggle_key", page.mouseAimToggleKeys[index])
            }
        }
    }

    Notice {
        width: parent.width
        visible: page.duplicateCount > 0
        tone: "warn"
        text: page.duplicateCount === 1
            ? qsTr("Bir klavye tuşu iki PlayStation tuşuna atanmış. Yayında bu tuş yalnızca birinde çalışır; turuncu işaretli satırlardan birini değiştir.")
            : qsTr("%1 klavye tuşu birden fazla PlayStation tuşuna atanmış. Yayında her biri yalnızca bir yerde çalışır; turuncu işaretli satırlara bak.").arg(page.duplicateCount)
    }

    GridLayout {
        id: groups
        width: parent.width
        columns: width >= 980 ? 2 : 1
        columnSpacing: 18
        rowSpacing: 18

        ColumnLayout {
            Layout.alignment: Qt.AlignTop
            Layout.fillWidth: true
            Layout.preferredWidth: groups.columns === 2 ? (groups.width - groups.columnSpacing) / 2 : groups.width
            spacing: 18
            Repeater {
                model: [
                    [qsTr("Yön tuşları"), qsTr("Kumandanın solundaki dört yön."), [64, 128, 16, 32]],
                    [qsTr("Eylem tuşları"), qsTr("Sağdaki dört simgeli tuş."), [1, 2, 4, 8]],
                    [qsTr("Omuz ve tetik tuşları"), qsTr("Üstteki L ve R tuşları."), [256, 65536, 512, 131072]]
                ]
                delegate: groupDelegate
            }
        }
        ColumnLayout {
            Layout.alignment: Qt.AlignTop
            Layout.fillWidth: true
            Layout.preferredWidth: groups.columns === 2 ? (groups.width - groups.columnSpacing) / 2 : groups.width
            spacing: 18
            Repeater {
                model: [
                    [qsTr("Sol çubuk"), qsTr("Yürümek ve menülerde gezinmek için."), [1048576, 2097152, 524288, 262144, 1024]],
                    [qsTr("Sağ çubuk"), qsTr("Genelde kamerayı çevirir."), [16777216, 33554432, 8388608, 4194304, 2048]],
                    [qsTr("Sistem tuşları"), qsTr("Menü, paylaşım ve PS düğmesi."), [4096, 8192, 16384, 32768]]
                ]
                delegate: groupDelegate
            }
        }
    }

    Component {
        id: groupDelegate
        SettingsCard {
            required property var modelData
            Layout.fillWidth: true
            title: modelData[0]
            description: modelData[1]
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 2
                Repeater {
                    model: modelData[2]
                    delegate: KeyRow {
                        required property var modelData
                        button: modelData
                    }
                }
            }
        }
    }
}
