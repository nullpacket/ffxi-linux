// SPDX-License-Identifier: GPL-3.0-or-later
// Scheduler ops that act on the caster / target actor rather than on particles (effect-format "Coverage, part 7A"):
// motion sets, texture overlays and their scroll, the refraction pass, knockback, head look-at, attach points,
// counters, draw / sheathe, the actor's own model generators. CODE addresses are the handlers of jump table 0x1005db4c.
#include "client/effects.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace xi::client
{

namespace
{

template <typename T>
T rd(std::span<const std::byte> s, std::size_t at)
{
    T v{};
    if (at + sizeof(T) <= s.size())
    {
        std::memcpy(&v, s.data() + at, sizeof(T));
    }
    return v;
}

// Op 0x5E's distances by knockback level (CODE table 0x1035bc70, 12 bytes each): speed per tick, the decay per tick
// after the hold, the hold in ticks.
struct KnockLevel
{
    float speed, decay, hold;
};
constexpr std::array<KnockLevel, 8> kKnockLevels{ { { 0.0f, 0.0f, 0.0f },
                                                    { 1.0f / 12.0f, 0.075f, 5.0f },
                                                    { 1.0f / 6.0f, 0.15f, 5.0f },
                                                    { 1.0f / 6.0f, 0.15f, 10.0f },
                                                    { 1.0f / 6.0f, 0.15f, 18.0f },
                                                    { 1.0f / 6.0f, 0.125f, 30.0f },
                                                    { 1.0f / 6.0f, 0.1f, 35.0f },
                                                    { 1.0f / 6.0f, 0.05f, 45.0f } } };

// Model generators that start by themselves (CODE 0x10084f90, switch tables 0x10085114 / 0x10085130): origin mode m
// = (origin bit 16) << 4 | (origin & 0xF), started when m - 1 indexes a 0 there.
bool modelGeneratorMode(uint32_t origin)
{
    const uint32_t m = (((origin >> 16) & 1) << 4) | (origin & 0xF);
    if (m == 0 || m > 0x13)
    {
        return false;
    }
    return !(m == 3 || m == 6 || m == 13 || m == 14 || m == 15);
}

} // namespace

const EffectSystem::FxStance* EffectSystem::stance(uint32_t actor) const
{
    const auto it = stances_.find(actor);
    return it == stances_.end() ? nullptr : &it->second;
}

std::optional<EffectSystem::FxOverlay> EffectSystem::actorOverlay(uint32_t actor) const
{
    const auto it = overlays_.find(actor);
    if (it == overlays_.end() || !it->second.texture)
    {
        return std::nullopt;
    }
    FxOverlay o{ it->second.texture, glm::vec2(0.0f), it->second.additive };
    if (const auto t = tints2_.find(actor); t != tints2_.end())
    {
        o.colour = t->second.now();
    }
    if (const auto sc = scrolls_.find(actor); sc != scrolls_.end())
    {
        o.scroll = glm::vec2(sc->second[0].now(), sc->second[1].now());
    }
    return o;
}

float EffectSystem::actorShimmer(uint32_t actor) const
{
    const auto it = shimmer_.find(actor);
    return it == shimmer_.end() ? 0.0f : it->second.now();
}

glm::vec3 EffectSystem::actorDisplacement(uint32_t actor) const
{
    const auto it = knockbacks_.find(actor);
    return it == knockbacks_.end() ? glm::vec3(0.0f) : it->second.offset;
}

void EffectSystem::clearDisplacement(uint32_t actor)
{
    if (const auto it = knockbacks_.find(actor); it != knockbacks_.end() && !it->second.active)
    {
        knockbacks_.erase(it);
    }
}

bool EffectSystem::lookAtSuppressed(uint32_t actor) const
{
    return lookOff_.contains(actor);
}

int EffectSystem::actorPoint(uint32_t actor, int which) const
{
    const auto it = points_.find(actor);
    return it == points_.end() || which < 0 || which > 1 ? -1 : it->second[static_cast<std::size_t>(which)];
}

void EffectSystem::resetActorState()
{
    stances_.clear(), overlays_.clear(), scrolls_.clear(), shimmer_.clear(), knockbacks_.clear(), lookOff_.clear();
    points_.clear(), noModelGenerators_.clear(), pendingModelGenerators_.clear(), acting_.clear();
}

void EffectSystem::actorOp(Scheduler& s, uint8_t op, int16_t dur)
{
    const auto  p     = s.res->payload;
    const auto  at    = s.pc;
    const auto  v     = rd<uint32_t>(p, at + 8);
    const float ticks = static_cast<float>(std::max<int16_t>(dur, 0)) * s.rate;
    const bool  trace = std::getenv("XI_FX_TRACE") != nullptr;
    switch (op)
    {
        case 0x40: // texture overlay on the caster (0x40 / 0x54) or target (0x41 / 0x55) for the duration (CODE 0x1005bb81 ..
        case 0x41: // 0x1005bb2f -> task 0x100616e0: actor vfunc +0x64 texture, +0x7c blend, +0x4c(1) on; the task's end
        case 0x54: // clears the texture if it is still its own, 0x10061660). +8 the texture's chunk name; 0x54 / 0x55
        case 0x55: // give the blend at +0x10, 0x40 / 0x41 use 0x48 (additive).
        {
            const uint32_t     actor = op == 0x40 || op == 0x54 ? s.caster : s.target;
            const dat::Texture* tex  = s.file ? s.file->textureByChunk(v) : nullptr;
            if (!tex && actorFiles_)
            {
                for (const auto id : actorFiles_(actor))
                {
                    if (const auto* f = file(id); f && (tex = f->textureByChunk(v)))
                    {
                        break;
                    }
                }
            }
            if (const auto* f0 = tex ? nullptr : file(0))
            {
                tex = f0->textureByChunk(v);
            }
            const uint32_t blend = op == 0x54 || op == 0x55 ? rd<uint32_t>(p, at + 0x10) : 0x48u;
            if (tex && actor)
            {
                overlays_[actor] = { tex, blend == 0x48 || blend == 0x1d, std::max(ticks, 1.0f) };
            }
            if (trace)
            {
                spdlog::info("fx: {} overlay {} on {:x} blend {:x} ({} ticks): {}", dat::fxNameString(s.res->name), dat::fxNameString(v), actor, blend, dur,
                             tex != nullptr);
            }
            break;
        }
        case 0x42: // the overlay's texture scroll on the caster (0x42 / 0x43) or target (0x44 / 0x45): from the current value
        case 0x43: // to +8 over the duration, wrapped to 2 (CODE 0x1005ba68 / 0x1005bab4 -> task 0x100614e0, tick
        case 0x44: // 0x10061560: actor vfuncs +0x410 / +0x414 = +0x73c, the 0x43 / 0x45 flag +0x418 / +0x41c = +0x740;
        case 0x45: // the draw passes both as the texture matrix's translation, 0x100d6dc4).
        {
            const uint32_t actor = op <= 0x43 ? s.caster : s.target;
            auto&          d     = scrolls_[actor][op == 0x43 || op == 0x45 ? 1 : 0];
            d                    = { d.now(), rd<float>(p, at + 8), 0.0f, ticks };
            break;
        }
        case 0x48: // the refraction pass strength on the caster (0x48) / target (0x49), current -> +8 over the duration
        case 0x49: // (CODE 0x1005b377 -> task 0x10061170: actor +0x744, flag +0x734 bit 1, cleared at the end when 0)
        {
            const uint32_t actor = op == 0x48 ? s.caster : s.target;
            auto&          d     = shimmer_[actor];
            d                    = { d.now(), rd<float>(p, at + 8), 0.0f, ticks };
            break;
        }
        case 0x4A: // the 0x0B sound at the target, only when the target is our own character (CODE 0x1005b50a: actor
        {          // vtable +0x304, then the 0x0B path 0x1005b54c)
            if (s.target == 0 || s.target != localPlayer_ || sounds_.size() >= 32)
            {
                break;
            }
            auto [f, r] = lookup(s.file, s.res->block, dat::FxType::Sep, v);
            if (!r)
            {
                std::tie(f, r) = lookupActor(s.target, dat::FxType::Sep, v);
            }
            if (r)
            {
                const float far = rd<float>(p, at + 0x14), near = rd<float>(p, at + 0x18);
                sounds_.push_back({ .actor = s.target, .name = v, .sep = r->payload, .near = near > 0 && near < 500 ? near : 3.0f, .far = far > 0 && far < 500 ? far : 30.0f, .at = {}, .gain = 1.0f });
            }
            break;
        }
        case 0x5C: // a counter: the caster's scheduler `cn?0` (CODE 0x1005a784): not engaged `cni0`; engaged by actor
        {          // +0x598 (2 `cnr0`, 3 `cnb0`, 4 `cnl0`, else `cnf0`), `cnf0` for some model kinds. The engaged state and
                   // +0x598 are not known here: `cnf0`, else `cni0` (INFERRED: counters happen in battle).
            for (const char* n : { "cnf0", "cni0" })
            {
                if (const auto [f, r] = lookupActor(s.caster, dat::FxType::Scheduler, dat::fxName(n)); r)
                {
                    startScheduler(f, r, s.caster, s.target, s.depth + 1);
                    break;
                }
            }
            break;
        }
        case 0x5E: // knockback of the target from the caster (CODE 0x1005aa93 -> 0x100aa810 / 0x100aa870, tick 0x100aaaa0)
        {
            if (!actors_ || !s.target || !s.caster || s.target == s.caster)
            {
                break;
            }
            const auto tgt = (*actors_)(s.target);
            const auto src = (*actors_)(s.caster);
            if (!tgt || !src)
            {
                break;
            }
            const bool pull = rd<int16_t>(p, at + 0xA) != 0;
            KnockLevel k{ rd<float>(p, at + 0x10), rd<float>(p, at + 0xC) / 60.0f, rd<float>(p, at + 0x14) };
            if (rd<int16_t>(p, at + 8) == 0)
            {
                // By the result's knockback level: var 0x2C (r+0xA) >> 2 (CODE 0x1005e290), 1 without an action.
                int level = 1;
                if (s.action && s.result < s.action->results.size())
                {
                    level = s.action->results[s.result].vars[0x2C] >> 2;
                }
                if (level <= 0 || level >= 8)
                {
                    break; // level 0: no knockback (the table's zero row); >= 8 is skipped (CODE 0x100aa815)
                }
                k = kKnockLevels[static_cast<std::size_t>(level)];
            }
            if (pull)
            {
                k.speed = -k.speed;
            }
            // Motion `bf0?` facing the caster, `bb0?` from behind (0x100ce0b0, the flinch's facing test); a pull swaps them.
            const glm::vec3 to(src->pos.x - tgt->pos.x, 0.0f, src->pos.z - tgt->pos.z);
            const bool      behind = std::cos(tgt->yaw) * to.x + std::sin(tgt->yaw) * to.z < 0.0f;
            const char*     name   = (behind != pull) ? "bb0" : "bf0";
            // Direction source -> target (CODE 0x10027440 normalises the 3-D difference; here level, INFERRED).
            glm::vec3 dir(tgt->pos.x - src->pos.x, 0.0f, tgt->pos.z - src->pos.z);
            if (glm::length(dir) < 1e-4f)
            {
                dir = glm::vec3(std::cos(tgt->yaw), 0.0f, std::sin(tgt->yaw)) * -1.0f;
            }
            dir = glm::normalize(dir);
            auto& kb    = knockbacks_[s.target];
            kb.velocity = dir * k.speed;
            kb.decay    = k.decay;
            kb.hold     = k.hold != 0.0f ? k.hold : 1.0f;
            kb.source   = src->pos;
            kb.near     = k.speed != 0.0f;
            kb.active   = true;
            if (motions_.size() < 64)
            {
                FxMotion m;
                m.actor     = s.target;
                m.stem      = name;
                m.knockback = true;
                motions_.push_back(m);
            }
            if (trace)
            {
                spdlog::info("fx: {} knockback {:x} from {:x}: {} speed {:.3f} decay {:.3f} hold {}", dat::fxNameString(s.res->name), s.target, s.caster,
                             name, k.speed, k.decay, kb.hold);
            }
            break;
        }
        case 0x6E: // the caster walks along its facing at +8 yalms a second for `dur` ticks (CODE 0x1005ae10 -> 0x100aac70:
        {          // entity +0x101 on, +0x118 ticks left, +0x120 = (1, 0, 0) through the actor's rotation x +8 / 60; the
                   // entity's position update 0x100a59eb / 0x100a5aa3 -> 0x100aad20 adds it once a tick until the ticks run
                   // out). DATA 39275 `cdam` (8 / s for 120 ticks while fading out), `coff` (12 / s for 72)
            const auto a = actors_ && *actors_ ? (*actors_)(s.caster) : std::nullopt;
            if (!a || dur <= 0)
            {
                break;
            }
            auto& kb    = knockbacks_[s.caster];
            kb.velocity = glm::vec3(std::cos(a->yaw), 0.0f, std::sin(a->yaw)) * (rd<float>(p, at + 8) / 60.0f);
            kb.hold     = static_cast<float>(dur);
            kb.decay    = 1.0f; // stops dead when the ticks run out
            kb.near     = false;
            kb.active   = kb.velocity != glm::vec3(0.0f);
            break;
        }
        case 0x79: // the caster's battle (0x79) / idle (0x8C) motion set by +8 (CODE 0x1005bf76 / 0x1005bfaa: tables
        case 0x8C: // 0x1032b804 "btl " "bt1 " "bt2 " "bt3 ", 0x1032b814 "idl " "id1 " ..; actor +0x7d4 / +0x7c8)
        {
            const uint32_t     table = op == 0x79 ? 0 : 1;
            static const char* kNames[2][4] = { { "btl", "bt1", "bt2", "bt3" }, { "idl", "id1", "id2", "id3" } };
            const std::string  stem         = v < 4 ? kNames[table][v] : "";
            auto&              st           = stances_[s.caster];
            (op == 0x79 ? st.battle : st.idle) = stem == kNames[table][0] ? std::string() : stem;
            if (trace)
            {
                spdlog::info("fx: {} {} set {} on {:x}", dat::fxNameString(s.res->name), op == 0x79 ? "battle" : "idle", stem, s.caster);
            }
            break;
        }
        case 0x84: // draw / sheathe: the caster's scheduler in1N / ot1N (main, +0xA 0) or in2N / ot2N (sub), +8 0 in / 1 out,
        {          // N = '0' + the weapon's sheath style (CODE 0x1005a9c3: actor +0x87c / +0x87f, max(0, s8))
            const bool sub   = rd<int16_t>(p, at + 0xA) != 0;
            const bool out   = rd<int16_t>(p, at + 8) != 0;
            int        style = 0;
            if (sheathStyles_)
            {
                style = std::max(0, sheathStyles_(s.caster)[sub ? 1 : 0]);
            }
            const std::string n = std::string(out ? "ot" : "in") + (sub ? '2' : '1') + static_cast<char>('0' + std::min(style, 9));
            if (const auto [f, r] = lookupActor(s.caster, dat::FxType::Scheduler, dat::fxName(n)); r)
            {
                startScheduler(f, r, s.caster, s.target, s.depth + 1);
            }
            break;
        }
        case 0x89: // no head look-at on the caster for the duration or until it moves a yalm in x or z (CODE 0x1005b07c
        {          // -> 0x1005f380: actor +0x840 |= 2, tick 0x1005f470; ranged shots ls01..ls06)
            auto& lo     = lookOff_[s.caster];
            lo.remaining = std::max(ticks, 1.0f);
            lo.start.reset();
            if (actors_)
            {
                if (const auto a = (*actors_)(s.caster))
                {
                    lo.start = a->pos;
                }
            }
            break;
        }
        case 0x9A: // +8 1: the actor's model generators do not start by themselves (CODE 0x1005c4bb -> vtable +0x120:
        {          // model +0x130 bit 1, read by 0x10084f90 through vtable +0x124)
            if (v != 0)
            {
                noModelGenerators_.insert(s.caster);
                for (auto& g : generators_)
                {
                    if (g->model && g->caster == s.caster)
                    {
                        g->stopped = true;
                    }
                }
            }
            else
            {
                noModelGenerators_.erase(s.caster);
            }
            break;
        }
        case 0x9E: // skeleton points: 0x9E the target arrow's (+0x8a6), 0x9F the other projected anchor's (+0x8a7)
        case 0x9F: // (CODE 0x1005c639 / 0x1005c669 -> vtable +0xb0 / +0xb8)
        {
            auto& pts = points_.try_emplace(s.caster, std::array<int, 2>{ -1, -1 }).first->second;
            pts[op == 0x9E ? 0 : 1] = static_cast<int>(v & 0xFF);
            break;
        }
        // Applied when the model loads (dat::effectLook: the look of effects-only NPC model files is their `init`):
        // 0x8D race, 0x8E-0x96 look slots, 0x98 an NPC model id, 0x99 the scale (CODE 0x1005c15b..0x1005c39b).
        case 0x8D: case 0x8E: case 0x8F: case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96:
        case 0x98: case 0x99:
            break;
        // Nothing drawn: 0x86 sets model +0x128 bit 7 and 0xBE model +0x13c bit 18, which nothing in the client reads;
        // 0x88's task only resets the caster's colour to 0x80808080 if it is deleted before its time runs out.
        case 0x86: case 0x88: case 0xBE:
            break;
        default: break;
    }
}

void EffectSystem::tickActorState(float ticks)
{
    for (auto it = overlays_.begin(); it != overlays_.end();)
    {
        it->second.remaining -= ticks;
        it = it->second.remaining <= 0 ? overlays_.erase(it) : std::next(it);
    }
    for (auto& [id, ds] : scrolls_)
    {
        for (auto& d : ds)
        {
            d.elapsed = std::min(d.elapsed + ticks, d.duration);
            // Wrapped to 2 (CODE 0x10061597: fmod with the double 2.0 at 0x1032abc8).
            const float n = std::fmod(d.now(), 2.0f);
            if (d.elapsed >= d.duration)
            {
                d = { n, n, 0.0f, 0.0f };
            }
        }
    }
    for (auto it = shimmer_.begin(); it != shimmer_.end();)
    {
        it->second.elapsed = std::min(it->second.elapsed + ticks, it->second.duration);
        const bool off     = it->second.elapsed >= it->second.duration && it->second.to == 0.0f;
        it                 = off ? shimmer_.erase(it) : std::next(it);
    }
    // Knockback (CODE 0x100aaaa0): `hold` ticks at full speed, then the velocity loses `decay` of itself each tick until
    // it is under 9e-6; a step that would bring a pulled actor within 0.81 of the source is skipped.
    for (auto& [id, kb] : knockbacks_)
    {
        if (!kb.active)
        {
            continue;
        }
        const glm::vec3 base = actors_ ? (*actors_)(id).value_or(FxActor{}).pos : glm::vec3(0.0f);
        for (float t = ticks; t > 0.0f && kb.active; t -= 1.0f)
        {
            const float     step = std::min(t, 1.0f);
            const glm::vec3 next = kb.offset + kb.velocity * step;
            if (!(kb.near && glm::distance(base + next, kb.source) < 0.81f))
            {
                kb.offset = next;
            }
            if (kb.hold > 0.0f)
            {
                kb.hold -= step;
            }
            else
            {
                kb.velocity -= kb.velocity * (kb.decay * step);
            }
            kb.active = glm::dot(kb.velocity, kb.velocity) >= 9e-6f && glm::length(kb.offset) < 30.0f;
        }
    }
    for (auto it = lookOff_.begin(); it != lookOff_.end();)
    {
        it->second.remaining -= ticks;
        bool moved = false;
        if (actors_ && it->second.start)
        {
            if (const auto a = (*actors_)(it->first))
            {
                moved = std::abs(a->pos.x - it->second.start->x) > 1.0f || std::abs(a->pos.z - it->second.start->z) > 1.0f;
            }
        }
        it = it->second.remaining <= 0 || moved ? lookOff_.erase(it) : std::next(it);
    }
    // Model generators end with their actor.
    if (actors_)
    {
        for (auto& g : generators_)
        {
            if (g->model && !g->stopped && !(*actors_)(g->caster))
            {
                g->stopped = true;
            }
        }
    }
    // The actor's model generators (CODE 0x10084f90): every type 5 resource in its files with flag 0x1000 and a
    // started origin mode, with the actor as caster and target; re-triggered when already running.
    for (const auto actor : std::exchange(pendingModelGenerators_, {}))
    {
        if (!actorFiles_ || noModelGenerators_.contains(actor))
        {
            continue;
        }
        for (const auto id : actorFiles_(actor))
        {
            const auto* f = file(id);
            if (!f)
            {
                continue;
            }
            for (const auto& r : f->resources())
            {
                if (r.type != dat::FxType::Generator || r.payload.size() < 0x80 || !(rd<uint32_t>(r.payload, 0x68) & 0x1000) ||
                    !modelGeneratorMode(rd<uint32_t>(r.payload, 0x00)))
                {
                    continue;
                }
                if (auto* g = startGenerator(f, &r, actor, actor, 0.0f))
                {
                    g->model = true;
                    if (std::getenv("XI_FX_TRACE"))
                    {
                        spdlog::info("fx: model generator {} (file {}) on {:x}", dat::fxNameString(r.name), id, actor);
                    }
                }
            }
        }
    }
}

} // namespace xi::client
