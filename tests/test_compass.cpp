// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/compass.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// The compass and the Vana'diel clock above the log window (#120).
using namespace xi::ui;
using Catch::Approx;

namespace
{
constexpr uint32_t kDay = 3456; // Earth seconds a Vana'diel day; 144 an hour
} // namespace

TEST_CASE("the compass sits above log window 1", "[compass]")
{
    const auto at = compassPlace(298);
    CHECK(at.x == 88.0f);
    CHECK(at.y == 256.0f); // 0x2A above the log's top
}

TEST_CASE("the clock line reads hour, minute and the map square", "[compass]")
{
    // Day 0 of the epoch is a Firesday; 13:30 is 13 * 144 + 30 * 144 / 60 seconds in.
    const uint32_t t = 13 * 144 + 72;
    CHECK(compassClockText(t, "(H-8)") == "13:30  (H-8)");
    CHECK(compassClockText(0, "(?-?)") == "0:00  (?-?)"); // the hour is not padded, the minutes are
    CHECK(compassClockText(23 * 144 + 143, "") == "23:59  ");
}

TEST_CASE("the day's icon follows the weekday", "[compass]")
{
    // Firesday .. Darksday: "menu frames" 106 fire, 109 earth, 111 water, 108 wind, 107 ice, 110 thunder, 112 light,
    // 113 dark (0x10385EE8).
    const uint16_t want[8] = { 106, 109, 111, 108, 107, 110, 112, 113 };
    for (uint32_t d = 0; d < 8; ++d)
    {
        CHECK(compassDaySprite(d * kDay + 100) == want[d]);
    }
    CHECK(compassDaySprite(8 * kDay) == 106); // the week wraps
}

TEST_CASE("the weather icon is the element's glyph, doubled when strong", "[compass]")
{
    CHECK_FALSE(compassWeather(0));  // clear
    CHECK_FALSE(compassWeather(2));  // clouds
    CHECK_FALSE(compassWeather(3));  // fog
    CHECK_FALSE(compassWeather(20)); // out of the table
    const auto hot = compassWeather(4);
    REQUIRE(hot);
    CHECK(hot->glyph == 0); // fire
    CHECK_FALSE(hot->strong);
    const auto squall = compassWeather(7);
    REQUIRE(squall);
    CHECK(squall->glyph == 5); // water
    CHECK(squall->strong);
    CHECK(compassWeather(12)->glyph == 1); // snow: ice
    CHECK(compassWeather(14)->glyph == 4); // thunder
    CHECK(compassWeather(19)->glyph == 7); // darkness
}

TEST_CASE("the radar's reach depends on the job", "[compass]")
{
    const auto war = compassRange(1, 2, true);
    CHECK(war.range == Approx(28.8f));
    CHECK(war.size == Approx(0.8f));
    CHECK_FALSE(war.monsters);
    const auto nin = compassRange(13, 0, false);
    CHECK(nin.range == Approx(36.0f));
    CHECK(nin.size == Approx(1.0f));
    CHECK(nin.monsters);
    CHECK(compassRange(1, 13, true).range == Approx(36.0f)); // Ninja support
    CHECK(compassRange(1, 6, true).monsters);                // Thief support, active
    CHECK_FALSE(compassRange(1, 6, false).monsters);         // ... not while the support job is off
    CHECK(compassRange(9, 0, false).monsters);               // Beastmaster
}

TEST_CASE("the dial keeps north where the camera says it is", "[compass]")
{
    // Facing north: north up (tilted), east right.
    auto t = compassTurn(0, 1);
    auto n = compassWorld(0, 10, t, 1);
    CHECK(n.x == Approx(0).margin(1e-4));
    CHECK(n.y == Approx(-10 * kCompassTilt));
    auto e = compassWorld(10, 0, t, 1);
    CHECK(e.x == Approx(10));
    CHECK(e.y == Approx(0).margin(1e-4));
    // Facing east: north on the left, east at the top.
    t = compassTurn(1, 0);
    n = compassWorld(0, 10, t, 1);
    CHECK(n.x == Approx(-10));
    CHECK(n.y == Approx(0).margin(1e-4));
    e = compassWorld(10, 0, t, 1);
    CHECK(e.x == Approx(0).margin(1e-4));
    CHECK(e.y == Approx(-10 * kCompassTilt));
    // Facing south: north at the bottom.
    t = compassTurn(0, -1);
    n = compassWorld(0, 10, t, 1);
    CHECK(n.y == Approx(10 * kCompassTilt));
    // The fade-in's half turn flips it.
    t = compassTurn(0, 1, 3.14159265f);
    n = compassWorld(0, 10, t, 1);
    CHECK(n.y == Approx(10 * kCompassTilt).margin(1e-3));
}

TEST_CASE("dots fade with distance and height", "[compass]")
{
    const auto r = compassRange(1, 0, false); // 28.8, inner 14.4
    CHECK(compassDotAlpha(5, 0, r, 0x80) == 0x80);
    CHECK(compassDotAlpha(14.4f, 0, r, 0x80) == 0x80);
    CHECK(compassDotAlpha(27, 0, compassRange(13, 0, false), 0x80) == 0x40); // half way out (Ninja: 18 .. 36)
    CHECK(compassDotAlpha(28.9f, 0, r, 0x80) == 0);
    CHECK(compassDotAlpha(5, 3, r, 0x80) == 0x40);  // 1 yalm past the 2 free
    CHECK(compassDotAlpha(5, -3, r, 0x80) == 0x40); // below as above
    CHECK(compassDotAlpha(5, 4.5f, r, 0x80) == 0);  // beyond 4: not shown
}

TEST_CASE("dot colours by kind", "[compass]")
{
    CHECK(compassDotColour(CompassDot::Player) == 0x0040F0);
    CHECK(compassDotColour(CompassDot::Ally) == 0x3060E0);
    CHECK(compassDotColour(CompassDot::Npc) == 0x00B040);
    CHECK(compassDotColour(CompassDot::Monster) == 0x808080);
    CHECK(compassDotColour(CompassDot::DeadMonster) == 0x206060);
}

TEST_CASE("no dial on ships and airships", "[compass]")
{
    CHECK(compassHiddenIn(3));    // Manaclipper
    CHECK(compassHiddenIn(0xdc)); // Ship bound for Selbina
    CHECK(compassHiddenIn(0xe4));
    CHECK_FALSE(compassHiddenIn(100));  // West Ronfaure
    CHECK_FALSE(compassHiddenIn(0xde)); // 222 is not in the list
}
