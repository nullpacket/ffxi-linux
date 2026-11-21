// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "xi/bytes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Skill values from S2C 0x062 CLISTATUS2 (skill_base[64] @+0x80), as the Status windows read them (CODE 0x101483e0 /
// 0x101485e0 / 0x101487d0): combat and magic skills 1..47 are the level in bits 0..14 with bit 15 = capped; crafts
// 48..62 are level << 5 | rank, bit 15 = capped, 0xFFFF = not shown.
namespace xi::client
{

using SkillTable = std::array<uint16_t, 64>;

// skill_base from a 0x062 packet (header included); false if it is too short.
[[nodiscard]] inline bool parseSkills(std::span<const std::byte> d, SkillTable& out)
{
    if (d.size() < 0x80 + 2 * out.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = load<uint16_t>(d, 0x80 + 2 * i);
    }
    return true;
}

[[nodiscard]] constexpr uint16_t skillLevel(uint16_t v)
{
    return v & 0x7FFF;
}
[[nodiscard]] constexpr bool skillCapped(uint16_t v)
{
    return (v & 0x8000) != 0;
}
// Combat / magic rows: a skill with bit 15 and level 0 is one the job does not have (LSB also sends 0x8000 for the
// automaton skills 22..24); the windows skip it.
[[nodiscard]] constexpr bool skillShown(uint16_t v)
{
    return !skillCapped(v) || skillLevel(v) != 0;
}
[[nodiscard]] constexpr uint16_t craftLevel(uint16_t v)
{
    return (v >> 5) & 0x3FF;
}
[[nodiscard]] constexpr uint8_t craftRank(uint16_t v)
{
    return static_cast<uint8_t>(v & 0x1F);
}

} // namespace xi::client
