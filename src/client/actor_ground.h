// SPDX-License-Identifier: GPL-3.0-or-later
// Where other actors stand: the game's ground probe for NPCs, monsters and other players (dat-format §7.8c).
#pragma once

#include <cstdint>
#include <optional>

namespace xi::client
{

// The game's actor update (CODE 0x100a4e00) stands an actor on the zone's collision floor (0x101813e0 -> 0x101691a0 ->
// 0x1016a8a0 with flag 1): a segment from the collision's step height above its server position (+0x16c: 2.0, or 1.5 in
// zone 149, CODE 0x10178d00) to 40 below it (0x10377320, the zone table 0x1018b2b0's default), the hit nearest the top,
// so the highest floor in that span. A miss leaves the server height. Skipped for an actor whose 0x00D / 0x00E
// GroundFlag (+0x18 bit 15 -> entity +0x120 bit 20, CODE 0x1009bb2d) or a PC's HackMove (0x00D +0x20 bit 27 -> +0x128
// bit 14, CODE 0x1009a456) is set, or after an event's op 33 (+0x120 bit 21).
inline constexpr float kActorGroundBelow = 40.0f;
[[nodiscard]] constexpr float actorGroundAbove(uint16_t zone)
{
    return zone == 149 ? 1.5f : 2.0f;
}

// `highestFloor(maxY)`: the highest floor at the actor's x / z whose height is <= maxY (render space, y up), or nullopt.
// Returns the height to stand the actor at (render space), nullopt to keep the server's `y`.
template <class HighestFloor>
[[nodiscard]] std::optional<float> actorGround(float y, HighestFloor&& highestFloor, float above = 2.0f)
{
    const auto g = highestFloor(y + above);
    if (g && *g >= y - kActorGroundBelow)
    {
        return g;
    }
    return std::nullopt;
}

} // namespace xi::client
