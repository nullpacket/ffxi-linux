// SPDX-License-Identifier: GPL-3.0-or-later
#include "actor_poses.h"

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>

namespace xi::app
{
namespace
{

std::optional<glm::vec3> pointIn(const std::unordered_map<uint32_t, PosedActor>& poses, uint32_t id, int pt)
{
    const auto it = poses.find(id);
    if (it == poses.end() || !it->second.model || pt < 0)
    {
        return std::nullopt;
    }
    const auto& pts = it->second.model->skeleton.points;
    const auto  i   = static_cast<std::size_t>(pt);
    if (i >= pts.size() || pts[i].bone < 0 || static_cast<std::size_t>(pts[i].bone) >= it->second.bones.size())
    {
        return std::nullopt;
    }
    const auto& o = pts[i].offset;
    return glm::vec3(it->second.bones[static_cast<std::size_t>(pts[i].bone)] * glm::vec4(o[0], o[1], o[2], 1.0f));
}

} // namespace

void ActorPoses::newFrame()
{
    std::swap(last_, now_);
    now_.clear();
}

void ActorPoses::keep(uint32_t id, const client::Look& look, const gfx::CharacterModel* model, const std::vector<glm::mat4>& bones)
{
    auto& p = now_[id];
    p.model = model;
    p.bones = bones;
    if (const auto* ce = chars_.find(look))
    {
        p.files = ce->effectFiles; // first: its `init` / `cate` win over the race files'
        p.files.insert(p.files.end(), ce->battleMotionFiles.begin(), ce->battleMotionFiles.end());
        p.files.insert(p.files.end(), ce->motionFiles.begin(), ce->motionFiles.end());
        p.files.insert(p.files.end(), ce->weaponFiles.begin(), ce->weaponFiles.end());
        // Face and armour last (INFERRED order): the voices `vatk` / `vdam` / `vswy` / `vded` and the body's `sdam`
        // that `atk0` / `damg` / `sway` / `dead` ask for with op 0x57.
        p.files.insert(p.files.end(), ce->gearFiles.begin(), ce->gearFiles.end());
        p.rangedClass = ce->rangedClass;
        p.sheathStyle = ce->sheathStyle;
    }
}

const PosedActor* ActorPoses::find(uint32_t id) const
{
    const auto it = now_.find(id);
    return it == now_.end() ? nullptr : &it->second;
}

std::optional<glm::vec3> ActorPoses::point(uint32_t id, int pt) const
{
    return pointIn(now_, id, pt);
}

std::optional<glm::vec3> ActorPoses::lastPoint(uint32_t id, int pt) const
{
    return pointIn(last_, id, pt);
}

void ActorPoses::look(uint32_t id, const gfx::CharacterModel& m, std::vector<glm::mat4>& bones, const glm::vec3& at, uint8_t dir,
                      const std::optional<glm::vec3>& target, bool headOnly, float dt)
{
    const float     a       = -static_cast<float>(dir) * 3.14159265f / 128.0f;
    const glm::vec3 forward = glm::vec3(std::cos(a), 0.0f, -std::sin(a));
    auto&           state   = looks_[id];
    gfx::applyLookAt(m, bones, state, at, forward, target, dt, headOnly);
    static const bool trace = std::getenv("XI_LOOK_TRACE") != nullptr;
    static uint64_t   calls = 0;
    if (trace && target && ++calls % 60 == 0)
    {
        const glm::vec3 d = *target - at;
        spdlog::info("look: {:x} weight {:.2f} target ahead {:.2f} left {:.2f} up {:.2f}", id, state.weight, glm::dot(d, forward),
                     glm::dot(d, glm::vec3(-std::sin(a), 0, -std::cos(a))), d.y);
    }
}

void scaleAbout(std::vector<glm::mat4>& bones, const glm::vec3& at, float k)
{
    const glm::mat4 sc = glm::translate(glm::mat4(1.0f), at) * glm::scale(glm::mat4(1.0f), glm::vec3(k)) * glm::translate(glm::mat4(1.0f), -at);
    for (auto& b : bones)
    {
        b = sc * b;
    }
}

} // namespace xi::app
