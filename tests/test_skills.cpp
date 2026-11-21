// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/skills.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

namespace
{
// A 0x062 packet (header included): skill_base[i] at 0x80 + 2 * i.
std::vector<std::byte> clistatus2(std::initializer_list<std::pair<int, uint16_t>> skills)
{
    std::vector<std::byte> p(0x100);
    p[0] = std::byte{ 0x62 };
    p[1] = std::byte{ 0x80 }; // size 0x100 / 2 in the upper bits
    for (const auto& [id, v] : skills)
    {
        std::memcpy(p.data() + 0x80 + 2 * id, &v, 2);
    }
    return p;
}
} // namespace

TEST_CASE("S2C 0x062: skill_base at +0x80, combat level and cap bit, craft level and rank")
{
    using namespace xi::client;
    // Sword 25 (skill 3), Dagger capped at 40 (2), automaton melee absent (22 = 0x8000), Fishing 12 rank 2 (48),
    // Cooking capped 100 rank 9 (57), Synergy not shown (58 = 0xFFFF).
    const auto p = clistatus2({ { 3, 25 }, { 2, 0x8000 | 40 }, { 22, 0x8000 }, { 48, 12 << 5 | 2 }, { 57, 0x8000 | 100 << 5 | 9 }, { 58, 0xFFFF } });
    SkillTable s{};
    REQUIRE(parseSkills(p, s));
    CHECK(skillLevel(s[3]) == 25);
    CHECK_FALSE(skillCapped(s[3]));
    CHECK(skillLevel(s[2]) == 40);
    CHECK(skillCapped(s[2]));
    CHECK(skillShown(s[2]));
    CHECK_FALSE(skillShown(s[22]));
    CHECK(skillShown(s[4])); // level 0, not capped: shown as 0
    CHECK(craftLevel(s[48]) == 12);
    CHECK(craftRank(s[48]) == 2);
    CHECK(craftLevel(s[57]) == 100);
    CHECK(craftRank(s[57]) == 9);
    CHECK(skillCapped(s[57]));
    CHECK(s[58] == 0xFFFF);

    SkillTable short_{};
    CHECK_FALSE(parseSkills(std::span(p).first(0xFF), short_));
}
