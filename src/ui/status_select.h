// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Select Window from the field (#122; docs/ui-format-notes.md §9.5). With no menu up the key steps a counter
// (CODE 0x10157e49, +0x68 of the field input object): 1 selects the log window ("menu logwindo", global 0x105eee1c),
// 2 our status icons ("menu buff", 0x10634698), 0 neither (0x1015e760); Cancel (key 0x0F) sets it back to 0
// (0x10157e9e). In the icons, Confirm on one with the "can cancel" flag sends C2S 0x0F1 at once (0x10227a10 ->
// 0x100a04c0); there is no Yes/No.

#include <algorithm>

namespace xi::ui
{

enum class FieldSelect
{
    None,
    Log,
    Buffs,
};

[[nodiscard]] inline FieldSelect nextFieldSelect(FieldSelect s)
{
    switch (s)
    {
        case FieldSelect::None: return FieldSelect::Log;
        case FieldSelect::Log: return FieldSelect::Buffs;
        case FieldSelect::Buffs: return FieldSelect::None;
    }
    return FieldSelect::None;
}

// The icon cursor: Left / Right step one icon, Up / Down a row of `perRow` ("menu buff" lays them out nine to a row);
// clamped to the icons there are (INFERRED: the list base 0x10221cb0 was not followed for this window).
enum class BuffKey
{
    Left,
    Right,
    Up,
    Down,
};

[[nodiscard]] inline int moveBuffCursor(int cursor, int count, BuffKey key, int perRow = 9)
{
    if (count <= 0)
    {
        return 0;
    }
    cursor = std::clamp(cursor, 0, count - 1);
    switch (key)
    {
        case BuffKey::Left: return std::max(0, cursor - 1);
        case BuffKey::Right: return std::min(count - 1, cursor + 1);
        case BuffKey::Up: return cursor >= perRow ? cursor - perRow : cursor;
        case BuffKey::Down: return cursor + perRow < count ? cursor + perRow : cursor;
    }
    return cursor;
}

} // namespace xi::ui
