// SPDX-License-Identifier: GPL-3.0-or-later
// Multi-hit weapon skills play every motion of their scheduler, each at its own tick (#112; effect-format §S.3,
// dat-format §10.8). Needs the game data (skipped when FFXI_ROOT is unset or missing).
#include "dat/dat_index.h"
#include "dat/motion.h"
#include "gfx/character_anim.h"
#include "xi/install.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <vector>

namespace
{

const xi::dat::DatIndex* install()
{
    static std::unique_ptr<xi::dat::DatIndex> idx = [] {
        const char* root = xi::install::gameRootOrNull();
        if (!root || !std::filesystem::exists(std::filesystem::path(root) / "VTABLE.DAT"))
        {
            return std::unique_ptr<xi::dat::DatIndex>{};
        }
        return std::make_unique<xi::dat::DatIndex>(root);
    }();
    return idx.get();
}

} // namespace

TEST_CASE("Combo's scheduler has a wind-up and one motion per punch, each at its own tick (#112)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Mithra (race 7), Combo (animation 16): file 37067 + 16. main: a20? t0 dur 30, a22? t30 dur 50, a02? t45 dur 35,
    // a52? t70 dur 60 (post-delay timing; the three target flinches at t40 / t57 / t80).
    const auto sets = xi::dat::loadWeaponSkillMotions(*install(), 7, 16);
    REQUIRE(sets.size() == 4);
    CHECK(sets[0].at == 0.0f);
    CHECK(sets[1].at == 30.0f);
    CHECK(sets[2].at == 45.0f);
    CHECK(sets[3].at == 70.0f);
    CHECK(sets[0].duration == 30.0f);
    CHECK(sets[1].duration == 50.0f);
    CHECK(sets[2].duration == 35.0f);
    CHECK(sets[3].duration == 60.0f);
    CHECK(sets[0].blendIn == 20.0f);
    CHECK(sets[3].blendIn == 10.0f);
}

TEST_CASE("a weapon skill's motions all play, in order and on the scheduler's clock (#112)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto sets = xi::dat::loadWeaponSkillMotions(*install(), 7, 16);
    REQUIRE(sets.size() == 4);
    xi::gfx::CharacterModel cm;
    xi::gfx::AnimTrack      track;
    xi::gfx::AnimInput      in;
    in.engaged = true;
    xi::gfx::animate(track, cm, in, 1.0f / 60.0f); // settle: engaged, no weapon skill yet
    in.weaponSkills = 1;
    in.ws           = &sets;
    std::vector<int> shown(200, -1); // per tick: which of the four motions is playing
    for (int tick = 0; tick < 200; ++tick)
    {
        xi::gfx::animate(track, cm, in, 1.0f / 60.0f);
        for (std::size_t k = 0; k < sets.size(); ++k)
        {
            if (track.oneShot == &sets[k])
            {
                shown[tick] = static_cast<int>(k);
            }
        }
    }
    CHECK(shown[5] == 0);    // wind-up
    CHECK(shown[35] == 1);   // first punch
    CHECK(shown[50] == 2);   // second punch
    CHECK(shown[75] == 3);   // third punch
    CHECK(shown[128] == 3);  // the last one fills its 60 ticks
    CHECK(shown[160] == -1); // then the battle stance returns
    int last = 0;
    for (const int k : shown)
    {
        if (k >= 0)
        {
            CHECK(k >= last); // never back to an earlier motion
            last = k;
        }
    }
}

TEST_CASE("moving cancels a weapon skill's remaining motions (#112)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto sets = xi::dat::loadWeaponSkillMotions(*install(), 7, 16);
    REQUIRE(sets.size() == 4);
    xi::gfx::CharacterModel cm;
    xi::gfx::AnimTrack      track;
    xi::gfx::AnimInput      in;
    in.engaged = true;
    xi::gfx::animate(track, cm, in, 1.0f / 60.0f);
    in.weaponSkills = 1;
    in.ws           = &sets;
    for (int tick = 0; tick < 35; ++tick)
    {
        xi::gfx::animate(track, cm, in, 1.0f / 60.0f);
    }
    REQUIRE(track.oneShot == &sets[1]);
    in.speed = 5.0f;
    xi::gfx::animate(track, cm, in, 1.0f / 60.0f);
    in.speed = 0.0f;
    for (int tick = 0; tick < 60; ++tick)
    {
        xi::gfx::animate(track, cm, in, 1.0f / 60.0f);
        CHECK(track.oneShot == nullptr);
    }
}
