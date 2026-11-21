// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/weather.h"

#include "dat/chunk.h"
#include "xi/bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xi::dat
{

namespace
{

// Weather id -> group fourcc, client table g_WeatherFourcc 0x10377bb8 (§9.4).
constexpr const char* kWeatherFourcc[20] = { "fine", "suny", "clod", "mist", "dryw", "heat", "rain", "squl", "dust", "sand",
                                             "wind", "stom", "snow", "bliz", "thdr", "bolt", "aura", "ligt", "fogd", "dark" };
constexpr int kDay = 86400;

std::array<float, 3> rgb(std::span<const std::byte> r, std::size_t off)
{
    return { static_cast<float>(std::to_integer<uint8_t>(r[off])), static_cast<float>(std::to_integer<uint8_t>(r[off + 1])),
             static_cast<float>(std::to_integer<uint8_t>(r[off + 2])) };
}

WeatherRecord decode(std::span<const std::byte> r)
{
    WeatherRecord w;
    w.flags     = load<uint32_t>(r, 0x00);
    w.sunA      = rgb(r, 0x0C);
    w.moonA     = rgb(r, 0x10);
    w.ambientA  = rgb(r, 0x14);
    w.fogEndA   = load<float>(r, 0x1C);
    w.fogStartA = load<float>(r, 0x20);
    w.multA     = load<float>(r, 0x24);
    w.sunZ      = rgb(r, 0x2C);
    w.moonZ     = rgb(r, 0x30);
    w.ambientZ  = rgb(r, 0x34);
    w.fogColor  = rgb(r, 0x38);
    w.fogEndZ   = load<float>(r, 0x3C);
    w.fogStartZ = load<float>(r, 0x40);
    w.multZ     = load<float>(r, 0x44);
    for (int i = 0; i < 3; ++i)
    {
        w.dirA[i] = static_cast<int8_t>(std::to_integer<uint8_t>(r[0x10 + static_cast<std::size_t>(i)]));
        w.dirZ[i] = static_cast<int8_t>(std::to_integer<uint8_t>(r[0x30 + static_cast<std::size_t>(i)]));
    }
    w.clear = rgb(r, 0x4C);
    for (std::size_t i = 0; i < 8; ++i)
    {
        w.sky[i]    = rgb(r, 0x6C + 4 * i);
        w.skyPos[i] = load<float>(r, 0x8C + 4 * i);
    }
    return w;
}

// Colour bytes interpolate with truncation like the client (prev + trunc((next - prev) * t)); floats linearly.
float lerpByte(float a, float b, float t)
{
    return a + std::trunc((b - a) * t);
}

} // namespace

ZoneWeather ZoneWeather::fromZoneFile(std::span<const std::byte> file)
{
    ZoneWeather        zw;
    std::vector<Chunk> chunks;
    parseChunks(file, chunks);
    std::vector<std::string> stack;
    for (const auto& c : chunks)
    {
        if (c.type == 1)
        {
            stack.push_back(c.name);
            continue;
        }
        if (c.type == 0)
        {
            if (!stack.empty())
            {
                stack.pop_back();
            }
            continue;
        }
        if (c.type != 47 || c.payload.size() < 0xAC || c.name.size() != 4)
        {
            continue;
        }
        // Only the `weat` block (event sets ev01/ev02/... are for cutscenes); group = the block right below it.
        const auto it = std::find(stack.begin(), stack.end(), "weat");
        if (it == stack.end() || it + 1 == stack.end())
        {
            continue;
        }
        const auto& n = c.name;
        if (!std::all_of(n.begin(), n.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        {
            continue;
        }
        Key k;
        k.ticks = ((n[0] - '0') * 10 + (n[1] - '0')) * 3600 + ((n[2] - '0') * 10 + (n[3] - '0')) * 60;
        std::memcpy(k.rec.data(), c.payload.data(), 0xAC);
        zw.groups_[*(it + 1)].push_back(k);
    }
    return zw;
}

std::vector<std::string> ZoneWeather::groupNames() const
{
    std::vector<std::string> out;
    for (const auto& [name, keys] : groups_)
    {
        out.push_back(name);
    }
    return out;
}

WeatherRecord ZoneWeather::lerp(const WeatherRecord& a, const WeatherRecord& b, float t)
{
    WeatherRecord o = a;
    auto c3         = [&](std::array<float, 3>& out, const std::array<float, 3>& x, const std::array<float, 3>& y) {
        for (int i = 0; i < 3; ++i)
        {
            out[i] = lerpByte(x[i], y[i], t);
        }
    };
    auto f = [&](float x, float y) { return x + (y - x) * t; };
    o.flags = a.flags | b.flags;
    c3(o.sunZ, a.sunZ, b.sunZ);
    c3(o.moonZ, a.moonZ, b.moonZ);
    c3(o.ambientZ, a.ambientZ, b.ambientZ);
    c3(o.fogColor, a.fogColor, b.fogColor);
    c3(o.sunA, a.sunA, b.sunA);
    c3(o.moonA, a.moonA, b.moonA);
    c3(o.ambientA, a.ambientA, b.ambientA);
    c3(o.clear, a.clear, b.clear);
    o.fogEndZ   = f(a.fogEndZ, b.fogEndZ);
    o.fogStartZ = f(a.fogStartZ, b.fogStartZ);
    o.multZ     = f(a.multZ, b.multZ);
    o.fogEndA   = f(a.fogEndA, b.fogEndA);
    o.fogStartA = f(a.fogStartA, b.fogStartA);
    o.multA     = f(a.multA, b.multA);
    for (std::size_t i = 0; i < 8; ++i)
    {
        c3(o.sky[i], a.sky[i], b.sky[i]);
        o.skyPos[i] = f(a.skyPos[i], b.skyPos[i]);
    }
    // Explicit directions (bit0) rotate between keys in the client; a normalised linear blend is close enough here.
    for (int i = 0; i < 3; ++i)
    {
        o.dirZ[i] = static_cast<int8_t>(std::lround(f(a.dirZ[i], b.dirZ[i])));
        o.dirA[i] = static_cast<int8_t>(std::lround(f(a.dirA[i], b.dirA[i])));
    }
    return o;
}

WeatherRecord ZoneWeather::sample(uint16_t weatherId, double secondsOfDay) const
{
    return sample(std::string_view(weatherId < std::size(kWeatherFourcc) ? kWeatherFourcc[weatherId] : ""), secondsOfDay);
}

WeatherRecord ZoneWeather::decodeRecord(std::span<const std::byte> record)
{
    return record.size() >= 0xAC ? decode(record) : WeatherRecord{};
}

WeatherRecord ZoneWeather::sample(std::string_view fourcc, double secondsOfDay) const
{
    const std::vector<Key>* keys = nullptr;
    if (const auto it = groups_.find(std::string(fourcc)); !fourcc.empty() && it != groups_.end())
    {
        keys = &it->second;
    }
    if (!keys)
    {
        if (const auto it = groups_.find("suny"); it != groups_.end())
        {
            keys = &it->second;
        }
        else if (!groups_.empty())
        {
            keys = &groups_.begin()->second;
        }
    }
    if (!keys || keys->empty())
    {
        // No Weather in the zone: the client's grey default (§9.6).
        WeatherRecord w;
        w.flags    = 1;
        w.sunZ     = w.sunA = w.ambientZ = w.ambientA = w.fogColor = w.clear = { 128, 128, 128 };
        w.dirZ     = w.dirA = { 0, 127, 0 };
        w.fogStartZ = w.fogStartA = 80.0f;
        w.fogEndZ   = w.fogEndA = 128.0f;
        for (auto& s : w.sky)
        {
            s = { 128, 128, 128 };
        }
        return w;
    }

    const int now = static_cast<int>(secondsOfDay) % kDay;
    // prev: smallest (now - key) >= 0, else the last key iterated; next: smallest (key - now) > 0, else the earliest key.
    const Key* prev = nullptr;
    const Key* next = nullptr;
    const Key* earliest = &keys->front();
    for (const auto& k : *keys)
    {
        if (k.ticks <= now && (!prev || now - k.ticks < now - prev->ticks))
        {
            prev = &k;
        }
        if (k.ticks > now && (!next || k.ticks - now < next->ticks - now))
        {
            next = &k;
        }
        if (k.ticks < earliest->ticks)
        {
            earliest = &k;
        }
    }
    if (!prev)
    {
        prev = &keys->back();
        for (const auto& k : *keys)
        {
            if (k.ticks > prev->ticks)
            {
                prev = &k; // latest key of the previous day
            }
        }
    }
    if (!next)
    {
        next = earliest;
    }
    const auto a = decode(prev->rec);
    if (prev == next)
    {
        return a;
    }
    const int   span = ((next->ticks - prev->ticks) % kDay + kDay) % kDay;
    const int   el   = ((now - prev->ticks) % kDay + kDay) % kDay;
    const float t    = span > 0 ? static_cast<float>(el) / static_cast<float>(span) : 0.0f;
    return lerp(a, decode(next->rec), t);
}

const char* weatherFourcc(uint16_t weatherId)
{
    return weatherId < std::size(kWeatherFourcc) ? kWeatherFourcc[weatherId] : "suny";
}

const char* altWeatherFourcc(uint16_t weatherId)
{
    // g_WeatherFourccAlt 0x10377c08 (§9.4).
    constexpr const char* kAlt[20] = { "fin1", "sun1", "clo1", "mis1", "dry1", "hea1", "rai1", "squ1", "dus1", "san1",
                                       "win1", "sto1", "sno1", "bli1", "thd1", "bol1", "aur1", "lig1", "fog1", "dar1" };
    return weatherId < std::size(kAlt) ? kAlt[weatherId] : "sun1";
}

} // namespace xi::dat
