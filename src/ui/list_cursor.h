// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

namespace xi::ui
{
// The scrolled item lists (base class ctor 0x101f6370, key handler 0x101f6820; Items, Equipment slot lists, trade,
// storage, shops, search results ... #124, docs/ui-format-notes.md §10.14) stop at both ends: Up on the first row of
// the first page scrolls no further (event 2, 0x101f6888: scroll +0x1e clamped at 0, no cursor sound), Down on the
// last item stays (event 1, 0x101f694b: scroll clamped at +0x22; a short list puts the cursor back on its last row,
// 0x101f69a6). Page steps clamp the same way (events 3 / 4). Nothing wraps.
// `index` is the 0-based row in the whole list, `count` the rows, `step` -1 / +1 for Up / Down or -page / +page.
// `wrap` keeps the old wrap-around for lists not drawn by that class (the event menu, INFERRED).
constexpr int listCursorStep(int index, int count, int step, bool wrap = false)
{
    if (count <= 0)
    {
        return 0;
    }
    const int i = index + step;
    if (wrap && (step == 1 || step == -1))
    {
        return i < 0 ? count - 1 : i >= count ? 0 : i;
    }
    return std::clamp(i, 0, count - 1);
}

// The keys of a scrolled list, as the list class sees them: events 1 / 2 Down / Up, 3 / 4 Right / Left. The layout
// code reads an element's navigation byte per event (0x1011b150: event 1 +0x18, 2 +0x17, 3 +0x19, 4 +0x1a), and the
// "menu equip" grid puts the element to the right in +0x19 and the one to the left in +0x1a, so event 3 is Right and
// 4 is Left. In the list class event 3 adds a page to the scroll (0x101f6a0e) and event 4 takes one away (0x101f68d7):
// Right pages down, Left pages up (#121, docs/ui-format-notes.md §10.14). Page Down / Page Up are this client's extra
// keys for the same steps.
enum class ListKey
{
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
};

// The `step` listCursorStep takes for a key, `page` being the rows on a page.
constexpr int listKeyStep(ListKey key, int page)
{
    switch (key)
    {
        case ListKey::Up: return -1;
        case ListKey::Down: return 1;
        case ListKey::Left:
        case ListKey::PageUp: return -page;
        case ListKey::Right:
        case ListKey::PageDown: return page;
    }
    return 0;
}
} // namespace xi::ui
