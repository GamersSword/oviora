import QtQuick
import QtQuick.Templates as T
import Oviora

// One settings section: a scrolling column of setting groups, left aligned under the section title
Flickable {
    id: page
    readonly property bool isSettingsPage: true
    default property alias content: column.data
    property int maxContentWidth: 900
    property alias spacing: column.spacing

    function scrollToEdge(bottom) {
        contentY = bottom ? Math.max(0, contentHeight - height) : 0;
    }
    function firstFocusable() {
        const item = nextItemInFocusChain(true);
        // A hidden page's chain comes back to the page itself
        if (item === page)
            return null;
        let p = item;
        while (p) {
            if (p === page)
                return item;
            p = p.parent;
        }
        return null;
    }
    // Keeps an item (or the setting row around it) in view
    function ensureVisible(item) {
        if (!item || item === page)
            return;
        // The first control: show the page from its top, card heading included
        if (item === firstFocusable()) {
            contentY = 0;
            return;
        }
        let target = item;
        for (let p = item; p && p !== page; p = p.parent) {
            if (p.isSettingRow === true) {
                target = p;
                break;
            }
        }
        const top = target.mapToItem(contentItem, 0, 0).y - 16;
        const bottom = top + target.height + 32;
        if (top < contentY)
            contentY = Math.max(0, top);
        else if (bottom > contentY + height)
            contentY = Math.min(Math.max(0, contentHeight - height), bottom - height);
    }

    clip: true
    contentWidth: width
    contentHeight: column.implicitHeight + 56
    flickableDirection: Flickable.VerticalFlick
    boundsBehavior: Flickable.StopAtBounds
    Behavior on contentY {
        enabled: !page.moving && !page.dragging
        NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
    }

    T.ScrollBar.vertical: T.ScrollBar {
        id: bar
        policy: T.ScrollBar.AsNeeded
        visible: page.contentHeight > page.height + 1
        padding: 3
        implicitWidth: 12
        contentItem: Rectangle {
            implicitWidth: 6
            radius: 3
            color: bar.pressed ? Tema.muted : Tema.lineStrong
            opacity: bar.active || bar.hovered ? 1 : 0.55
        }
    }

    // Centred in a wide page (the form screens); the settings panel keeps it at the left
    property bool centered: false

    Column {
        id: column
        x: page.centered ? Math.max(24, Math.round((page.width - width) / 2)) : 24
        y: 28
        width: Math.min(page.width - 60, page.maxContentWidth)
        spacing: 44
    }
}
