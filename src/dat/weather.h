// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xi::dat
{

// One evaluated environment record (docs/dat-format.md §9.2). Colours are 0..255 floats (R, G, B).
struct WeatherRecord
{
    uint32_t                 flags = 0; // bit0: explicit light directions (indoor zones)
    // Set Z (zone meshes) and set A (characters).
    std::array<float, 3>     sunZ{}, moonZ{}, ambientZ{}, fogColor{};
    float                    fogEndZ = 100.0f, fogStartZ = 80.0f, multZ = 1.0f;
    std::array<float, 3>     sunA{}, moonA{}, ambientA{};
    float                    fogEndA = 100.0f, fogStartA = 80.0f, multA = 1.0f;
    std::array<int8_t, 3>    dirZ{}, dirA{}; // valid when flags bit0 (+0x30 / +0x10 as int8)
    std::array<float, 3>     clear{};        // +0x4C
    std::array<std::array<float, 3>, 8> sky{}; // +0x6C gradient colours, stop 0 = horizon
    std::array<float, 8>     skyPos{};        // +0x8C gradient positions 0..1
};

// The zone's Weather keyframes grouped by weather fourcc (the `weat` block, §9.4).
class ZoneWeather
{
public:
    static ZoneWeather fromZoneFile(std::span<const std::byte> zoneFile);

    [[nodiscard]] bool empty() const
    {
        return groups_.empty();
    }

    // Record for a server weather id (S2C 0x00A/0x057) at a Vana'diel time of day in seconds [0, 86400).
    // Falls back to "suny" like the client, then to any group; with no Weather at all, the client's grey default.
    [[nodiscard]] WeatherRecord sample(uint16_t weatherId, double secondsOfDay) const;
    // The same for a group by name (`suny`, or an alternate-table `sun1`; effect ops 0x7C / 0xAE), same fallbacks.
    [[nodiscard]] WeatherRecord sample(std::string_view fourcc, double secondsOfDay) const;
    // One Weather keyframe payload (type 0x2f, 0xAC bytes) as a record (effect op 0x7E).
    static WeatherRecord decodeRecord(std::span<const std::byte> record);

    [[nodiscard]] std::vector<std::string> groupNames() const;

    static WeatherRecord lerp(const WeatherRecord& a, const WeatherRecord& b, float t);

private:
    struct Key
    {
        int                     ticks = 0; // seconds of day for HHMM
        std::array<std::byte, 0xAC> rec{};
    };
    std::map<std::string, std::vector<Key>> groups_;
};

// The weather group fourcc for a server weather id (`fine`, `suny`, `clod` ...; client table 0x10377bb8), "suny"
// for unknown ids.
const char* weatherFourcc(uint16_t weatherId);
// The alternate table (`fin1`, `sun1` ...; client 0x10377c08), chosen by effect op 0xAE.
const char* altWeatherFourcc(uint16_t weatherId);

} // namespace xi::dat
