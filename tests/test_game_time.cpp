// SPDX-License-Identifier: GPL-3.0-or-later
// The game's clock functions (issue #88; ashita/game_time.h), as FFXiMain.dll computes them. The date / time formats
// come from the user's own install (string table 10, file 50; skipped when FFXI_ROOT is unset or missing).
#include "ashita/game_time.h"
#include "dat/dat_index.h"
#include "dat/messages.h"
#include "xi/install.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace gt = xi::ashita::gametime;

namespace
{

const xi::dat::XiStringTable* formats()
{
    static std::optional<xi::dat::XiStringTable> table = []() -> std::optional<xi::dat::XiStringTable> {
        const char* root = xi::install::gameRootOrNull();
        if (!root || !std::filesystem::exists(std::filesystem::path(root) / "VTABLE.DAT"))
        {
            return std::nullopt;
        }
        const xi::dat::DatIndex idx(root);
        return xi::dat::XiStringTable::parse(idx.read(50));
    }();
    return table ? &*table : nullptr;
}

// Runs `f` with the local time zone set to UTC.
template <typename F>
void inUtc(F f)
{
    const char*       old = std::getenv("TZ");
    const std::string saved = old ? old : "";
    setenv("TZ", "UTC", 1);
    tzset();
    f();
    if (old)
    {
        setenv("TZ", saved.c_str(), 1);
    }
    else
    {
        unsetenv("TZ");
    }
    tzset();
}

} // namespace

TEST_CASE("game clock: the calendar functions")
{
    // The epoch is 886/1/1 0:00, Firesday, as the game counts (the library adds 886 and 1).
    CHECK(gt::year(0) == 0);
    CHECK(gt::month(0) == 0);
    CHECK(gt::day(0) == 0);
    CHECK(gt::weekday(0) == 0);
    CHECK(gt::hours(0) == 0);
    CHECK(gt::minutes(0) == 0);
    // One Earth second is 25 Vana'diel seconds: 144 a Vana'diel hour, 3456 a day.
    const uint32_t t = 1244160u * 3 + 103680u * 4 + 3456u * 9 + 144u * 13 + 60u;
    CHECK(gt::year(t) == 3);
    CHECK(gt::month(t) == 4);
    CHECK(gt::day(t) == 9);
    CHECK(gt::weekday(t) == (3u * 360u + 4u * 30u + 9u) % 8u);
    CHECK(gt::hours(t) == 13);
    CHECK(gt::minutes(t) == 25);
    CHECK(gt::moonPhase(3456u * 7u * 13u) == 1); // a phase a week, 12 phases
}

TEST_CASE("game clock: the moon percent")
{
    auto at = [](uint32_t days) { return gt::moonPercent(days * 3456u); };
    CHECK(at(4) == 0);    // new moon
    CHECK(at(0) == 10);   // 4 days before: 9.5 -> 10 (rounded away from 0, as the game does)
    CHECK(at(3) == 2);
    CHECK(at(46) == 100); // full moon
    CHECK(at(47) == 98);  // waning
    CHECK(at(84 + 4) == 0);
}

TEST_CASE("game clock: time until")
{
    CHECK(gt::timeDiff(100, 150) == 50);
    CHECK(gt::timeDiff(150, 150) == 0);
    CHECK(gt::timeDiff(150, 100) == 0);
    CHECK(gt::timeDiff(0x90000000u, 0x10000000u) == 0x7FFFFFFFu); // far behind wraps as the game's u32 compare does
}

TEST_CASE("game clock: local time folds 28-year cycles past 2038")
{
    inUtc([] {
        gt::Tm tm;
        REQUIRE(gt::localTime(1761311109u, tm));
        CHECK(tm.year == 125);
        CHECK(tm.mon == 9);
        CHECK(tm.mday == 24);
        CHECK(tm.hour == 13);
        CHECK(tm.min == 5);
        CHECK(tm.sec == 9);
        REQUIRE(gt::localTime(0x80000000u, tm)); // 2038-01-19 03:14:08 UTC
        CHECK(tm.year == 138);
        CHECK(tm.mon == 0);
        CHECK(tm.mday == 19);
        CHECK(tm.hour == 3);
    });
}

TEST_CASE("game clock: string table 10's format codes")
{
    const std::string num  = "\xFA\x40\x8B\x01\x81";
    const std::string str  = "\xFA\x40\x8C\x03\x80\x80";
    const std::string pad2 = "\xFA\x40\x83\x01\x82\x03\x8A\x80\xFA\x40\x89\x0A" "0\xFA\x40\x8B\x01\x82\xFA\x40\x84\x05\xFA\x40\x8B\x01\x82";
    CHECK(gt::format(num + "/" + str + ":" + pad2, { "Oct." }, { 2025, 7 }) == "2025/Oct.:07");
    CHECK(gt::format(pad2, {}, { 0, 42 }) == "42");
    const std::string pick = std::string("\xFA\x40\x82\x01\x81\x83\x80", 7) + std::string("\x80\x80\x80\x80\x80\x80\x80\x80", 8) + "a\xFA\x40\x8D" "b\xFA\x40\x8D" "c\xFA\x40\x8D";
    CHECK(gt::format(pick, {}, { 1 }) == "b");
    CHECK(gt::format(pick, {}, { 2 }) == "c");
}

TEST_CASE("game clock: the time string from the install's formats")
{
    const auto* t = formats();
    if (!t)
    {
        SKIP("no FFXI install (FFXI_ROOT)");
    }
    const auto text = [t](int id) { return std::string(t->get(static_cast<std::size_t>(id))); };
    inUtc([&] {
        const gt::Locale en; // month first, 12-hour (INFERRED for an English client)
        CHECK(gt::timeString(1761311109u, 0, en, text) == "10/24/25 1:05:09pm");
        CHECK(gt::timeString(1761311109u, 1, en, text) == "Oct. 24, 2025 1:05:09pm");
        CHECK(gt::timeString(1761311109u, 2, en, text) == "10/24/25");
        CHECK(gt::timeString(1761311109u, 8, en, text) == "13:05");
        CHECK(gt::timeString(1761264000u, 0x10, en, text) == "12:00:00am");
        gt::Locale jp{ 0, 0, 0 };
        CHECK(gt::timeString(1761311109u, 0, jp, text) == "25/10/24 13:05:09");
        CHECK(gt::timeString(0, 0, en, text) == "--/--/-- --:--:--");
    });
}
