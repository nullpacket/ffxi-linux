// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/doors.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace xi::client
{

namespace
{

bool opens(uint8_t a)
{
    return a == 8 || a == 26 || a == 29 || a == 45;
}

bool closes(uint8_t a)
{
    return a == 9 || a == 27 || a == 30 || a == 46;
}

} // namespace

void Doors::updateLifts(const WorldSnapshot& world, const LiftHooks& hooks)
{
    // dat-format §5.8 lifts: state = animation - 10; on a change the platform moves linearly from Time to Time + travel
    // (whole earth seconds since the Vana'diel epoch, CODE 0x1008b448 / task 0x10060020); at spawn it stands at its
    // floor. The scheduler `mvXY` (sound, op 0x1D) is not run here: the height comes from the Rid states directly.
    if (!hooks.heights || !hooks.setY)
    {
        return;
    }
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - 1009810800.0;
    for (const auto& e : world.entities)
    {
        if (e.look.kind != 3 || e.doorId == 0 || e.animation < 10 || e.animation > 17)
        {
            continue;
        }
        const auto heights = hooks.heights(e.doorId);
        const auto state   = static_cast<std::size_t>(e.animation - 10);
        if (state >= heights.size())
        {
            continue;
        }
        auto [it, fresh] = lifts_.try_emplace(e.id);
        auto& l          = it->second;
        if (fresh)
        {
            l = { e.doorId, e.animation, heights[state], heights[state], heights[state], 0, 0 };
        }
        else if (e.animation != l.animation)
        {
            l.animation = e.animation;
            l.fromY     = l.y;
            l.toY       = heights[state];
            l.start     = std::max(now, static_cast<double>(e.liftTime));
            l.end       = std::max(l.start, static_cast<double>(e.liftTime) + e.liftTravel);
            spdlog::debug("lift {} state {}: {:.2f} -> {:.2f} over {:.1f} s (starts in {:.1f} s)", dat::fxNameString(l.fourcc), state, l.fromY, l.toY,
                          l.end - l.start, l.start - now);
        }
        if (l.end > l.start && now < l.end)
        {
            const double u = std::clamp((now - l.start) / (l.end - l.start), 0.0, 1.0);
            l.y            = l.fromY + static_cast<float>(u) * (l.toY - l.fromY);
        }
        else
        {
            l.y = l.toY;
        }
        hooks.setY(l.fourcc, l.y);
    }
}

void Doors::update(const WorldSnapshot& world, EffectSystem& fx, float dt, const PartsOf& parts, const Pose& pose, const Solid& solid,
                   const LiftHooks& lifts)
{
    updateLifts(world, lifts);
    const bool trace = std::getenv("XI_FX_TRACE") != nullptr;
    // Part moves (op 0x0D rotation, 0x0C offset): from the part's current value, linearly (CODE tick 0x10043a30).
    for (const auto& m : fx.takePartMoves())
    {
        const auto it = doors_.find(m.actor);
        if (it == doors_.end() || m.part >= it->second.parts.size())
        {
            continue;
        }
        auto& p = it->second.parts[m.part];
        (m.rotation ? p.rotTask : p.offTask) = Task{ m.rotation ? p.rot : p.offset, m.target, 0.0f, std::max(m.seconds, 0.001f) };
    }
    for (const auto& e : world.entities)
    {
        if (e.look.kind != 2 || e.doorId == 0)
        {
            continue;
        }
        // Invisible gates (Selbina `_6ww`) have a box but no parts: only the collision toggles.
        const bool hasParts = parts(e.doorId) > 0;
        auto [it, fresh] = doors_.try_emplace(e.id);
        auto& d          = it->second;
        if (fresh)
        {
            // At spawn: open doors run `into` (else `open`), closed ones `intc` if there is one (CODE 0x100acf90 /
            // 0x100ad060).
            d.fourcc    = e.doorId;
            d.animation = e.animation;
            solid(d.fourcc, !opens(e.animation) || e.animation == 45);
            // Model-less ones (no parts: Jeuno's street lamps `_l01`..) use `ints` / `inte` and `evst` / `eved`
            // instead (CODE 0x100acad0).
            const bool ok = opens(e.animation)
                                ? fx.playDoor(d.fourcc, dat::fxName(hasParts ? "into" : "ints"), e.id) || fx.playDoor(d.fourcc, dat::fxName(hasParts ? "open" : "evst"), e.id)
                                : fx.playDoor(d.fourcc, dat::fxName(hasParts ? "intc" : "inte"), e.id);
            if (trace)
            {
                spdlog::info("fx: door {} ({:x}) appears, animation {}: {}", dat::fxNameString(d.fourcc), e.id, e.animation, ok);
            }
            continue;
        }
        if (e.animation != d.animation)
        {
            d.animation = e.animation;
            if (opens(e.animation) || closes(e.animation))
            {
                solid(d.fourcc, closes(e.animation) || e.animation == 45);
                const bool ok = fx.playDoor(d.fourcc, dat::fxName(opens(e.animation) ? (hasParts ? "open" : "evst") : (hasParts ? "clos" : "eved")), e.id);
                if (trace)
                {
                    spdlog::info("fx: door {} ({:x}) animation {}: {}", dat::fxNameString(d.fourcc), e.id, e.animation, ok);
                }
            }
        }
    }
    for (auto& [id, d] : doors_)
    {
        for (std::size_t i = 0; i < d.parts.size(); ++i)
        {
            auto& p     = d.parts[i];
            bool  moved = false;
            for (auto [task, value] : { std::pair{ &p.rotTask, &p.rot }, std::pair{ &p.offTask, &p.offset } })
            {
                if (!*task)
                {
                    continue;
                }
                auto& t = **task;
                t.t     = std::min(t.t + dt, t.seconds);
                *value  = t.from + (t.to - t.from) * (t.t / t.seconds);
                moved   = true;
                if (t.t >= t.seconds)
                {
                    task->reset(); // the game stops a frame short of the target; this lands on it
                }
            }
            if (moved)
            {
                pose(d.fourcc, i, p.rot, p.offset);
            }
        }
    }
}

} // namespace xi::client
