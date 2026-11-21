// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/sound_system.h"
#include "client/effects.h"
#include "dat/character.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

// Footsteps (docs/sound-format.md; CODE 0x100d4af0): for an actor within 30 of the listener, a foot attach point
// (9 / 8, and 0xb / 0xa for quadrupeds; effect-format §C.6) coming down while moving plays the zone's `fses` / `fser`
// Sep for the floor material and footwear at the actor.
namespace xi::audio
{

class Footsteps
{
public:
    // The footwear sound bytes (dat::CharacterAsset::shoe / shoeVariant / footLayout).
    struct Feet
    {
        int8_t  shoe = -1, variant = -1;
        uint8_t layout = 0; // 2 = quadruped
    };
    struct Step
    {
        std::string            name; // the Sep name, e.g. "0223"
        uint8_t                material = 0;
        SoundSystem::Played    played;
    };

    // A new zone.
    void reset()
    {
        state_.clear();
    }
    // One actor's frame. `bones` and `at` (the actor's feet) in render space (Y up); `material()` is the floor
    // material under it, asked only when a step falls. `walking`: 80% volume (vol 102 of 127). The step, when one
    // played.
    std::optional<Step> update(uint32_t id, const Feet& feet, std::span<const dat::Skeleton::Point> points, std::span<const glm::mat4> bones,
                               const glm::vec3& at, bool moving, bool walking, float dt, const std::function<uint8_t()>& material, bool rain,
                               client::EffectSystem& fx, SoundSystem& sound, const SoundSystem::Listener& ear);

private:
    struct State
    {
        std::array<float, 4> prev{}, low{ 1e9f, 1e9f, 1e9f, 1e9f };
        std::array<bool, 4>  down{};
        float                since = 0.0f; // seconds since the last step
    };
    std::unordered_map<uint32_t, State> state_;
};

} // namespace xi::audio
