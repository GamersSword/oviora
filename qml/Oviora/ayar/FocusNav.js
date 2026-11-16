// Yakamoz settings: arrow-key movement between the controls of one settings page.
// Up/Down go to the previous/next focusable control, but never out of the page
// (the home screen behind the panel also has focusable items).

function pageOf(item) {
    let p = item;
    while (p) {
        if (p.isSettingsPage === true)
            return p;
        p = p.parent;
    }
    return null;
}

function contains(outer, item) {
    let p = item;
    while (p) {
        if (p === outer)
            return true;
        p = p.parent;
    }
    return false;
}

function firstIn(page) {
    const first = page.nextItemInFocusChain(true);
    return first && contains(page, first) ? first : null;
}

function lastIn(page) {
    let it = firstIn(page);
    let guard = 0;
    while (it && guard++ < 1000) {
        const n = it.nextItemInFocusChain(true);
        if (!n || n === it || !contains(page, n) || n === firstIn(page))
            return it;
        it = n;
    }
    return it;
}

// Moves the focus one control forward/back. At the end of the page it scrolls to the edge instead.
function move(item, forward) {
    const page = pageOf(item);
    let next = item.nextItemInFocusChain(forward);
    // A control may hold focusable parts of its own (e.g. a key row): skip what is inside it
    let guard = 0;
    while (next && next !== item && contains(item, next) && guard++ < 20)
        next = next.nextItemInFocusChain(forward);
    if (next && next !== item && (!page || contains(page, next))) {
        // Wrapping around from the last control to the first (or back) is not "next"
        if (page && next === (forward ? firstIn(page) : lastIn(page))) {
            page.scrollToEdge(forward);
            return;
        }
        next.forceActiveFocus(Qt.TabFocusReason);
        return;
    }
    if (page)
        page.scrollToEdge(forward);
}

// Common key handling for a control: Up/Down move between controls
function handleUpDown(item, event) {
    if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier))
        return false;
    if (event.key === Qt.Key_Up) {
        move(item, false);
        event.accepted = true;
        return true;
    }
    if (event.key === Qt.Key_Down) {
        move(item, true);
        event.accepted = true;
        return true;
    }
    return false;
}
