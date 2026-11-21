// SPDX-License-Identifier: GPL-3.0-or-later
#include "map/map_client.h"

#include <catch2/catch_test_macros.hpp>

// The game's C2S send cadence (CODE 0x100fa920; docs/lsb-packet-timing.md §3).
using std::chrono::milliseconds;

TEST_CASE("the default send interval is the game's /pcnt default", "[pacing]")
{
    CHECK(xi::map::kDefaultSendInterval == milliseconds(400));
}

TEST_CASE("unacknowledged datagrams back the interval off 50 ms, capped at base + 3000", "[pacing]")
{
    const milliseconds base(400);
    CHECK(xi::map::nextSendInterval(base, base, 2, true) == milliseconds(450));
    CHECK(xi::map::nextSendInterval(base, base, -2, true) == milliseconds(450));
    CHECK(xi::map::nextSendInterval(milliseconds(3380), base, 5, true) == milliseconds(3400));
    CHECK(xi::map::nextSendInterval(milliseconds(3400), base, 5, true) == milliseconds(3400));
}

TEST_CASE("in step, the interval falls back toward the base one base step at a time", "[pacing]")
{
    const milliseconds base(400);
    // The session's first interval is 5 x /pcnt (CODE 0x100f9de0): 2000 -> 1600 -> 1200 -> 800 -> 400.
    milliseconds iv(2000);
    for (const int want : { 1600, 1200, 800, 400, 400 })
    {
        iv = xi::map::nextSendInterval(iv, base, 1, true);
        CHECK(iv == milliseconds(want));
    }
    CHECK(xi::map::nextSendInterval(milliseconds(650), base, 0, true) == base);
}

TEST_CASE("the interval only changes while in zone", "[pacing]")
{
    const milliseconds base(400);
    CHECK(xi::map::nextSendInterval(milliseconds(2000), base, 0, false) == milliseconds(2000));
    CHECK(xi::map::nextSendInterval(base, base, 9, false) == base);
}

// #97: player commands go out at once; after one, a quick window pulls the server's result back sooner. Positions stay
// on the timer.
TEST_CASE("a queued command is sent on the same tick, not at the next timer send", "[pacing]")
{
    using xi::map::SendKind;
    const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    const milliseconds iv(400);
    // The timer sent 150 ms ago: nothing due yet without a command...
    CHECK(xi::map::sendDue(t0, t0 - milliseconds(150), t0 - milliseconds(150), iv, false, {}, true) == SendKind::None);
    // ...but a queued command goes out now, without a position update.
    CHECK(xi::map::sendDue(t0, t0 - milliseconds(150), t0 - milliseconds(150), iv, true, {}, true) == SendKind::Quick);
    CHECK(xi::map::nextSendAt(t0, t0 - milliseconds(150), t0 - milliseconds(150), iv, true, {}, true) == t0);
    // With quick sends off (XI_NET_URGENT=0, the game's own behaviour) it waits for the timer.
    CHECK(xi::map::sendDue(t0, t0 - milliseconds(150), t0 - milliseconds(150), iv, true, {}, false) == SendKind::None);
    CHECK(xi::map::nextSendAt(t0, t0 - milliseconds(150), t0 - milliseconds(150), iv, true, {}, false) == t0 + milliseconds(250));
    // The timer's own send still wins when it is due (it carries the position).
    CHECK(xi::map::sendDue(t0, t0 - iv, t0 - milliseconds(50), iv, true, {}, true) == SendKind::Paced);
}

TEST_CASE("quick sends keep a minimum gap and stop when the window ends", "[pacing]")
{
    using xi::map::SendKind;
    const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    const milliseconds iv(400);
    const auto window = t0 + xi::map::kQuickWindow;
    // 40 ms after the last datagram: a command waits for the gap (at most 100 ms), not the timer.
    CHECK(xi::map::sendDue(t0, t0 - milliseconds(200), t0 - milliseconds(40), iv, true, {}, true) == SendKind::None);
    CHECK(xi::map::nextSendAt(t0, t0 - milliseconds(200), t0 - milliseconds(40), iv, true, {}, true) == t0 + milliseconds(60));
    // Inside the window: a datagram every gap, so the server's reply comes back within ~100 ms of it producing it.
    CHECK(xi::map::sendDue(t0 + milliseconds(100), t0 - milliseconds(200), t0, iv, false, window, true) == SendKind::Quick);
    CHECK(xi::map::sendDue(t0 + milliseconds(50), t0 - milliseconds(200), t0, iv, false, window, true) == SendKind::None);
    // After it: back to the timer only.
    const auto after = window + milliseconds(10);
    CHECK(xi::map::sendDue(after, after - milliseconds(100), after - milliseconds(100), iv, false, window, true) == SendKind::None);
    CHECK(xi::map::nextSendAt(after, after - milliseconds(100), after - milliseconds(100), iv, false, window, true) == after + milliseconds(300));
}

TEST_CASE("the quick window's polls wait for the reply to the last datagram; a command does not", "[pacing]")
{
    using xi::map::SendKind;
    const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    const milliseconds iv(400);
    const auto window = t0 + milliseconds(500);
    CHECK(xi::map::sendDue(t0, t0 - milliseconds(200), t0 - milliseconds(200), iv, false, window, true, true) == SendKind::None);
    CHECK(xi::map::sendDue(t0, t0 - milliseconds(200), t0 - milliseconds(200), iv, false, window, true, false) == SendKind::Quick);
    CHECK(xi::map::sendDue(t0, t0 - milliseconds(200), t0 - milliseconds(200), iv, true, {}, true, true) == SendKind::Quick);
    // The timer's send is not held back by it (the game sends on its timer whatever the replies do).
    CHECK(xi::map::sendDue(t0, t0 - iv, t0 - milliseconds(200), iv, true, {}, true, true) == SendKind::Paced);
}
