// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "characters.h"
#include "gfx/look_at.h"

#include <glm/glm.hpp>

#include <optional>
#include <unordered_map>
#include <vector>

// Each drawn actor's pose this frame (render-space bones) and the DAT files its resources live in: the effects attach to
// its skeleton points and run its motions; the previous frame's poses give look-at targets.
namespace xi::app
{

struct PosedActor
{
    const gfx::CharacterModel* model = nullptr;
    std::vector<glm::mat4>     bones;
    std::vector<uint32_t>      files; // the weapon-class battle files, then the base motion files, then the weapons'
    int                        rangedClass = -1;
    int                        sheathStyle = 0; // the main weapon's (effect op 0x84)
};

class ActorPoses
{
public:
    explicit ActorPoses(Characters& chars)
        : chars_(chars)
    {
    }

    // Start of a frame: this frame's poses become last frame's.
    void newFrame();
    void keep(uint32_t id, const client::Look& look, const gfx::CharacterModel* model, const std::vector<glm::mat4>& bones);
    [[nodiscard]] const PosedActor* find(uint32_t id) const;

    // A skeleton point (effect-format §C.6: the posed bone's matrix applied to the point's offset), render space, as
    // posed this frame / last frame.
    [[nodiscard]] std::optional<glm::vec3> point(uint32_t id, int point) const;
    [[nodiscard]] std::optional<glm::vec3> lastPoint(uint32_t id, int point) const;

    // Head look-at (gfx::applyLookAt) with the actor's own smoothing state. `XI_LOOK_TRACE=1` logs it.
    void look(uint32_t id, const gfx::CharacterModel& m, std::vector<glm::mat4>& bones, const glm::vec3& at, uint8_t dir,
              const std::optional<glm::vec3>& target, bool headOnly, float dt);

private:
    Characters&                                  chars_;
    std::unordered_map<uint32_t, PosedActor>     now_, last_;
    std::unordered_map<uint32_t, gfx::LookState> looks_;
};

// Scales a palette by `k` about `at` (render space).
void scaleAbout(std::vector<glm::mat4>& bones, const glm::vec3& at, float k);

} // namespace xi::app
