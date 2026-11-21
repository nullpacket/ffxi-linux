// SPDX-License-Identifier: GPL-3.0-or-later
// The game's log objects as addons see them (src/ui/log_object.h, issue #91).
#include "ui/log_object.h"

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

TEST_CASE("log objects: lines, window height and text top", "[ui][log]")
{
    using namespace xi::ui;
    CHECK(logLinesForHeight(0) == 0);
    CHECK(logLinesForHeight(-16) == 0);
    CHECK(logLinesForHeight(1) == 1);
    CHECK(logLinesForHeight(16) == 1);
    CHECK(logLinesForHeight(17) == 2);
    CHECK(logLinesForHeight(128) == 8);
    CHECK(logWindowHeight(8) == 134); // "menu logwindo"'s authored height
    CHECK(logWindowHeight(0) == 6);
    CHECK(logTextTopFromBottom(0) == 19); // fancychat's "UISizeY - 19": the log shrunk away
    CHECK(logTextTopFromBottom(8) == 147);
}

TEST_CASE("log objects: an addon's hold on the shown height", "[ui][log]")
{
    using xi::ui::LogHold;
    const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
    LogHold    h;
    CHECK(h.shown(128, t0) == 128); // nothing held: the client's own
    h.write(0, t0);
    CHECK(h.shown(128, t0 + 16ms) == 0);
    // The client's value matches the addon's: kept, however long.
    CHECK(h.shown(0, t0 + 10s) == 0);
    // A new line grows the client's log: still the addon's while it keeps writing...
    CHECK(h.shown(144, t0 + 10s + 16ms) == 0);
    h.write(0, t0 + 10s + 32ms);
    CHECK(h.shown(144, t0 + 10s + 48ms) == 0);
    // ...and the client's own once the writes stop.
    CHECK(h.shown(144, t0 + 11s) == 144);
    CHECK(h.shown(144, t0 + 11s + 16ms) == 144);
    // Released (the addon unloaded): the client's own at once.
    h.write(0, t0 + 12s);
    h.release();
    CHECK(h.shown(144, t0 + 12s) == 144);
}
