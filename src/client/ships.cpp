// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/ships.h"

#include "client/effects.h"
#include "dat/effect.h"
#include "xi/bytes.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>

namespace xi::client
{

namespace
{

// The leg easings (CODE 0x10055a00): 0 linear, 1 sin(pi t / 2), 2 1 - cos(pi t / 2), 3 piecewise sine (not traced:
// as 4), 4 0.5 - 0.5 cos(pi t).
float ease(int kind, float t)
{
    constexpr float kPi = 3.14159265f;
    switch (kind)
    {
        case 1: return std::sin(kPi * t / 2);
        case 2: return 1 - std::cos(kPi * t / 2);
        case 3:
        case 4:
        case 5: return 0.5f - 0.5f * std::cos(kPi * t);
        default: return t;
    }
}

} // namespace

std::optional<uint32_t> Ships::shipId(const Entity& e)
{
    if (e.look.kind != 4 || e.doorId > 15 || e.animation == 55)
    {
        return std::nullopt;
    }
    return e.doorId;
}

const Ships::Data& Ships::data(const dat::DatIndex& index, uint32_t ship)
{
    auto [it, fresh] = data_.try_emplace(ship);
    if (!fresh || !index.path(fileFor(ship)))
    {
        return it->second;
    }
    try
    {
        const dat::EffectFile f(index, fileFor(ship));
        for (std::size_t n = 0; n < 8; ++n)
        {
            const auto* r = f.find(dat::FxType::Scheduler, dat::fxName("seq" + std::to_string(n)));
            if (!r)
            {
                continue;
            }
            // Post-delay timing (effect-format §S.1): a record starts at the sum of the delays before it.
            const auto& p     = r->payload;
            const auto  first = load<uint32_t>(p, 0x14);
            float       t     = 0;
            for (std::size_t at = first >= 0x10 ? first - 0x10 : 0x40; at + 8 <= p.size();)
            {
                const auto w   = load<uint32_t>(p, at);
                const auto op  = w & 0xFF, len = (w >> 8) & 0x1F;
                if (op == 0 || len == 0)
                {
                    break;
                }
                if ((op == 0x26 || op == 0x27) && at + 0x1C <= p.size())
                {
                    // {+6 duration, +8 Vtx, +0x10 flags, +0x14 ease (<= 6) or a keyframe name, +0x18 yaw offset}.
                    Leg leg;
                    leg.start    = t;
                    leg.duration = static_cast<float>(std::max<int16_t>(load<int16_t>(p, at + 6), 1));
                    leg.flags    = load<uint32_t>(p, at + 0x10);
                    const auto e = load<uint32_t>(p, at + 0x14);
                    leg.ease     = e <= 6 ? static_cast<int>(std::min<uint32_t>(e, 5)) : 2; // custom curves: INFERRED ease 2
                    leg.yawOffset = load<float>(p, at + 0x18);
                    if (const auto* v = f.find(dat::FxType::Vtx, load<uint32_t>(p, at + 8)))
                    {
                        const auto count = load<uint32_t>(v->payload, 0);
                        for (std::size_t k = 0; k < count && 16 + 16 * (k + 1) <= v->payload.size(); ++k)
                        {
                            leg.path.emplace_back(load<float>(v->payload, 16 + 16 * k), load<float>(v->payload, 20 + 16 * k), load<float>(v->payload, 24 + 16 * k));
                        }
                    }
                    if (!leg.path.empty())
                    {
                        it->second.seqs[n].push_back(std::move(leg));
                    }
                }
                if (op == 0xA9 && at + 0x18 <= p.size())
                {
                    Turn turn;
                    turn.start    = t;
                    turn.duration = static_cast<float>(std::max<int16_t>(load<int16_t>(p, at + 6), 1));
                    turn.yaw      = load<float>(p, at + 0xC) * 0.017452778f;
                    turn.timed    = std::to_integer<uint8_t>(p[at + 0x14]) != 0;
                    it->second.turns[n].push_back(turn);
                }
                t += static_cast<float>(load<int16_t>(p, at + 4));
                at += 4 * len;
            }
        }
    }
    catch (const std::exception& e)
    {
        spdlog::warn("ship {}: {}", ship, e.what());
    }
    return it->second;
}

Ships::Pose Ships::pose(const dat::DatIndex& index, const Entity& e)
{
    Pose       pose{ e.pos, -static_cast<float>(e.dir) * 3.14159265f / 128.0f };
    const auto ship = shipId(e);
    if (!ship || e.animation < 18 || e.animation > 25)
    {
        return pose;
    }
    const auto& legs = data(index, *ship).seqs[static_cast<std::size_t>(e.animation - 18)];
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - 1009810800.0;
    const float  elapsed = static_cast<float>((now - static_cast<double>(e.liftTime)) * 60.0);
    if (legs.empty() || elapsed < 0)
    {
        return pose;
    }
    // The leg running now, else the last one that ran (held at its end: the game snaps to u = 1, which for a reverse leg
    // is the far end; ship_re.md recommends keeping the last point instead).
    const Leg* leg = nullptr;
    for (const auto& l : legs)
    {
        if (elapsed >= l.start)
        {
            leg = &l;
        }
    }
    if (!leg)
    {
        return pose;
    }
    const float frac    = std::clamp((elapsed - leg->start) / leg->duration, 0.0f, 1.0f);
    const bool  forward = (leg->flags & 8) != 0;
    const float t       = forward ? frac : 1.0f - frac;
    const float u       = ease(leg->ease, t);
    pose.pos            = EffectSystem::pathPoint(leg->path, u);
    // Facing: orient modes 1..3 turn the hull along the motion (yaw = offset - atan2(dz, dx)); 0 keeps it.
    if ((leg->flags >> 1) & 3)
    {
        const float     du = forward ? 0.002f : -0.002f;
        const glm::vec3 a  = EffectSystem::pathPoint(leg->path, ease(leg->ease, std::clamp(t - du, 0.0f, 1.0f)));
        const glm::vec3 b  = EffectSystem::pathPoint(leg->path, ease(leg->ease, std::clamp(t + du, 0.0f, 1.0f)));
        if (const glm::vec2 d(b.x - a.x, b.z - a.z); glm::length(d) > 1e-4f)
        {
            pose.yaw = leg->yawOffset - std::atan2(d.y, d.x);
        }
    }
    // A rotation drive (op 0xA9) that started after the leg and is not overridden by a later leg's orientation: the
    // task holds the hull at its angle (mode 0 never ends; a timed one eases from the facing at its start).
    const auto& turns = data(index, *ship).turns[static_cast<std::size_t>(e.animation - 18)];
    for (const auto& turnRec : turns)
    {
        if (elapsed < turnRec.start || (((leg->flags >> 1) & 3) != 0 && leg->start > turnRec.start))
        {
            continue;
        }
        const float k = turnRec.timed ? std::clamp((elapsed - turnRec.start) / turnRec.duration, 0.0f, 1.0f) : 1.0f;
        pose.yaw      = pose.yaw + std::remainder(turnRec.yaw - pose.yaw, 6.2831853f) * k;
    }
    return pose;
}

} // namespace xi::client
