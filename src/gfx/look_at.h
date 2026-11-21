// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/character_model.h"

#include <glm/glm.hpp>

#include <optional>
#include <vector>

// Head look-at (docs/event-format.md §6.6; CODE 0x100d5a40, 0x1002ab90): the head (skeleton point 3) and neck (point 7)
// turn toward a point on top of the motion, within the skeleton's limits, with a smoothed direction and a weight that
// fades in and out.
namespace xi::gfx
{

struct LookState
{
    glm::vec3 dir{ 0.0f }; // smoothed look direction from the head (render space)
    float     weight = 0.0f;
    bool      primed = false;
};

// `bones`: the render-space palette from characterPalette; `feet` / `forward`: where the actor stands and faces (render
// space, forward horizontal). `target` = the point to look at (nullopt: fade back). `headOnly`: sitting / mounted.
void applyLookAt(const CharacterModel& m, std::vector<glm::mat4>& bones, LookState& state, const glm::vec3& feet, const glm::vec3& forward,
                 const std::optional<glm::vec3>& target, float dt, bool headOnly = false);

} // namespace xi::gfx
