// SPDX-License-Identifier: GPL-3.0-or-later
// The game's clock functions, as FFXiMain.dll has them (docs/ashita-compat.md, "The game's clock"), for the addons
// that call them through the Ashita `ffxi.time` library (fishaid, ...). `t` is the game's raw time: Earth seconds since
// the Vana'diel epoch (one second = 25 Vana'diel seconds; a Vana'diel day is 3456 of them).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace xi::ashita::gametime
{

[[nodiscard]] uint32_t weekday(uint32_t t);     // ntGameDayWeekGet 0x10103a10: 0..7
[[nodiscard]] uint32_t day(uint32_t t);         // ntGameDayGet 0x101039e0: 0..29
[[nodiscard]] uint32_t month(uint32_t t);       // ntGameMonthGet 0x101039c0: 0..11
[[nodiscard]] uint32_t year(uint32_t t);        // ntGameYearGet 0x101039b0: years since 886
[[nodiscard]] uint32_t hours(uint32_t t);       // ntGameHourGet 0x10103a40: 0..23
[[nodiscard]] uint32_t minutes(uint32_t t);     // ntGameMinGet 0x10103a60: 0..59
[[nodiscard]] uint32_t moonPhase(uint32_t t);   // ntGameMoonGet 0x10103a90: 0..11 (a phase a week)
[[nodiscard]] uint32_t moonPercent(uint32_t t); // ntGameMoonPerGet 0x10103ad0: 0..100
// ntGameTimeDiff 0x10103980: seconds from `now` until `t`; 0 once it has passed (up to 2^31 back).
[[nodiscard]] uint32_t timeDiff(uint32_t now, uint32_t t);

// The CRT's struct tm as the game uses it (9 int32s).
struct Tm
{
    int32_t sec = 0, min = 0, hour = 0, mday = 0, mon = 0, year = 0, wday = 0, yday = 0, isdst = 0;
};
// sqLocalTime 0x101c88c0: localtime of a Unix time; from 0x7FE81780 on the time is moved back 28-year cycles (the same
// calendar) and the years added back.
[[nodiscard]] bool localTime(uint32_t unixTime, Tm& out);

// The PlayOnline locale the time string follows (the object at 0x10456a28): +0x18 bit 0 the time format, bits 1-2 the
// date order, +8 the language. Not traced to a client setting: an English client's (INFERRED).
struct Locale
{
    int dateOrder = 1; // 0 year first, 1 month first, 2 day first
    int timeFormat = 1; // 0 24-hour, 1 12-hour with am / pm
    int language = 1;
};

// YkGetTimeString 0x101c8b80 for a Unix time: date and time from string table 10's formats (`text` gives an entry's
// raw bytes). flags: 1 the month by name (4: the second name list), 2 the date only, 8 hours and minutes only, 0x18
// the time only. 0 or an unreadable time gives the tables' placeholders ("--/--/--").
[[nodiscard]] std::string timeString(uint32_t unixTime, uint8_t flags, const Locale& locale, const std::function<std::string(int id)>& text);

// String table 10's format codes (0xFA 0x40 op ...) with the string and number arguments the game passes; numbers are
// referred to from 1. The codes these formats use: 0x8B a number, 0x8C a string, 0x83 + 0x86 / 0x89 a comparison (equal
// / less than) whose then-part ends in 0x84 (skip the else-part), 0x82 a choice among 0x8D-separated texts.
[[nodiscard]] std::string format(std::string_view fmt, const std::vector<std::string>& strings, const std::vector<int32_t>& numbers);

} // namespace xi::ashita::gametime
