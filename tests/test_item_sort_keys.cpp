// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/item_sort_keys.h"
#include "ui/keymap.h"

#include <catch2/catch_test_macros.hpp>

using namespace xi::ui;

TEST_CASE("item sort keys: Select Window moves between the Items list and its sort window", "[ui][items]")
{
    // List handler 0x101f0530 event 8 -> the sort window; sort window handler 0x101f3600 events 6 / 8 -> the list.
    CHECK(itemSortMove(false, false, ItemSortKey::SelectWindow) == ItemSortMove::ToSortWindow);
    CHECK(itemSortMove(true, false, ItemSortKey::SelectWindow) == ItemSortMove::ToList);
    CHECK(itemSortMove(true, false, ItemSortKey::Cancel) == ItemSortMove::ToList);
    // Cancel in the list is the list's own (closes it), unless Manual Sort is on.
    CHECK(itemSortMove(false, false, ItemSortKey::Cancel) == ItemSortMove::None);
    CHECK(itemSortMove(false, true, ItemSortKey::Cancel) == ItemSortMove::EndManualSort);
    CHECK(itemSortMove(false, true, ItemSortKey::SelectWindow) == ItemSortMove::ToSortWindow);
}

TEST_CASE("item sort keys: the arrow keys never open or leave the sort window", "[ui][items]")
{
    for (const bool inSort : { false, true })
    {
        for (const bool manual : { false, true })
        {
            CHECK(itemSortMove(inSort, manual, ItemSortKey::Left) == ItemSortMove::None);
            CHECK(itemSortMove(inSort, manual, ItemSortKey::Right) == ItemSortMove::None);
        }
    }
}

TEST_CASE("item sort keys: Num + is Select Window in both layouts", "[ui][items][keymap]")
{
    for (const auto layout : { KeyLayout::Normal, KeyLayout::Compact })
    {
        const Keymap km(layout);
        CHECK(km.pressed(SDL_SCANCODE_KP_PLUS, SDL_KMOD_NONE, true) == KeyAction::SelectWindow);
    }
}
