// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ashita/game_time.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace xi::ui
{
// The compass and the Vana'diel clock above the log window (issue #120; docs/ui-format-notes.md §10.13). The game keeps
// one object for both (0x10634364, 0x3C bytes, ctor 0x1021ea10), opened in the field (0x10160050) and drawn each frame
// by 0x1021e8f0 -> 0x1021ff10 (the compass) and 0x1021fd50 (the clock line).

// Where it sits (window units): x 0x58, and 0x2A above the top of log window 1 (ctor +0x28 / +0x2C; the log manager
// stores window 1's top through 0x1021e980 at 0x1015fbe9).
struct CompassPlace
{
    float x = 0, y = 0; // the dial's centre
};
inline CompassPlace compassPlace(float logTop)
{
    return { 88.0f, logTop - 42.0f };
}

// The radar's reach and size by job (0x1021eac0): 0.8 x 36 = 28.8 yalms at size 0.8, a Ninja (main or support) 36 at
// 1.0; Thief, Beastmaster, Ranger, Ninja, Summoner and Blue Mage (main, or support while it is active) also see
// monsters on it.
struct CompassRange
{
    float range    = 28.8f; // yalms (+0x34); dots fade out from half of it (+0x38)
    float size     = 0.8f;  // the dial's size (+0x30)
    bool  monsters = false; // +0x2F
};
inline CompassRange compassRange(uint8_t mainJob, uint8_t subJob, bool subActive)
{
    auto         sees = [](uint8_t j) { return j == 6 || j == 9 || j == 11 || j == 13 || j == 15 || j == 16; };
    CompassRange r;
    if (mainJob == 13 || subJob == 13)
    {
        r.range = 36.0f;
        r.size  = 1.0f;
    }
    r.monsters = sees(mainJob) || (subActive && sees(subJob));
    return r;
}

// Zones without the dial (0x100ad6f0: the ferries, the airships and the ships at sea); the clock line still shows.
inline bool compassHiddenIn(uint16_t zone)
{
    switch (zone)
    {
        case 1:
        case 3:
        case 0x2e:
        case 0x2f:
        case 0x3a:
        case 0x3b:
        case 0x3c:
        case 0xdc:
        case 0xdd:
        case 0xdf:
        case 0xe0:
        case 0xe1:
        case 0xe2:
        case 0xe3:
        case 0xe4: return true;
        default: return false;
    }
}

// The dial is a disc seen at a slant: scaled, turned with the camera, then tipped back about the screen's x axis by
// 1.0996 radians (0x10027ab0 with 0xBF8CBE4C), so it reads as an ellipse lying on the ground. A world offset (game x
// east, z north) becomes a screen offset with the camera's forward (fx, fz) pointing up. The game builds the turn from
// its camera yaw (0x10220780: 3pi/2 - yaw); this takes the forward vector instead (the same turn, INFERRED).
inline constexpr float kCompassTilt = 0.45399f; // cos(1.0996)

// The dial's turn (cos, sin of a clockwise angle on screen): north points the way the camera's forward does, so with
// the camera facing north, north is at the top and east on the right. `spin` adds the fade-in's swing.
struct CompassTurn
{
    float c = 1, s = 0;
};
inline CompassTurn compassTurn(float fx, float fz, float spin = 0)
{
    const float len = std::sqrt(fx * fx + fz * fz);
    if (len <= 0.0f)
    {
        fx = 0, fz = 1;
    }
    else
    {
        fx /= len, fz /= len;
    }
    // Turning dial-space up (0, -1) clockwise by a gives (sin a, -cos a); north must land at (-fx, -fz) before the
    // tilt (to the right by how far the camera looks left of north, up by how far it looks north).
    const float a = std::atan2(-fx, fz) + spin;
    return { std::cos(a), std::sin(a) };
}

struct CompassOffset
{
    float x = 0, y = 0;
};
// A point of the dial (lx right, ly down, north up; authored units times `k`) on screen, turned and tilted.
inline CompassOffset compassPoint(float lx, float ly, CompassTurn t, float k)
{
    return { (lx * t.c - ly * t.s) * k, (lx * t.s + ly * t.c) * k * kCompassTilt };
}
// A world offset (game x east, z north, yalms) on the dial at `k` pixels a yalm.
inline CompassOffset compassWorld(float dx, float dz, CompassTurn t, float k)
{
    return compassPoint(dx, -dz, t, k);
}

// The fade in after zoning (0x1021eda0): 24 ticks (0x10385ECC) from the zone being ready; alpha 0 -> 0x80, the size
// 0 -> 1, and the dial swings in from half a turn ((1 - f) * pi added to its angle).
inline constexpr int kCompassFadeTicks = 24;

// A dot's alpha (0x10220340..0x102203d7): full inside half the range, fading to 0 at the range; above 2 yalms of
// height difference it fades again, gone at 4 (the entity is skipped beyond).
inline int compassDotAlpha(float dist, float dy, const CompassRange& r, int alpha)
{
    const float inner = r.range * 0.5f;
    dy                = std::abs(dy);
    if (dist > r.range || dy > 4.0f)
    {
        return 0;
    }
    float a = static_cast<float>(alpha);
    if (dist - inner > 0.0f)
    {
        a *= 1.0f - (dist - inner) / (r.range - inner);
    }
    if (dy - 2.0f > 0.0f)
    {
        a *= 1.0f - (dy - 2.0f) * 0.5f;
    }
    return std::clamp(static_cast<int>(a), 0, 0xFF);
}

// A dot's colour, D3DCOLOR RGB (0x102203dd..): players blue (party or alliance 0x3060E0, others 0x0040F0), NPCs green
// 0x00B040, monsters (only when the job sees them) grey 0x808080, dead ones 0x206060.
enum class CompassDot : uint8_t
{
    None,
    Player,
    Ally,
    Npc,
    Monster,
    DeadMonster,
};
inline uint32_t compassDotColour(CompassDot k)
{
    switch (k)
    {
        case CompassDot::Player: return 0x0040F0;
        case CompassDot::Ally: return 0x3060E0;
        case CompassDot::Npc: return 0x00B040;
        case CompassDot::Monster: return 0x808080;
        case CompassDot::DeadMonster: return 0x206060;
        default: return 0;
    }
}

// The clock line (0x1021fd50): "%d:%02d  %s" (0x10385F5C) with the Vana'diel hour and minute and our map square
// ("(H-8)", or "(?-?)" where the zone has no map; 0x101f71b0), white (0x80808080) at the dial's x - 0x32, y + 0x1A.
inline std::string compassClockText(uint32_t gameTime, std::string_view square)
{
    return fmt::format("{}:{:02d}  {}", ashita::gametime::hours(gameTime), ashita::gametime::minutes(gameTime), square);
}

// The day's icon left of the clock (x - 0x44, y + 0x1A): "menu frames" sprite by weekday (table 0x10385EE8:
// Firesday .. Darksday).
inline uint16_t compassDaySprite(uint32_t gameTime)
{
    constexpr std::array<uint16_t, 8> kDay = { 106, 109, 111, 108, 107, 110, 112, 113 };
    return kDay[ashita::gametime::weekday(gameTime) % 8];
}

// The weather's element icon above the dial (x - 0x44, y - 0x2A): "font usgaiji" glyph by weather id 4..19 (the same
// table from entry 8), drawn twice side by side for the strong (odd) weathers; none for clear skies, clouds or fog.
struct CompassWeather
{
    uint16_t glyph  = 0;
    bool     strong = false;
};
inline std::optional<CompassWeather> compassWeather(uint16_t weather)
{
    constexpr std::array<uint16_t, 8> kGlyph = {
        0, 5, 3, 2, 1, 4, 6, 7
    }; // fire, water, earth, wind, ice, thunder, light, dark
    if (weather < 4 || weather >= 20)
    {
        return std::nullopt;
    }
    return CompassWeather{ kGlyph[(weather - 4) / 2], (weather & 1) != 0 };
}
} // namespace xi::ui
