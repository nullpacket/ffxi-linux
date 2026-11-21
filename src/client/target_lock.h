// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/glm.hpp>

// Target lock (issue #116; ui-format-notes §9.2, dat-format §7.8b "Camera follow"). The game keeps one lock (target
// object +0x5c bits 0..1): while it is set the target commands leave the target alone, the camera looks at the target
// and the character faces it, stepping back or sideways relative to it.
namespace xi::client
{

// Num * (key action 0x30, CODE 0x10158110) and /lockon (command 0x5c, 0x1007a630) toggle it; turning it on needs a
// living target (the setter 0x10157590 refuses when 0x10157620 finds the target gone or dead).
[[nodiscard]] constexpr bool targetLockToggle(bool on, bool targetAlive)
{
    return !on && targetAlive;
}

// A target the lock can hold: there, not gone (statuses 2 / 3) and, for monsters and players, not knocked out
// (0x10157620). E is client::Entity.
template <class E>
[[nodiscard]] constexpr bool lockable(const E* e)
{
    return e && e->status != 2 && e->status != 3 && !((e->isMob || e->isPc) && e->hpp == 0);
}

// Once a frame. Engaging a living monster locks on it (INFERRED: the user's retail observation; no setter on engage was
// found), disengaging clears it (0x1008b722: our status leaves engaged), and so does the target dying or going
// (0x10157620, per frame at 0x10157969).
[[nodiscard]] constexpr bool targetLockStep(bool on, bool engaged, bool wasEngaged, bool targetAlive)
{
    if (engaged && !wasEngaged && targetAlive)
    {
        on = true;
    }
    if (!engaged && wasEngaged)
    {
        on = false;
    }
    return on && targetAlive;
}

// Moving while locked: the character keeps facing the target and the step is named relative to it (the motion set
// picks run / back / left / right). face and move are render-space x/z directions (need not be unit length).
// 0 ahead, 1 back, 2 left, 3 right.
[[nodiscard]] inline int lockedMoveDir(glm::vec2 face, glm::vec2 move)
{
    if (glm::length(face) < 1e-6f || glm::length(move) < 1e-6f)
    {
        return 0;
    }
    face               = glm::normalize(face);
    move               = glm::normalize(move);
    const float ahead  = glm::dot(face, move);
    const float across = face.x * move.y - face.y * move.x;
    return ahead > 0.5f ? 0 : ahead < -0.5f ? 1 : across > 0.0f ? 3 : 2;
}

} // namespace xi::client
