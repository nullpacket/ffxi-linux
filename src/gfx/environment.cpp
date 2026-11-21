// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/environment.h"

#include <algorithm>
#include <cmath>

namespace xi::gfx
{

// Zone environment for the renderer (docs/dat-format.md §9.5): one directional light from the clock (sun 06:00-18:00,
// else moon) or from the record (indoor zones), ambient, fog, and the sky gradient. Game space is Y-down; directions in
// the record/clock are the direction light travels, the shader wants the direction toward the light.
void applyEnvironment(const dat::WeatherRecord& r, double secondsOfDay, gfx::SceneParams& sp, gfx::SkyRenderer::Gradient& sky)
{
    auto col = [](const std::array<float, 3>& c) { return glm::vec3(c[0], c[1], c[2]) / 255.0f; };
    glm::vec3 travel;
    glm::vec3 colour;
    if (r.flags & 1)
    {
        travel = glm::vec3(r.dirZ[0], r.dirZ[1], r.dirZ[2]);
        travel = glm::length(travel) > 0.0f ? glm::normalize(travel) : glm::vec3(0, 1, 0);
        colour = col(r.sunZ);
    }
    else
    {
        const float     a    = static_cast<float>(secondsOfDay / 86400.0) * 6.283185f;
        const glm::vec3 moon(std::sin(a), std::cos(a), 0.0f);
        const glm::vec3 sun(std::sin(a + 3.141593f), std::cos(a + 3.141593f), 0.0f);
        const bool      day = sun.y >= 0.0f;
        travel              = day ? sun : moon;
        colour              = col(day ? r.sunZ : r.moonZ);
    }
    // Toward the light, in render space: -(gameToRender(travel)) = (-x, y, z).
    sp.light0Dir   = glm::normalize(glm::vec3(-travel.x, travel.y, travel.z) + glm::vec3(0, 1e-4f, 0));
    sp.light0Color = colour * r.multZ;
    sp.light1Color = glm::vec3(0.0f);
    sp.ambient     = col(r.ambientZ);
    sp.fogColor    = col(r.fogColor);
    sp.fogStart    = r.fogStartZ;
    sp.fogEnd      = std::max(r.fogEndZ, r.fogStartZ + 1.0f);
    for (std::size_t i = 0; i < 8; ++i)
    {
        sky.colors[i]    = col(r.sky[i]);
        sky.positions[i] = r.skyPos[i];
    }
    sp.skyColor = col(r.clear);
}

void applyActorEnvironment(const dat::WeatherRecord& r, double secondsOfDay, gfx::SceneParams& sp)
{
    // Reuse the zone evaluation with set A swapped into the set Z fields.
    dat::WeatherRecord a = r;
    a.sunZ      = r.sunA;
    a.moonZ     = r.moonA;
    a.ambientZ  = r.ambientA;
    a.fogStartZ = r.fogStartA;
    a.fogEndZ   = r.fogEndA;
    a.multZ     = r.multA;
    a.dirZ      = r.dirA;
    SkyRenderer::Gradient unused;
    applyEnvironment(a, secondsOfDay, sp, unused);
}

} // namespace xi::gfx
