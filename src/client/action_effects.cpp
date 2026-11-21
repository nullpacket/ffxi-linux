// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/action_effects.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <utility>

namespace xi::client
{

void ActionEffects::reset()
{
    spawnSeen_.clear();
    casting_.clear();
    motionWindow_.clear();
    dying_.clear();
}

void ActionEffects::tick(float dt)
{
    for (auto it = motionWindow_.begin(); it != motionWindow_.end();)
    {
        it->second -= dt;
        it = it->second <= 0 ? motionWindow_.erase(it) : std::next(it);
    }
}

void ActionEffects::update(EffectSystem& fx, const WorldSnapshot& world, bool settled, const std::function<bool(uint32_t)>& posed,
                           std::unordered_set<uint64_t>& shownResults, bool trace, const std::function<std::string(uint32_t)>& filesText)
{
    // A character appearing after zone-in fades in with its model's `pop0` (black, invisible -> normal over
    // 86 ticks via op 0x29; monster model files and the PC race files both have it). INFERRED trigger:
    // first seen 3 s or more after zone-in.
    for (const auto& e : world.entities)
    {
        if (e.hpp == 0)
        {
            spawnSeen_.erase(e.id); // dead: its respawn (same id) fades in again
            continue;
        }
        if (!e.look.valid || !posed(e.id))
        {
            continue; // not posed yet (model loading or out of range): its files are unknown
        }
        if (spawnSeen_.insert(e.id).second)
        {
            // pop0 ends by starting `init` (the model's standing effects, e.g. 2587 `fire`); characters
            // already there at zone-in run `init` alone.
            const char* stem   = settled ? "pop0" : "init";
            const bool  played = fx.playFromActor(e.id, dat::fxName(stem), e.id, e.id);
            fx.startModelGenerators(e.id); // its model's own generators, after `init` (op 0x9A can turn them off)
            if (trace)
            {
                spdlog::info("fx: {:x} appears, {} {}", e.id, stem, played);
            }
        }
    }
    // S2C 0x05B appear modes: 3 `pop0` from the model, 6 `pop1` (file 0: alpha 0 -> 0x80 with the refraction
    // outline, sched 0x48). CODE 0x1009095b.
    const auto freshPos = std::min<std::size_t>(world.posEffectSerial - posSeen_, world.posEffects.size());
    posSeen_            = world.posEffectSerial;
    for (std::size_t i = world.posEffects.size() - freshPos; i < world.posEffects.size(); ++i)
    {
        const auto& p      = world.posEffects[i];
        const char* stem   = p.mode == 6 ? "pop1" : "pop0";
        const bool  played = fx.playFromActor(p.id, dat::fxName(stem), p.id, p.id);
        if (trace)
        {
            spdlog::info("fx: {:x} position mode {}, {} {}", p.id, p.mode, stem, played);
        }
    }
    for (const auto id : std::exchange(dying_, {}))
    {
        // Effect op 0xAC picks `1ead` .. `3ead` instead (entity +0x130 bits 5-6, CODE 0x1008ca55, table 0x1032d5f0).
        static constexpr std::array<const char*, 4> kDeath{ "dead", "1ead", "2ead", "3ead" };
        const bool played = fx.playFromActor(id, dat::fxName(kDeath[static_cast<std::size_t>(fx.motionVariant(id, 4) & 3)]), id, id)
                            || fx.playFromActor(id, dat::fxName("dead"), id, id);
        if (trace)
        {
            spdlog::info("fx: {:x} dies, dead {}", id, played);
        }
    }
    const auto fresh = std::min<std::size_t>(world.actionSerial - actionSeen_, world.actions.size());
    actionSeen_       = world.actionSerial;
    for (std::size_t i = world.actions.size() - fresh; i < world.actions.size(); ++i)
    {
        const auto&    a      = world.actions[i];
        const uint32_t target = a.targets.empty() ? a.actor : a.targets[0].id;
        const auto*    r0     = !a.targets.empty() && !a.targets[0].results.empty() ? &a.targets[0].results[0] : nullptr;
        const uint16_t anim   = r0 ? r0->animation : 0;
        // Expression VM variables from a result (effect-format §S.5).
        auto varsOf = [](const ActionResult& r) {
            FxVars v{};
            v[0x28] = r.resolution, v[0x29] = r.kind, v[0x2A] = r.animation, v[0x2B] = r.info, v[0x2C] = r.scale;
            v[0x2D] = static_cast<int32_t>(r.param), v[0x2E] = r.message;
            v[0x2F] = r.hasAddEffect ? r.addEffect : 0, v[0x31] = static_cast<int32_t>(r.addEffectParam);
            v[0x32] = r.addEffectMessage, v[0x33] = r.hasSpikes ? r.spikesEffect : 0;
            v[0x35] = static_cast<int32_t>(r.spikesParam), v[0x36] = r.spikesMessage;
            return v;
        };
        // Every (target, result) pair, for op 0x32's loop over targets / hits; var 0x27 = target count.
        FxAction vars;
        vars.actor = a.actor;
        vars.tag   = static_cast<uint64_t>(world.actionSerial - (world.actions.size() - 1 - i)) << 16; // as Session's ChatLine::hold
        for (const auto& t : a.targets)
        {
            for (const auto& r : t.results)
            {
                auto v  = varsOf(r);
                v[0x27] = static_cast<int32_t>(a.targets.size());
                vars.results.push_back({ t.id, v });
            }
        }
        // Melee rounds and the other actions whose messages log at once (Session: not held for op 0x2B): their
        // results count as shown now (damage pop-ups).
        if (!((a.actionType >= 2 && a.actionType <= 6) || a.actionType == 11 || a.actionType == 13))
        {
            for (std::size_t k = 0; k < vars.results.size(); ++k)
            {
                released_.push_back(vars.tag | (k + 1));
            }
        }
        const Look* look = a.actor == world.charId ? &world.look : nullptr;
        bool                pc   = a.actor == world.charId;
        for (const auto& e : world.entities)
        {
            if (e.id == a.actor)
            {
                look = &e.look;
                pc   = e.isPc;
            }
        }
        if (trace)
        {
            spdlog::info("fx: action type {} id {:x} ({}) actor {:x} anim {}, files {}", a.actionType, a.actionId, dat::fxNameString(a.actionId), a.actor, anim,
                         filesText(a.actor));
        }
        // Start actions (cast / ready / item / ranged start): cmd_arg is the 4CC of a scheduler in the caster's
        // own files (`cawh`, `cate`, `cait`, `calg` ...), `sp..` on interrupt (effect-format §C.5). A new one
        // first stops the previous one's `st..` partner.
        if (a.actionType == 7 || a.actionType == 8 || a.actionType == 9 || a.actionType == 12)
        {
            const dat::FxName name = a.actionId;
            if (const auto prev = casting_.find(a.actor); prev != casting_.end())
            {
                if ((name & 0xFFFF) == (dat::fxName("ca") & 0xFFFF))
                {
                    fx.play(0, "st" + dat::fxNameString(prev->second).substr(2, 2), a.actor, a.actor);
                }
                casting_.erase(prev);
            }
            const bool started = (name & 0xFFFF) == (dat::fxName("ca") & 0xFFFF) && fx.playFromActor(a.actor, name, a.actor, a.actor);
            spdlog::debug("fx: start action {} {} for {:x}: {}", a.actionType, dat::fxNameString(name), a.actor, started);
            if (started)
            {
                casting_[a.actor] = name;
            }
            else if ((name & 0xFFFF) == (dat::fxName("sp") & 0xFFFF))
            {
                fx.playFromActor(a.actor, name, a.actor, a.actor);
            }
        }
        else if (a.actionType >= 2 && a.actionType <= 6)
        {
            casting_.erase(a.actor); // the finish's own scheduler takes over
        }
        // Scheduler motions drive the actor only for item use and job abilities; melee, weapon skills,
        // casting and TP moves already animate from their own state.
        if (a.actionType == 5 || a.actionType == 6)
        {
            motionWindow_[a.actor] = 6.0f;
        }
        else
        {
            motionWindow_.erase(a.actor);
        }
        bool played = true;
        switch (a.actionType)
        {
            case 1: // melee round (§ Melee hit effects): the attacker's `atk0` (cmd_arg) picks a swing per
                    // result (op 0x24, looped by op 0x32); the swing plays its sound and file 0 `dada`.
                if (fx.playFromActor(a.actor, a.actionId, a.actor, target, &vars))
                {
                    break;
                }
                // Without the attacker's files: per result at the swing's contact, the weapon's `ef h` =
                // file 0 `hit1` on a hit, then file 0 `dada` driven by the result.
                for (const auto& t : a.targets)
                {
                    for (const auto& r : t.results)
                    {
                        // About 15 ticks in (INFERRED from the swing motions).
                        if (r.resolution == 0) // LSB ActionResolution: 0 hit, 1 miss, 2 guard, 3 parry, 4 block
                        {
                            fx.play(0, "hit1", a.actor, t.id, 0.25f);
                        }
                        const FxAction rv{ { { t.id, varsOf(r) } }, a.actor };
                        fx.play(0, "dada", a.actor, t.id, 0.25f, &rv);
                    }
                }
                break;
            case 2: // ranged finish: no effect file; the shooter's `shlg` runs `lsNN` (§C.5)
                played = fx.playFromActor(a.actor, dat::fxName("shlg"), a.actor, target, &vars);
                break;
            case 3: // weapon skill: a PC's per-race file (dat-format §10.8), else 3400 + animation
            {
                static constexpr uint32_t kRaceBase[9] = { 0, 33227, 33995, 34763, 35531, 36299, 36299, 37067, 37835 };
                const uint8_t race = look ? static_cast<uint8_t>(look->ids[0] >> 8) : 0;
                const uint32_t f   = pc && race >= 1 && race <= 8 && anim < 256 ? kRaceBase[race] + anim : 3400u + anim;
                played = fx.play(f, "main", a.actor, target, 0.0f, &vars);
                break;
            }
            case 4: played = fx.play(2800u + anim, "main", a.actor, target, 0.0f, &vars); break; // magic
            case 5: played = fx.play((anim >= 100 ? 56485u : 4912u) + anim, "main", a.actor, target, 0.0f, &vars); break; // item
            case 6: played = fx.play(4412u + anim, "main", a.actor, target, 0.0f, &vars); break; // job ability
            case 7:
            case 11: // readying / monster TP move
            {
                const uint32_t f = anim >= 0x800 ? 84743u + (anim - 0x800u) : anim >= 0x600 ? 59193u + (anim - 0x600u)
                                 : anim >= 0x200 ? 49647u + (anim - 0x200u)  : 3900u + anim;
                played = fx.play(f, "main", a.actor, target, 0.0f, &vars);
                break;
            }
            case 13: played = fx.play(49391u + anim, "main", a.actor, target, 0.0f, &vars); break; // pet skill
            default: break;
        }
        if (!played)
        {
            // No effect will show the damage (ChatLine::hold): log the results now.
            for (std::size_t k = 0; k < vars.results.size(); ++k)
            {
                shownResults.insert(vars.tag | (k + 1));
                released_.push_back(vars.tag | (k + 1));
            }
        }
    }
}

} // namespace xi::client
