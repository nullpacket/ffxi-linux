// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/target_lock.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace xi::client;

namespace
{

struct FakeEntity
{
    uint8_t status = 0;
    bool    isMob  = false;
    bool    isPc   = false;
    uint8_t hpp    = 100;
};

} // namespace

TEST_CASE("target lock: Num * and /lockon toggle it, only on a living target")
{
    CHECK(targetLockToggle(false, true));
    CHECK_FALSE(targetLockToggle(true, true));   // unlock
    CHECK_FALSE(targetLockToggle(false, false)); // nothing to lock on
    CHECK_FALSE(targetLockToggle(true, false));
}

TEST_CASE("target lock: what it can hold")
{
    FakeEntity mob{ .isMob = true };
    CHECK(lockable(&mob));
    mob.hpp = 0;
    CHECK_FALSE(lockable(&mob)); // knocked out
    FakeEntity npc{ .hpp = 0 };
    CHECK(lockable(&npc)); // NPCs carry no HP
    npc.status = 2;
    CHECK_FALSE(lockable(&npc)); // gone
    FakeEntity pc{ .status = 3, .isPc = true };
    CHECK_FALSE(lockable(&pc));
    CHECK_FALSE(lockable<FakeEntity>(nullptr));
}

TEST_CASE("target lock: engaging locks on, unlocking holds until we disengage")
{
    bool on    = false;
    bool was   = false;
    auto frame = [&](bool engaged, bool alive) {
        on  = targetLockStep(on, engaged, was, alive);
        was = engaged;
    };
    frame(false, true);
    CHECK_FALSE(on);
    frame(true, true); // engaged
    CHECK(on);
    on = targetLockToggle(on, true); // Num *: unlocked while still fighting
    frame(true, true);
    CHECK_FALSE(on);                 // stays off: only the engage edge locks
    on = targetLockToggle(on, true); // Num * again
    frame(true, true);
    CHECK(on);
    frame(false, true); // disengaged
    CHECK_FALSE(on);
    on = targetLockToggle(on, true); // /lockon out of combat
    frame(false, true);
    CHECK(on);
    frame(false, false); // the target died or left
    CHECK_FALSE(on);
}

TEST_CASE("target lock: steps are named relative to the target")
{
    const glm::vec2 face(0.0f, -1.0f);                // render x/z: the target straight ahead along -z
    CHECK(lockedMoveDir(face, { 0.0f, -2.0f }) == 0); // toward it
    CHECK(lockedMoveDir(face, { 0.0f, 1.0f }) == 1);  // away: back step
    CHECK(lockedMoveDir(face, { 1.0f, 0.0f }) == 3);  // +x with the target at -z: right
    CHECK(lockedMoveDir(face, { -1.0f, 0.0f }) == 2); // left
    CHECK(lockedMoveDir(face, { 0.3f, -1.0f }) == 0); // mostly ahead
    CHECK(lockedMoveDir(face, { 0.0f, 0.0f }) == 0);  // no step
}
