// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/look_at.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace xi::gfx
{
namespace
{

constexpr float kWeightRate = 0.04f * 30.0f; // 0.04 per frame at the game's 30 fps
constexpr float kSmooth     = 1.0f / 32.0f;  // per 60 Hz tick

std::optional<glm::vec3> pointOf(const CharacterModel& m, const std::vector<glm::mat4>& bones, std::size_t pt)
{
    const auto& pts = m.skeleton.points;
    if (pt >= pts.size() || pts[pt].bone < 0 || static_cast<std::size_t>(pts[pt].bone) >= bones.size())
    {
        return std::nullopt;
    }
    const auto& o = pts[pt].offset;
    return glm::vec3(bones[static_cast<std::size_t>(pts[pt].bone)] * glm::vec4(o[0], o[1], o[2], 1.0f));
}

// CODE 0x1002b070: the direction projected on the plane at distance L.z in the aim's frame, pulled onto the ellipse
// (L.x across, L.y up) when outside it.
glm::vec3 clampDir(const glm::vec3& aim, const glm::vec3& t, const std::array<float, 3>& lim)
{
    glm::vec3 r = glm::cross(aim, glm::vec3(0, 1, 0));
    r           = glm::length(r) < 1e-4f ? glm::vec3(1, 0, 0) : glm::normalize(r);
    const glm::vec3 u  = glm::cross(r, aim);
    const float     tf = std::max(glm::dot(t, aim), 1e-3f);
    float           x  = lim[0] > 0 ? glm::dot(t, r) / tf * lim[2] : 0.0f;
    float           y  = lim[1] > 0 ? glm::dot(t, u) / tf * lim[2] : 0.0f;
    const float     e  = (lim[0] > 0 ? x * x / (lim[0] * lim[0]) : 0.0f) + (lim[1] > 0 ? y * y / (lim[1] * lim[1]) : 0.0f);
    if (e > 1.0f)
    {
        const float s = 1.0f / std::sqrt(e);
        x *= s;
        y *= s;
    }
    return glm::normalize(aim * lim[2] + r * x + u * y);
}

} // namespace

void applyLookAt(const CharacterModel& m, std::vector<glm::mat4>& bones, LookState& state, const glm::vec3& feet, const glm::vec3& forward,
                 const std::optional<glm::vec3>& target, float dt, bool headOnly)
{
    const auto head = pointOf(m, bones, 3);
    if (!head || m.skeleton.points.size() <= 7)
    {
        return;
    }
    // The resting aim: from the head toward point 4 (ahead of the face, root space), else straight ahead.
    glm::vec3 aim = forward;
    if (const auto p4 = pointOf(m, bones, 4); p4 && glm::length(*p4 - *head) > 1e-3f)
    {
        const glm::vec3 a = glm::normalize(*p4 - *head);
        aim               = glm::dot(a, forward) > 0.5f ? a : forward;
    }
    // Weight: fades out with no target, a target behind (local x < -0.5), turned away, or nearer than 0.3 across.
    bool have = target.has_value();
    if (have)
    {
        const glm::vec3 rel = *target - feet;
        const glm::vec3 to  = *target - *head;
        have = glm::dot(rel, forward) >= -0.5f && glm::length(glm::vec2(rel.x, rel.z)) >= 0.3f && glm::length(to) > 1e-3f &&
               glm::dot(glm::normalize(to), aim) >= -0.1f;
    }
    if (have)
    {
        state.weight = std::min(1.0f, state.weight + kWeightRate * dt);
        if (!state.primed)
        {
            state.dir    = aim;
            state.primed = true;
        }
        const float k = 1.0f - std::pow(1.0f - kSmooth, 60.0f * dt);
        state.dir     = glm::normalize(glm::mix(state.dir, glm::normalize(*target - *head), k));
    }
    else
    {
        state.weight -= kWeightRate * dt;
        if (state.weight <= 0.0f)
        {
            state = {};
            return;
        }
    }
    if (!state.primed)
    {
        return;
    }
    // Head first, then the neck takes what is left; each turns its bone and everything below it about the joint.
    const auto& pts  = m.skeleton.points;
    glm::vec3   t    = state.dir;
    const int   bonesFor[2] = { pts[3].bone, pts[7].bone };
    glm::quat   q[2] = { glm::quat(1, 0, 0, 0), glm::quat(1, 0, 0, 0) };
    for (int j = 0; j < (headOnly ? 1 : 2); ++j)
    {
        const auto& lim = m.skeleton.lookLimits[static_cast<std::size_t>(j)];
        if ((lim[0] <= 0 && lim[1] <= 0) || lim[2] <= 0 || bonesFor[j] < 0)
        {
            continue;
        }
        const glm::vec3 c     = clampDir(aim, t, lim);
        const glm::vec3 axis  = glm::cross(aim, c);
        const float     angle = std::acos(std::clamp(glm::dot(aim, c), -1.0f, 1.0f)) * state.weight;
        if (glm::length(axis) < 1e-6f || angle < 1e-5f)
        {
            continue;
        }
        q[j] = glm::angleAxis(angle, glm::normalize(axis));
        t    = glm::inverse(q[j]) * t;
    }
    const auto& sb = m.skeleton.bones;
    for (int j = 0; j < 2; ++j)
    {
        const int b = bonesFor[j];
        if (b < 0 || static_cast<std::size_t>(b) >= bones.size() || q[j] == glm::quat(1, 0, 0, 0))
        {
            continue;
        }
        const glm::vec3 pivot(bones[static_cast<std::size_t>(b)][3]);
        const glm::mat4 r = glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(q[j]) * glm::translate(glm::mat4(1.0f), -pivot);
        std::vector<char> below(bones.size(), 0);
        below[static_cast<std::size_t>(b)] = 1;
        for (std::size_t k = static_cast<std::size_t>(b) + 1; k < bones.size() && k < sb.size(); ++k)
        {
            below[k] = sb[k].parent < k ? below[sb[k].parent] : 0;
        }
        for (std::size_t k = 0; k < bones.size(); ++k)
        {
            if (below[k])
            {
                bones[k] = r * bones[k];
            }
        }
    }
}

} // namespace xi::gfx
