// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace xi::ui
{
// The Items window's "held/capacity" count in the title window beside "Items" (#107; docs/ui-format-notes.md §10.7).
// The game titles its counted lists "%s          %d/%d" (0x103869e4: the title, ten spaces, the count; the quest list
// 0x102334d0 / 0x10231b00, drawn 0x80808080 = plain white). That the inventory uses the same pattern is INFERRED from
// retail captures; its own draw was not traced.

// The count: items held (gil not counted) over the inventory's slots. S2C 0x01C sends ItemNum = 1 + the slots (slot 0
// holds the gil); before it arrives only the held count is known, so nothing is shown.
inline std::string itemCountText(std::size_t held, uint8_t itemNum, bool sizeKnown)
{
    if (!sizeKnown || itemNum == 0)
    {
        return {};
    }
    return fmt::format("{}/{}", held, itemNum - 1);
}

// The count's x in the title window (window units): after the title and the ten spaces' gap, pulled left so it ends
// 4 inside the window when that would run past its right edge, never over the title itself.
inline float itemCountX(float titleWidth, float gapWidth, float countWidth, float windowWidth)
{
    constexpr float kTextX = 4; // the title's own x in "menu titlewin"
    const float     after  = kTextX + titleWidth;
    return std::max(after + 4, std::min(after + gapWidth, windowWidth - kTextX - countWidth));
}
} // namespace xi::ui
