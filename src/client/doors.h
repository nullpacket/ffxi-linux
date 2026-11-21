// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "client/effects.h"
#include "client/session.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

// Zone doors (docs/dat-format.md §5 doors, effect-format §S.3 ops 0x0C / 0x0D): a door entity (0x00E look kind 2)
// names zone placements by its fourcc; its animation byte runs the zone block's `open` / `clos` schedulers, whose part
// moves turn the placements about their hinges.
namespace xi::client
{

class Doors
{
public:
    using PartsOf = std::function<std::size_t(uint32_t door)>;
    using Pose    = std::function<void(uint32_t door, std::size_t part, const glm::vec3& rot, const glm::vec3& offset)>;
    using Solid   = std::function<void(uint32_t door, bool solid)>;
    // Lifts (look kind 3): the platform heights per state, and moving the platform (game y).
    struct LiftHooks
    {
        std::function<std::vector<float>(uint32_t lift)> heights;
        std::function<void(uint32_t lift, float y)>      setY;
    };

    // A new zone.
    void reset()
    {
        doors_.clear();
        lifts_.clear();
    }
    // Per frame, before EffectSystem::update: the part moves its schedulers asked for, new animations (8 / 26 / 29 /
    // 45 open, 9 / 27 / 30 / 46 close; CODE 0x1008c7c6), then the moving parts advance and `pose` gets each one.
    // `solid`: the door's collision box, off from the start of an opening (not 45) and on again at a closing.
    void update(const WorldSnapshot& world, EffectSystem& fx, float dt, const PartsOf& parts, const Pose& pose, const Solid& solid,
                const LiftHooks& lifts = {});

private:
    struct Task
    {
        glm::vec3 from{ 0.0f }, to{ 0.0f };
        float     t = 0.0f, seconds = 0.0f;
    };
    struct Part
    {
        glm::vec3           rot{ 0.0f }, offset{ 0.0f };
        std::optional<Task> rotTask, offTask;
    };
    struct Door
    {
        uint32_t             fourcc = 0;
        uint8_t              animation = 0;
        std::array<Part, 4>  parts{};
    };
    std::unordered_map<uint32_t, Door> doors_; // by entity id
    struct Lift
    {
        uint32_t fourcc = 0;
        uint8_t  animation = 0;
        float    y = 0, fromY = 0, toY = 0;
        double   start = 0, end = 0; // seconds since the Vana'diel epoch
    };
    std::unordered_map<uint32_t, Lift> lifts_; // by entity id
    void updateLifts(const WorldSnapshot& world, const LiftHooks& hooks);
};

} // namespace xi::client
