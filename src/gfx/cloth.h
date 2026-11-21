// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/character.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace xi::gfx
{

// Os2 class-1 soft bodies (hair, capes, skirts; dat-format §10.3) of one character model. Each particle is drawn by
// one extra palette matrix after the skeleton's (base + its asset-wide index): translation = its position, y axis =
// its normal (the cloth vertices carry p0 = 0, n0 = +y). base = 0: none, or uploaded rigid (too many bones). CPU-only.
struct ClothModel
{
    std::vector<dat::ClothMesh>        meshes;
    std::vector<dat::Skeleton::Sphere> spheres; // the skeleton's collision spheres (dat::Skeleton::spheres)
    uint32_t                           base = 0, particles = 0;
};

// Per-actor soft-body state (render space). Steps at the game's 30 ticks a second (the game steps once per drawn
// frame at its 30 fps, CODE 0x10022400 -> 0x100231c0) and draws between the last two ticks.
struct ClothSim
{
    std::vector<glm::vec3> p, p0, prev; // position, previous position (Verlet), position at the tick before
    std::vector<glm::vec3> n;           // normals as last drawn (the wind pushes along them)
    std::vector<std::vector<int>>       tie;     // per mesh, per spring: the sphere it collides with (-1 none), set at reset
    std::vector<std::vector<glm::vec3>> centres; // per mesh, per sphere slot: the centre at the last tick (the sweep's start)
    glm::vec3              root{ 0.0f };
    float                  acc  = 0.0f;
    bool                   init = false;
};

// The scene's wind for soft bodies (CODE 0x10188960, scene object 0x10629fe4): gusts of rand() % 60 ticks, 1.2 times
// as long while blowing, alternate with lulls; while blowing the strength is (4 + rand() % 10) x 0.0025 every frame, in
// a lull it falls 0.01 a frame to 0. The direction is fixed, game (-1, 0, 0) (+0x124, set only at init 0x10186f80).
struct ClothWind
{
    float    strength = 0.0f, timer = 0.0f, acc = 0.0f;
    bool     gust     = false;
    uint32_t rng      = 0x2545f491u;
};
// Advances the wind at the game's 30 frames a second. Call once per drawn frame for every actor.
void stepClothWind(ClothWind& w, float dt);

// Skins the particles from the first `base` palette matrices and appends them (the rest pose, rigid).
void restCloth(const ClothModel& m, std::vector<glm::mat4>& palette);
// One frame of the soft body: `palette` holds the posed skeleton (render space) of an actor whose feet are at
// `rootRender`; appends / overwrites the particle matrices. dt in seconds. `wind`: the scene wind's strength, 0 for an
// actor on a roofed floor (CODE 0x100a5303: actor +0x17c = the floor placement's flag bit 2 skips the push).
void simulateCloth(const ClothModel& m, ClothSim& sim, std::vector<glm::mat4>& palette, const glm::vec3& rootRender, float dt,
                   float wind = 0.0f);

} // namespace xi::gfx
