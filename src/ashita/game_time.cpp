// SPDX-License-Identifier: GPL-3.0-or-later
#include "ashita/game_time.h"

#include <ctime>

namespace xi::ashita::gametime
{

namespace
{
constexpr uint32_t kDay   = 3456;  // Earth seconds in a Vana'diel day
constexpr uint32_t kMonth = 103680; // 30 days
constexpr uint32_t kYear  = 1244160; // 360 days
} // namespace

uint32_t weekday(uint32_t t)
{
    return t % (8 * kDay) / kDay;
}

uint32_t day(uint32_t t)
{
    return t % kMonth / kDay;
}

uint32_t month(uint32_t t)
{
    return t % kYear / kMonth;
}

uint32_t year(uint32_t t)
{
    return t / kYear;
}

uint32_t hours(uint32_t t)
{
    return t % kDay / 144;
}

uint32_t minutes(uint32_t t)
{
    return t % 144 * 60 / 144;
}

uint32_t moonPhase(uint32_t t)
{
    return t / kDay / 7 % 12;
}

uint32_t moonPercent(uint32_t t)
{
    // 84 days a cycle, the day 4 days in being new moon; x87 float as the game computes it, truncated after +-0.5.
    const auto  r = static_cast<int32_t>(t / kDay % 84);
    const float x = static_cast<float>(r - 4) * (200.0f / 84.0f);
    const auto  n = static_cast<int32_t>(x < 0.0f ? x - 0.5f : x + 0.5f);
    if (r < 4)
    {
        return static_cast<uint32_t>(-n);
    }
    if (r <= 46)
    {
        return static_cast<uint32_t>(n);
    }
    return static_cast<uint32_t>(200 - n);
}

uint32_t timeDiff(uint32_t now, uint32_t t)
{
    if (now <= t)
    {
        return t - now;
    }
    const uint32_t d = t - now - 1;
    return d <= 0x7FFFFFFFu ? d : 0;
}

bool localTime(uint32_t unixTime, Tm& out)
{
    uint32_t t     = unixTime;
    int32_t  years = 0;
    if (t >= 0x7FE81780u)
    {
        const auto cycles = static_cast<uint32_t>((uint64_t{ t - 0x4B3D3B00u } * 0x4DC56229u) >> 60);
        t -= cycles * 0x34AADC80u; // 28 years of seconds
        years = static_cast<int32_t>(cycles) * 28;
    }
    const auto when = static_cast<std::time_t>(t);
    std::tm    tm{};
    if (localtime_r(&when, &tm) == nullptr)
    {
        return false;
    }
    out = { tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year + years, tm.tm_wday, tm.tm_yday, tm.tm_isdst };
    return true;
}

namespace
{

constexpr char kEsc0 = static_cast<char>(0xFA), kEsc1 = 0x40;

// An operand: 01 8N = number argument N; 03 lo hi = a constant (7 bits each, 0x80 added).
bool operand(std::string_view s, std::size_t& i, const std::vector<int32_t>& numbers, int32_t& v)
{
    if (i >= s.size())
    {
        return false;
    }
    const auto kind = static_cast<uint8_t>(s[i]);
    if (kind == 0x01 && i + 1 < s.size())
    {
        const int n = static_cast<uint8_t>(s[i + 1]) - 0x80;
        v           = n >= 1 && static_cast<std::size_t>(n) <= numbers.size() ? numbers[static_cast<std::size_t>(n) - 1] : 0;
        i += 2;
        return true;
    }
    if (kind == 0x03 && i + 2 < s.size())
    {
        v = (static_cast<uint8_t>(s[i + 1]) - 0x80) | ((static_cast<uint8_t>(s[i + 2]) - 0x80) << 7);
        i += 3;
        return true;
    }
    return false;
}

bool escapeAt(std::string_view s, std::size_t i)
{
    return i + 2 < s.size() && s[i] == kEsc0 && s[i + 1] == kEsc1;
}

} // namespace

std::string format(std::string_view s, const std::vector<std::string>& strings, const std::vector<int32_t>& numbers)
{
    std::string out;
    std::size_t i = 0;
    while (i < s.size())
    {
        if (!escapeAt(s, i))
        {
            out += s[i++];
            continue;
        }
        const auto op = static_cast<uint8_t>(s[i + 2]);
        i += 3;
        int32_t v = 0;
        switch (op)
        {
            case 0x8B: // a number
                if (!operand(s, i, numbers, v))
                {
                    return out;
                }
                out += std::to_string(v);
                break;
            case 0x8C: // a string
                if (!operand(s, i, numbers, v))
                {
                    return out;
                }
                if (v >= 0 && static_cast<std::size_t>(v) < strings.size())
                {
                    out += strings[static_cast<std::size_t>(v)];
                }
                break;
            case 0x83: // compare a with b; then FA 40 86 (equal) / 89 (less) L: L bytes of then-part (ending in FA 40 84 M,
                       // which skips the M-byte else-part that follows)
            {
                int32_t a = 0, b = 0;
                if (!operand(s, i, numbers, a) || !operand(s, i, numbers, b) || !escapeAt(s, i) || i + 3 >= s.size())
                {
                    return out;
                }
                const auto kind = static_cast<uint8_t>(s[i + 2]);
                const auto len  = static_cast<uint8_t>(s[i + 3]);
                i += 4;
                const bool yes = kind == 0x86 ? a == b : kind == 0x89 ? a < b : false;
                if (!yes)
                {
                    i += len;
                }
                break;
            }
            case 0x84: // the end of a then-part: skip the else-part
                if (i >= s.size())
                {
                    return out;
                }
                i += 1 + static_cast<uint8_t>(s[i]);
                break;
            case 0x82: // choose: operand, the entry count (2 bytes), count + 1 offsets (2 bytes each), then the entries,
                       // each ended by FA 40 8D
            {
                if (!operand(s, i, numbers, v) || i + 1 >= s.size())
                {
                    return out;
                }
                const int count = static_cast<uint8_t>(s[i]) - 0x80;
                i += 2 + static_cast<std::size_t>(count + 1) * 2;
                int k = 0;
                while (i < s.size() && k < count)
                {
                    std::size_t end = i;
                    while (end < s.size() && !(escapeAt(s, end) && static_cast<uint8_t>(s[end + 2]) == 0x8D))
                    {
                        ++end;
                    }
                    if (k == v)
                    {
                        out.append(s.substr(i, end - i));
                    }
                    i = end + 3;
                    ++k;
                }
                break;
            }
            default: return out; // a code these formats do not use
        }
    }
    return out;
}

namespace
{

struct Formats
{
    int key, f1, f2, f3, f4, f5;
};
// 0x1037d31c (dates) and 0x1037d34c (times): string table 10 ids by the locale's date order / time format.
constexpr Formats kDates[] = { { 1, 0x3C, 0x4E, 0x49, 0x44, 0x44 }, { 2, 0x3D, 0x4F, 0x4A, 0x45, 0x45 }, { 0, 0x3B, 0x4D, 0x48, 0x43, 0x43 } };
constexpr Formats kTimes[] = { { 1, 0x3F, 0x3F, 0x4B, 0x46, 0x40 }, { 0, 0x3E, 0x3E, 0x4B, 0x46, 0x40 } };

const Formats* find(const Formats* list, std::size_t n, int key)
{
    for (std::size_t i = 0; i < n; ++i)
    {
        if (list[i].key == key)
        {
            return &list[i];
        }
    }
    return nullptr;
}

} // namespace

std::string timeString(uint32_t unixTime, uint8_t flags, const Locale& locale, const std::function<std::string(int id)>& text)
{
    auto str = [&](int id) { return text ? text(id) : std::string(); };
    Tm   tm;
    const bool have = unixTime != 0 && localTime(unixTime, tm);
    int        hour12 = 0;
    if (have)
    {
        hour12 = tm.hour >= 12 ? tm.hour - 12 : tm.hour;
        if (hour12 == 0 && (locale.language != 0 || tm.hour == 12))
        {
            hour12 = 12;
        }
    }
    const Formats* date = find(kDates, std::size(kDates), locale.dateOrder & 3);
    const Formats* time = find(kTimes, std::size(kTimes), locale.timeFormat & 1);
    std::string    d, t;
    if (date == nullptr || time == nullptr)
    {
        d = (flags & 1) ? "----/--/--" : "--/--/--";
    }
    else if (have)
    {
        const int32_t y = tm.year + 1900;
        if (flags & 1)
        {
            const auto mon = format(str((flags & 4) ? 0x61 : 0x60), {}, { tm.mon + 1 });
            d              = format(str(date->f1), { mon }, { y, tm.mday });
        }
        else
        {
            d = format(str(date->f2), { std::to_string(tm.mon + 1) }, { y % 100, tm.mday });
        }
        t = (flags & 8) ? format(str(time->f5), {}, { tm.hour, hour12, tm.min }) : format(str(time->f1), {}, { tm.hour, hour12, tm.min, tm.sec });
    }
    else
    {
        d = format(str((flags & 1) ? date->f3 : 0x4C), {}, {});
        t = format(str(time->f3), {}, {});
    }
    if (flags & 2)
    {
        return d;
    }
    if (flags & 0x18)
    {
        return t;
    }
    return d + " " + t;
}

} // namespace xi::ashita::gametime
