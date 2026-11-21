// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/list_cursor.h"

#include <catch2/catch_test_macros.hpp>

using xi::ui::listCursorStep;

TEST_CASE("item lists stop at the ends (#124, 0x101f6820)", "[ui][list]")
{
    // Up on the first item and Down on the last stay put.
    CHECK(listCursorStep(0, 30, -1) == 0);
    CHECK(listCursorStep(29, 30, 1) == 29);
    // In between they move one row.
    CHECK(listCursorStep(5, 30, -1) == 4);
    CHECK(listCursorStep(5, 30, 1) == 6);
    // Page steps clamp the same way.
    CHECK(listCursorStep(3, 30, -10) == 0);
    CHECK(listCursorStep(25, 30, 10) == 29);
    CHECK(listCursorStep(10, 30, 10) == 20);
    // A short list and an empty one.
    CHECK(listCursorStep(1, 2, 1) == 1);
    CHECK(listCursorStep(0, 1, -1) == 0);
    CHECK(listCursorStep(0, 0, 1) == 0);
}

TEST_CASE("the event menu keeps wrapping", "[ui][list]")
{
    CHECK(listCursorStep(0, 4, -1, true) == 3);
    CHECK(listCursorStep(3, 4, 1, true) == 0);
    CHECK(listCursorStep(1, 4, 1, true) == 2);
    // Page steps never wrap.
    CHECK(listCursorStep(3, 4, 10, true) == 3);
}

TEST_CASE("Right pages down and Left pages up in item lists (#121, events 3 / 4)", "[ui][list]")
{
    using xi::ui::ListKey;
    using xi::ui::listKeyStep;
    CHECK(listKeyStep(ListKey::Right, 10) == 10);
    CHECK(listKeyStep(ListKey::Left, 10) == -10);
    CHECK(listKeyStep(ListKey::PageDown, 10) == 10);
    CHECK(listKeyStep(ListKey::PageUp, 10) == -10);
    CHECK(listKeyStep(ListKey::Down, 10) == 1);
    CHECK(listKeyStep(ListKey::Up, 10) == -1);
    // On a 30-item list with 10 rows a page: Right from the top goes to row 11, Left from there back to the top,
    // and both stop at the ends.
    CHECK(listCursorStep(0, 30, listKeyStep(ListKey::Right, 10)) == 10);
    CHECK(listCursorStep(10, 30, listKeyStep(ListKey::Left, 10)) == 0);
    CHECK(listCursorStep(25, 30, listKeyStep(ListKey::Right, 10)) == 29);
    CHECK(listCursorStep(4, 30, listKeyStep(ListKey::Left, 10)) == 0);
}
