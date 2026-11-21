// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/battle_colours.h"
#include "client/combat.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

namespace
{

// Writes bits least-significant first, like LandSandBoat's packBitsBE.
struct BitWriter
{
    std::array<std::byte, 128> buf{};
    std::size_t                bit = 8 * 5;
    void put(uint64_t v, unsigned n)
    {
        for (unsigned i = 0; i < n; ++i, ++bit)
        {
            if ((v >> i) & 1)
            {
                buf[bit >> 3] |= std::byte{ static_cast<uint8_t>(1u << (bit & 7)) };
            }
        }
    }
};

} // namespace

TEST_CASE("S2C 0x028 melee round parses", "[combat]")
{
    BitWriter w;
    w.put(0x01001234, 32); // actor
    w.put(1, 6);           // one target
    w.put(0, 4);
    w.put(1, 4);  // melee round
    w.put(0, 32); // action id
    w.put(0, 32); // info
    w.put(0x010A0020, 32); // target
    w.put(2, 4);           // two results
    for (const auto& [msg, dmg] : { std::pair{ 1u, 37u }, std::pair{ 15u, 0u } })
    {
        w.put(1, 3);
        w.put(0, 2);
        w.put(0, 12);
        w.put(0, 5);
        w.put(0, 2);
        w.put(0, 3);
        w.put(dmg, 17);
        w.put(msg, 10);
        w.put(0, 31);
        w.put(0, 1); // no add effect
        w.put(0, 1); // no spikes
    }
    const auto a = xi::client::parseBattleAction(std::span(w.buf.data(), (w.bit + 7) / 8));
    REQUIRE(a);
    CHECK(a->actor == 0x01001234);
    CHECK(a->actionType == 1);
    REQUIRE(a->targets.size() == 1);
    CHECK(a->targets[0].id == 0x010A0020);
    REQUIRE(a->targets[0].results.size() == 2);
    CHECK(a->targets[0].results[0].param == 37);
    CHECK(a->targets[0].results[0].message == 1);
    CHECK(a->targets[0].results[1].message == 15);
    CHECK(*xi::client::formatBasicMessage(1, "Botthree", "Wild Rabbit", 37) == "Botthree hits Wild Rabbit for 37 points of damage.");

    // Truncated packets are rejected rather than read past the end.
    CHECK_FALSE(xi::client::parseBattleAction(std::span(w.buf.data(), 20)));
}

TEST_CASE("battle line colours follow who is hit", "[combat]")
{
    // Message 1 ("hits"): HP you lose (36) when the target is us, others lose HP (42) otherwise; 7 (recovers HP): 35 / 41.
    CHECK(xi::client::battleColour(1, true, false) == 36);
    CHECK(xi::client::battleColour(1, false, true) == 42);
    CHECK(xi::client::battleColour(1, false, false) == 42);
    CHECK(xi::client::battleColour(7, true, false) == 35);
    CHECK(xi::client::battleColour(7, false, false) == 41);
    CHECK(xi::client::battleColour(0, false, false) == 0); // a fixed colour
    CHECK(xi::client::battleColour(5000, false, false) == 0);
}

TEST_CASE("castChange: start, interrupt, finish and a paralysed finish (#113)")
{
    using xi::client::BattleAction;
    auto action = [](uint8_t type, uint16_t message, uint32_t param, uint16_t animation) {
        BattleAction a;
        a.actor      = 0x10760EF;
        a.actionType = type;
        a.targets.push_back({ 0x10760EF, { xi::client::ActionResult{} } });
        auto& r     = a.targets[0].results[0];
        r.message   = message;
        r.param     = param;
        r.animation = animation;
        return a;
    };
    // LSB magic_state.cpp: the start carries "starts casting" (message 3 / 327) and the spell id.
    const auto start = xi::client::castChange(action(8, 3, 33, 0));
    CHECK(start.applies);
    CHECK(start.spell == 33);
    CHECK_FALSE(start.released);
    // interrupts.cpp MagicInterrupt: no message.
    const auto interrupt = xi::client::castChange(action(8, 0, 33, 0));
    CHECK(interrupt.applies);
    CHECK(interrupt.spell == 0);
    CHECK_FALSE(interrupt.released);
    // A normal finish releases the spell.
    const auto finish = xi::client::castChange(action(4, 2, 40, 33));
    CHECK(finish.applies);
    CHECK(finish.spell == 0);
    CHECK(finish.released);
    // MagicParalyzed / MagicIntimidated: type 4 with animation 508 and no release.
    const auto paralysed = xi::client::castChange(action(4, 29, 0, 508));
    CHECK(paralysed.applies);
    CHECK(paralysed.spell == 0);
    CHECK_FALSE(paralysed.released);
    CHECK_FALSE(xi::client::castChange(action(1, 1, 10, 0)).applies);
}
