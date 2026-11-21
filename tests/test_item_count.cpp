// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/item_count.h"

#include <catch2/catch_test_macros.hpp>

// The Items window's count beside its title (#107).
using namespace xi::ui;

TEST_CASE("the item count is held over the inventory's slots", "[item-count]")
{
    CHECK(itemCountText(28, 31, true) == "28/30"); // ItemNum is 1 + the slots (slot 0 = gil)
    CHECK(itemCountText(0, 81, true) == "0/80");
    CHECK(itemCountText(80, 81, true) == "80/80");
    CHECK(itemCountText(5, 31, false).empty()); // before S2C 0x01C
    CHECK(itemCountText(5, 0, true).empty());
}

TEST_CASE("the count sits after the title, inside the title window", "[item-count]")
{
    // "Items" (30 wide) + a 40-wide gap: 4 + 30 + 40.
    CHECK(itemCountX(30, 40, 30, 112) == 74.0f);
    // Too wide for the gap: pulled left to end 4 inside the 112-wide window.
    CHECK(itemCountX(30, 40, 50, 112) == 58.0f);
    // Never over the title: at least 4 after it.
    CHECK(itemCountX(80, 40, 60, 112) == 88.0f);
}
