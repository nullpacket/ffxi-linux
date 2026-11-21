// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/combat.h"

#include "dat/messages.h"

#include <fmt/format.h>

namespace xi::client
{

uint64_t BitReader::read(unsigned bits)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bits; ++i, ++bit_)
    {
        const std::size_t byte = bit_ >> 3;
        if (byte >= data_.size())
        {
            overrun_ = true;
            return 0;
        }
        const auto b = static_cast<uint8_t>(data_[byte]);
        value |= static_cast<uint64_t>((b >> (bit_ & 7)) & 1u) << i;
    }
    return value;
}

CastChange castChange(const BattleAction& a)
{
    if (a.actionType != 8 && a.actionType != 4)
    {
        return {};
    }
    const ActionResult* r0 = !a.targets.empty() && !a.targets[0].results.empty() ? &a.targets[0].results[0] : nullptr;
    CastChange          c;
    c.applies = true;
    if (a.actionType == 8 && r0 && r0->message != 0)
    {
        c.spell = static_cast<uint16_t>(r0->param);
    }
    c.released = a.actionType == 4 && !(r0 && r0->animation == 508);
    return c;
}

std::optional<BattleAction> parseBattleAction(std::span<const std::byte> packet)
{
    // Byte 4 is the payload size; the bit stream starts at byte 5 (LSB 0x028_battle2.cpp).
    BitReader    r(packet, 8 * 5);
    BattleAction a;
    a.actor             = static_cast<uint32_t>(r.read(32));
    const auto targets  = static_cast<unsigned>(r.read(6));
    r.read(4); // res_sum, always 0
    a.actionType = static_cast<uint8_t>(r.read(4));
    a.actionId   = static_cast<uint32_t>(r.read(32));
    a.info       = static_cast<uint32_t>(r.read(32));
    for (unsigned t = 0; t < targets && !r.overrun(); ++t)
    {
        ActionTarget tg;
        tg.id              = static_cast<uint32_t>(r.read(32));
        const auto results = static_cast<unsigned>(r.read(4));
        for (unsigned i = 0; i < results && !r.overrun(); ++i)
        {
            ActionResult res;
            res.resolution = static_cast<uint8_t>(r.read(3));
            res.kind       = static_cast<uint8_t>(r.read(2));
            res.animation  = static_cast<uint16_t>(r.read(12));
            res.info       = static_cast<uint8_t>(r.read(5));
            res.scale      = static_cast<uint8_t>(r.read(5)); // hit distortion (2 bits), knockback (3 bits)
            res.param    = static_cast<uint32_t>(r.read(17));
            res.message  = static_cast<uint16_t>(r.read(10));
            res.modifier = static_cast<uint32_t>(r.read(31));
            if ((res.hasAddEffect = r.read(1) != 0))
            {
                res.addEffect = static_cast<uint8_t>(r.read(6));
                r.read(4); // info
                res.addEffectParam   = static_cast<uint32_t>(r.read(17));
                res.addEffectMessage = static_cast<uint16_t>(r.read(10));
            }
            if ((res.hasSpikes = r.read(1) != 0))
            {
                res.spikesEffect = static_cast<uint8_t>(r.read(6));
                r.read(4);
                res.spikesParam   = static_cast<uint32_t>(r.read(14));
                res.spikesMessage = static_cast<uint16_t>(r.read(10));
            }
            tg.results.push_back(res);
        }
        a.targets.push_back(std::move(tg));
    }
    if (r.overrun())
    {
        return std::nullopt;
    }
    return a;
}

std::optional<std::string> formatBasicMessage(uint16_t message, std::string_view actor, std::string_view target, uint32_t param,
                                              uint32_t value)
{
    switch (message)
    {
        case 0: return std::nullopt;
        case 1: return fmt::format("{} hits {} for {} points of damage.", actor, target, param);
        case 4: return fmt::format("{} is out of range.", target);
        case 5: return fmt::format("Unable to see {}.", target);
        case 6: return fmt::format("{} defeats {}.", actor, target);
        case 8: return fmt::format("{} gains {} experience points.", target, param);
        case 9: return fmt::format("{} attains level {}!", target, param);
        case 12: return std::string("Cannot attack. Your target is already claimed.");
        case 15: return fmt::format("{} misses {}.", actor, target);
        case 20: return fmt::format("{} falls to the ground.", target);
        case 24: return fmt::format("{} recovers {} HP.", target, param);
        case 30: return fmt::format("{} anticipates the attack.", target);
        case 31: return fmt::format("{} of {}'s shadows absorbs the damage and disappears.", param, target);
        case 32: return fmt::format("{} dodges the attack.", target);
        case 33: return fmt::format("{}'s attack is countered by {}. {} takes {} points of damage.", actor, target, actor, param);
        case 36: return fmt::format("You lose sight of {}.", target);
        case 37: return std::string("You are too far from the battle to gain experience.");
        case 38: return fmt::format("{}'s {} skill rises {}.{} points.", target, dat::skillName(param), value / 10, value % 10);
        case 44: return fmt::format("{}'s spikes deal {} points of damage to {}.", target, param, actor);
        case 53: return fmt::format("{}'s {} skill reaches level {}.", target, dat::skillName(param), value);
        case 67: return fmt::format("{} scores a critical hit! {} takes {} points of damage.", actor, target, param);
        case 70: return fmt::format("{} parries {}'s attack.", target, actor);
        case 71: return std::string("You cannot perform that action on the specified target.");
        case 78: return fmt::format("{} is too far away.", target);
        case 97: return fmt::format("{} was defeated by {}.", target, actor);
        case 253: return fmt::format("EXP chain {}! {} gains {} experience points.", value, target, param);
        case 264: return fmt::format("{} takes {} points of damage.", target, param);
        default: return fmt::format("[{}] {} -> {} ({})", message, actor, target, param);
    }
}

} // namespace xi::client
