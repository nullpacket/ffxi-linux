// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Battle packets (docs/protocol-map.md "Combat"):
//  - S2C 0x028 BATTLE2: a bit-packed action (actor, command type, per-target results).
//  - S2C 0x029 BATTLE_MESSAGE: one basic message (defeats, experience, out of range, ...).
namespace xi::client
{

struct ActionResult
{
    uint8_t  resolution = 0; // LSB ActionResolution: 0 hit, 1 miss, 2 guard, 3 parry, 4 block
    uint8_t  kind       = 0;
    uint16_t animation  = 0;
    uint8_t  info       = 0;
    uint8_t  scale      = 0; // 5 bits: hit distortion (low 2) and knockback level (high 3; effect op 0x5E, VM var 0x2C)
    uint32_t param      = 0; // damage / amount
    uint16_t message    = 0; // MsgBasic id
    uint32_t modifier   = 0;
    bool     hasAddEffect = false;
    uint8_t  addEffect    = 0; // LSB ActionProcAddEffect (melee: 1 fire ... 23) or ActionProcSkillChain (weapon skills)
    uint8_t  spikesEffect = 0;
    uint32_t addEffectParam   = 0;
    uint16_t addEffectMessage = 0;
    bool     hasSpikes    = false;
    uint32_t spikesParam   = 0;
    uint16_t spikesMessage = 0;
};

struct ActionTarget
{
    uint32_t                  id = 0;
    std::vector<ActionResult> results;
};

struct BattleAction
{
    uint32_t                  actor      = 0;
    uint8_t                   actionType = 0; // 1 melee round, 2 ranged finish, 3 WS finish, 4 magic finish, ...
    uint32_t                  actionId   = 0;
    uint32_t                  info       = 0;
    std::vector<ActionTarget> targets;
};

// Reads values packed least-significant bit first, the way LandSandBoat's packBitsBE writes them.
class BitReader
{
public:
    BitReader(std::span<const std::byte> data, std::size_t bitOffset)
    : data_(data)
    , bit_(bitOffset)
    {
    }

    uint64_t read(unsigned bits);
    [[nodiscard]] bool overrun() const
    {
        return overrun_;
    }
    [[nodiscard]] std::size_t bit() const
    {
        return bit_;
    }

private:
    std::span<const std::byte> data_;
    std::size_t                bit_     = 0;
    bool                       overrun_ = false;
};

// Parses a whole S2C 0x028 packet (including its 4-byte header). Returns nothing when the packet is truncated.
std::optional<BattleAction> parseBattleAction(std::span<const std::byte> packet);

// What an action does to its actor's casting state (docs/effect-format.md §C.1-C.3). Type 8 with a message starts a cast
// (result param = spell id); type 8 without one interrupts it (LSB action/interrupts.cpp MagicInterrupt, cmd_arg `sp..`);
// type 4 finishes it. A type 4 whose animation is 508 (LSB ActionAnimation::SkillInterrupt: paralysed or intimidated at
// the end of the cast) ends the cast without a release: its effect file 2800 + 508 has no op 0x3C `sh..` (DATA).
struct CastChange
{
    bool     applies  = false; // the action changes the casting state at all
    uint16_t spell    = 0;     // the spell now being cast, 0 = none
    bool     released = false; // the spell went off: the caster plays its release motion
};
CastChange castChange(const BattleAction& a);

// Renders a MsgBasic message as combat-log text, or nothing for ids we do not know yet.
// The real client reads these strings from the message DATs; this is a small built-in subset.
std::optional<std::string> formatBasicMessage(uint16_t message, std::string_view actor, std::string_view target, uint32_t param,
                                              uint32_t value = 0);

} // namespace xi::client
