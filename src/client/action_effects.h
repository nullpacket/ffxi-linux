// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "client/effects.h"
#include "client/session.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Starts the effect schedulers for what happens in the world (docs/effect-format.md §C): each new S2C 0x028 action
// (melee `atk0`, ranged, weapon skills, magic, items, abilities, TP moves, cast starts and interrupts), characters
// appearing (`pop0` / `init`) and monsters dying (`dead`).
namespace xi::client
{

class ActionEffects
{
public:
    // A new zone: forget who was seen and what was being cast.
    void reset();
    // `id` died this frame; its `dead` plays at the next update (once it is posed, so its files are known).
    void died(uint32_t id)
    {
        dying_.push_back(id);
    }
    // `settled`: 3 s or more after zone-in (appearing characters fade in rather than being there already).
    // `posed(id)`: the actor's model files are known. Results that no effect will show go into `shownResults`
    // (ChatLine::hold). `filesText` is for XI_FX_TRACE.
    void update(EffectSystem& fx, const WorldSnapshot& world, bool settled, const std::function<bool(uint32_t)>& posed,
                std::unordered_set<uint64_t>& shownResults, bool trace, const std::function<std::string(uint32_t)>& filesText);
    // Results logged at once because no effect will show them (ChatLine::hold), since the last call.
    std::vector<uint64_t> takeReleased()
    {
        return std::exchange(released_, {});
    }
    // Ages the motion windows.
    void tick(float dt);
    // Scheduler motions (ops 0x05 / 0x06) drive an actor only for a while after its item use or job ability; melee,
    // weapon skills, casting and TP moves animate from their own state.
    [[nodiscard]] bool motionsAllowed(uint32_t actor) const
    {
        return motionWindow_.contains(actor);
    }

private:
    uint32_t                                  actionSeen_ = 0;
    uint32_t                                  posSeen_    = 0;
    std::unordered_map<uint32_t, dat::FxName> casting_;      // actor -> its running `ca..`
    std::unordered_map<uint32_t, float>       motionWindow_; // actor -> seconds its scheduler motions apply
    std::unordered_set<uint32_t>              spawnSeen_;    // characters seen this zone
    std::vector<uint32_t>                     dying_;
    std::vector<uint64_t>                     released_;
};

} // namespace xi::client
