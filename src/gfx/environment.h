// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/weather.h"
#include "gfx/mesh_renderer.h"
#include "gfx/sky_renderer.h"

namespace xi::gfx
{

// Zone environment for the renderer (docs/dat-format.md §9.5): one directional light from the clock (sun 06:00-18:00,
// else moon) or from the record (indoor zones), ambient, fog, and the sky gradient. Game space is Y-down; directions in
// the record/clock are the direction light travels, the shader wants the direction toward the light.
void applyEnvironment(const dat::WeatherRecord& r, double secondsOfDay, gfx::SceneParams& sp, gfx::SkyRenderer::Gradient& sky);

// Same evaluation for characters/actors (record light set A: +0x0C..+0x24, fog colour shared).
void applyActorEnvironment(const dat::WeatherRecord& r, double secondsOfDay, gfx::SceneParams& sp);

} // namespace xi::gfx
