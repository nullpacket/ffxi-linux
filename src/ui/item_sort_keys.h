// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace xi::ui
{
// Moving between the Items list ("menu inventor") and its sort window ("menu itmsortw" / "menu itmsort2") (#117;
// docs/ui-format-notes.md §10.12). The sort window opens with the list (0x101f01b0); the keys only move the focus:
//   list handler 0x101f0530: event 8 (Select Window: Num +, Compact F) focuses the sort window (0x1015da90 on
//     0x10630c28 / 0x10630c2c); 5 confirm uses the item, 6 cancel closes the list. No arrow key goes to the window.
//   sort window handler 0x101f3600: events 6 (cancel) and 8 (Select Window) both focus the list again (0x101f32f0).
// Manual Sort: cancel in the list ends it (as 0x101f06e0 drops its pick on cancel); Select Window during it going to
// the sort window and ending it is INFERRED.
enum class ItemSortKey : uint8_t
{
    SelectWindow,
    Cancel,
    Left,
    Right,
};

enum class ItemSortMove : uint8_t
{
    None,          // the key does its usual thing (Left / Right page or move the cursor, cancel closes the list)
    ToSortWindow,  // focus the sort window
    ToList,        // focus the list again
    EndManualSort, // stop Manual Sort, staying in the list
};

constexpr ItemSortMove itemSortMove(bool inSortWindow, bool manualSort, ItemSortKey key)
{
    if (inSortWindow)
    {
        return key == ItemSortKey::SelectWindow || key == ItemSortKey::Cancel ? ItemSortMove::ToList
                                                                              : ItemSortMove::None;
    }
    if (key == ItemSortKey::SelectWindow)
    {
        return ItemSortMove::ToSortWindow;
    }
    if (manualSort && key == ItemSortKey::Cancel)
    {
        return ItemSortMove::EndManualSort;
    }
    return ItemSortMove::None;
}

} // namespace xi::ui
