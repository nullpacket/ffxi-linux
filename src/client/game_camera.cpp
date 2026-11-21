// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/game_camera.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace xi::client
{

namespace
{
constexpr glm::vec3 kUp(0.0f, 1.0f, 0.0f);

glm::vec3 flat(glm::vec3 v)
{
    v.y = 0.0f;
    return v;
}

// A rate given per 1/60 s tick, over `ticks` ticks.
float perTicks(float rate, float ticks)
{
    return 1.0f - std::pow(1.0f - rate, ticks);
}

glm::vec3 viewDir(float yaw, float pitch)
{
    return { std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch) };
}
} // namespace

void GameCamera::reset(const Subject& s)
{
    // CODE 0x100215b0: the offset (-4, -2, 0) in the actor's frame (x forward, y down) is 4 behind and 2 up; the look-at
    // starts 0.5 above the feet and eases to its height from there.
    const glm::vec3 f = glm::length(flat(s.facing)) > 1e-4f ? glm::normalize(flat(s.facing)) : glm::vec3(0.0f, 0.0f, -1.0f);
    eye_        = s.feet - f * 4.0f + kUp * 2.0f;
    at_         = s.feet + kUp * 0.5f;
    placed_     = true;
    holdRise_   = std::abs(eye_.y - at_.y);
    inputHold_  = 0.0f;
    settle_     = 0.0f;
    swingHold_  = 0.0f;
    recentre_   = 0.0f;
    backtrack_  = false;
    trailCount_ = 0;
    fpPitch_ = fpYaw_ = 0.0f;
}

void GameCamera::resetAt(const Subject& s, float yaw, float pitch, float dist)
{
    reset(s);
    at_       = s.feet + kUp * (kTargetHeight * s.height);
    eye_      = at_ - viewDir(yaw, pitch) * dist;
    holdRise_ = std::abs(eye_.y - at_.y);
}

float GameCamera::yaw() const
{
    const glm::vec3 d = at_ - eye_;
    return std::atan2(d.x, -d.z);
}

float GameCamera::pitch() const
{
    const glm::vec3 d = at_ - eye_;
    const float     l = glm::length(d);
    return l > 1e-5f ? std::asin(std::clamp(d.y / l, -1.0f, 1.0f)) : 0.0f;
}

float GameCamera::distance() const
{
    return glm::distance(at_, eye_);
}

void GameCamera::rotateEye(float radians)
{
    // CODE 0x1001eae0: the eye turns about the look-at in x / z, keeping its level distance and height.
    const glm::vec3 off = eye_ - at_;
    const float     r   = glm::length(flat(off));
    if (r < 1e-4f)
    {
        return;
    }
    const float a = std::atan2(off.x, -off.z) + radians;
    eye_.x        = at_.x + std::sin(a) * r;
    eye_.z        = at_.z - std::cos(a) * r;
}

void GameCamera::record(const glm::vec3& at)
{
    // CODE 0x1001e250: the look-at's positions 0.1 or more apart, newest first.
    if (trailCount_ > 0 && glm::distance(at, trail_[0]) < 0.1f)
    {
        return;
    }
    for (std::size_t i = trail_.size() - 1; i > 0; --i)
    {
        trail_[i] = trail_[i - 1];
    }
    trail_[0]   = at;
    trailCount_ = std::min(trailCount_ + 1, 127);
}

void GameCamera::update(const Subject& s, const Input& in, const std::optional<Lock>& lock, bool firstPerson, float ticks,
                        const Ray& ray, const Clearance& clearance)
{
    if (!placed_)
    {
        reset(s);
    }
    ticks = std::clamp(ticks, 0.0f, 30.0f);
    if (firstPerson)
    {
        firstPersonView(s, in, ticks);
        wasFirst_ = true;
        return;
    }
    if (wasFirst_)
    {
        wasFirst_ = false;
        fpPitch_ = fpYaw_ = 0.0f;
    }
    const glm::vec3 eyeStart = eye_;
    inputHold_               = std::max(0.0f, inputHold_ - ticks);
    if (lock)
    {
        lockOn(s, in, *lock, ticks, ray);
        wasLocked_ = true;
    }
    else
    {
        if (wasLocked_)
        {
            wasLocked_ = false;
            settle_    = 20.0f; // CODE 0x1001f8.. : 20 ticks before the follow rules take over again
        }
        thirdPerson(s, in, ticks, ray);
    }

    // Home (action 0x51, CODE 0x1001ff2a..): for 8 frames the eye closes 1/8 a tick on the point 3 behind the
    // character at its head height, while the view is more than 0.05 rad off the facing. The game counts frames
    // (30 a second); here 16 ticks.
    if (recentre_ <= 0.0f)
    {
        if (in.recentre)
        {
            recentre_ = 16.0f;
        }
    }
    else
    {
        const glm::vec3 f   = glm::length(flat(s.facing)) > 1e-4f ? glm::normalize(flat(s.facing)) : glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 to  = s.feet - f * 3.0f + kUp * s.height;
        const float     off = std::remainder(yaw() - std::atan2(f.x, -f.z), 6.2831853f);
        if (std::abs(off) >= 0.05f)
        {
            eye_ += (to - eye_) * perTicks(0.125f, ticks);
            recentre_ -= ticks;
        }
        else
        {
            recentre_ = 0.0f;
        }
    }

    // CODE 0x10020760..: the eye keeps 0.17 off the ground, a ceiling and side walls (when it moved).
    if (clearance && glm::distance(eye_, eyeStart) > 0.01f)
    {
        const glm::vec3 fwd = at_ - eye_;
        eye_                = clearance(eye_, glm::length(fwd) > 1e-4f ? glm::normalize(fwd) : glm::vec3(0.0f, 0.0f, -1.0f));
    }
    // CODE 0x10020997: once settled, a last look from the look-at to the eye: 0.2 in front of a hit (after the
    // clearance and Home have moved it).
    if (ray && settle_ <= 0.0f)
    {
        const glm::vec3 d = eye_ - at_;
        const float     l = glm::length(d);
        if (l > 1e-4f)
        {
            if (const auto hit = ray(at_, eye_); hit && *hit < l)
            {
                eye_ = at_ + d / l * std::max(*hit - 0.2f, 0.1f);
            }
        }
    }
    // Ours, not the game's: in a cramped spot (a corner under an overhang) the push out, the view line and the
    // clearance can chase each other for ever while the character stands. Once the eye's distance has turned back
    // and forth three times within half a second with nothing moving, it stays where it is until the character, the
    // look-at or the camera keys move again.
    const bool still = !lock && !s.moving && s.platformDy == 0.0f && in.yaw == 0.0f && in.vertical == 0.0f && in.mouseYaw == 0.0f &&
                       in.mouseUp == 0.0f && !in.recentre && glm::distance(at_, stillAt_) < 0.02f;
    stillAt_          = at_;
    const float dd    = glm::distance(eye_, at_) - lastDist_;
    lastDist_         = glm::distance(eye_, at_);
    if (!still)
    {
        frozen_    = false;
        turns_     = 0.0f;
        lastDelta_ = 0.0f;
    }
    else if (frozen_)
    {
        eye_      = frozenEye_;
        lastDist_ = glm::distance(eye_, at_);
    }
    else
    {
        turns_ = std::max(0.0f, turns_ - ticks / 30.0f); // three turns within half a second
        if (std::abs(dd) > 0.02f)
        {
            if (lastDelta_ * dd < 0.0f)
            {
                turns_ += 1.0f;
            }
            lastDelta_ = dd;
        }
        if (turns_ >= 3.0f)
        {
            frozen_    = true;
            frozenEye_ = eye_;
        }
    }
    holdRise_ = std::abs(eye_.y - at_.y);
}

void GameCamera::thirdPerson(const Subject& s, const Input& in, float ticks, const Ray& ray)
{
    // Camera input (CODE 0x1001ef3c..0x1001f0f0): the keys turn the eye about the look-at 0.0279 rad a tick and move
    // it up or down 0.1067 a tick; not while the follow rules wait after first person or a lock.
    const float dist = std::max(distance(), 0.01f);
    float       turn = 0.0f, rise = 0.0f;
    if (settle_ <= 0.0f)
    {
        turn = in.yaw * tuning.yawPerTick * ticks + in.mouseYaw;
        rise = in.vertical * tuning.risePerTick * ticks + in.mouseUp * dist;
    }
    if (turn != 0.0f)
    {
        rotateEye(turn);
        inputHold_ = 10.0f;
    }
    if (rise != 0.0f)
    {
        eye_.y += rise;
        inputHold_ = 10.0f;
    }
    // On a lift or a ship the eye rides up and down with the character (actor +0x5e2, CODE 0x1001f1ab).
    if (s.platformDy != 0.0f)
    {
        eye_.y += s.platformDy;
        inputHold_ = 10.0f;
    }

    // The look-at: 0.6 of the model's height above the feet (CODE 0x1001f236..0x1001f2f0), closing a quarter of
    // the gap a tick (0x1001f2f0..0x1001f320).
    const glm::vec3 want = s.feet + kUp * (kTargetHeight * s.height);
    at_ += (want - at_) * perTicks(kTargetEase, ticks);

    if (backtrack_)
    {
        // DAT_10456d70 (CODE 0x1001fe9c..): the eye walks 5 % a tick to where the look-at was four records back,
        // then normal following resumes; camera input or a short trail ends it.
        if (inputHold_ > 0.0f)
        {
            trailCount_ = 0;
        }
        if (trailCount_ < 4)
        {
            trailCount_ = 0;
            backtrack_  = false;
        }
        else
        {
            const glm::vec3 p = trail_[3];
            if (glm::distance(p, eye_) <= 0.05f)
            {
                trailCount_ = 0;
                backtrack_  = false;
            }
            else
            {
                eye_ += (p - eye_) * perTicks(0.05f, ticks);
            }
            if (glm::distance(p, at_) > 10.0f)
            {
                trailCount_ = 0;
                backtrack_  = false;
            }
        }
        return;
    }

    // Locked behind's room test (CODE 0x1001f8a6..): with the eye 1..3 (level) from the look-at, the push out to 3
    // needs a clear line 3.1 from the look-at toward it. Without the setting the push always applies.
    bool room = true;
    if (in.lockedBehind && ray)
    {
        const glm::vec3 h = flat(eye_ - at_);
        const float     l = glm::length(h);
        if (l > 1.0f && l < 3.0f && ray(at_, at_ + h / l * 3.1f))
        {
            room = false;
        }
    }
    const bool input = std::abs(in.yaw) >= 0.01f || std::abs(in.vertical) >= 0.01f || in.mouseYaw != 0.0f || in.mouseUp != 0.0f;
    const int  steps = std::max(1, static_cast<int>(std::ceil(ticks)));
    const float h    = ticks / static_cast<float>(steps);
    glm::vec3   pushed(0.0f);
    for (int i = 0; i < steps; ++i)
    {
        settle_ -= h;
        if (settle_ > 0.0f)
        {
            continue;
        }
        settle_ = 0.0f;
        // Locked behind (cfg 0x91, CODE 0x1001f9d0..0x1001fc2a): the eye swings 2.5 % a tick toward the point at
        // its distance behind the character, unless it already looks along the facing (dot >= 0.99) or at the
        // character's front (<= -0.5). After camera input the swing waits until the character has moved 0.5 from
        // where it was, and 4 ticks more.
        if (in.lockedBehind)
        {
            if (input)
            {
                swingFrom_ = s.feet;
                swingHold_ = 4.0f;
            }
            bool hold = false;
            if (swingHold_ > 0.0f)
            {
                if (glm::length(flat(s.feet - swingFrom_)) > 0.5f)
                {
                    swingHold_ = std::max(0.0f, swingHold_ - h);
                }
                hold = swingHold_ > 0.0f;
            }
            const glm::vec3 f   = glm::length(flat(s.facing)) > 1e-4f ? glm::normalize(flat(s.facing)) : glm::vec3(0.0f, 0.0f, -1.0f);
            const glm::vec3 off = flat(eye_ - at_);
            if (!hold && glm::length(off) > 1e-4f)
            {
                const float dot = glm::dot(-f, glm::normalize(off));
                if (dot > -0.5f && dot < 0.99f)
                {
                    const glm::vec3 behind = at_ - f * distance();
                    eye_ += flat(behind - eye_) * perTicks(kSwing, h);
                }
            }
        }
        // The follow distance (CODE 0x1001fc2f..0x1001fda3).
        const glm::vec3 v = at_ - eye_;
        const float     d = glm::length(v);
        if (d > kFar)
        {
            eye_ += v / d * ((d - kFar) * 0.5f * h);
        }
        else if (d >= tuning.follow)
        {
            eye_ += v * ((d - tuning.follow) * kFollowRate * h);
        }
        if (room)
        {
            const glm::vec3 before = eye_;
            if (d < tuning.nearDist)
            {
                eye_ -= v * ((tuning.nearDist - d) * kNearRate * h);
                inputHold_ = 10.0f;
            }
            const glm::vec3 lv = flat(eye_ - at_);
            const float     ll = glm::length(lv);
            if (ll < tuning.nearDist)
            {
                // Straight over the look-at the level direction is noise; then behind the character (ours, INFERRED).
                const glm::vec3 f   = glm::length(flat(s.facing)) > 1e-4f ? glm::normalize(flat(s.facing)) : glm::vec3(0.0f, 0.0f, -1.0f);
                const glm::vec3 dir = ll > 0.1f ? lv / ll : -f;
                eye_ += dir * ((tuning.nearDist - ll) * kNearRate * h);
                inputHold_ = 10.0f;
            }
            pushed += eye_ - before;
        }
    }
    if (settle_ > 0.0f)
    {
        return;
    }
    // Ours, not the game's: the push out toward 3 is kept only when the view line to where it put the eye is clear
    // (with the 0.5 gap). The game pushes regardless and lets the view-line pull below take the eye back in, which
    // against a slope or a pillar alternates frame by frame (xi-dat camcheck "bouncing runs"; INFERRED that the
    // game's 30 frames a second hide it less well than not at all).
    if (ray && glm::length(pushed) > 1e-5f)
    {
        const glm::vec3 v = eye_ - at_;
        const float     l = glm::length(v);
        if (const auto hit = l > 1e-4f ? ray(at_, eye_ + v / l * kWallGap) : std::nullopt; hit && *hit < l + kWallGap)
        {
            eye_ -= pushed;
        }
    }
    // CODE 0x1001fdc0..: while the camera is left alone the eye keeps the height over the look-at it ended the last
    // frame with (the follow pull would otherwise lower it).
    if (inputHold_ <= 0.0f)
    {
        const float r = eye_.y - at_.y;
        if (r > 0.0f && r < holdRise_)
        {
            eye_.y = at_.y + holdRise_;
        }
    }
    // The view line (CODE 0x1001fe0a..): the eye stops 0.5 in front of the first surface between the look-at and it;
    // left alone with a trail of four, it walks back along the trail instead (above).
    if (ray)
    {
        const glm::vec3 v = eye_ - at_;
        const float     l = glm::length(v);
        if (l > 1e-4f)
        {
            if (const auto hit = ray(at_, eye_); hit && *hit < l)
            {
                if (inputHold_ <= 0.0f && trailCount_ > 3)
                {
                    backtrack_ = true;
                }
                else
                {
                    eye_ = at_ + v / l * std::max(*hit - kWallGap, 0.1f);
                }
                return;
            }
        }
    }
    record(at_);
}

void GameCamera::lockOn(const Subject& s, const Input& in, const Lock& l, float ticks, const Ray& ray)
{
    // /lockon (actor +0xf9 clear; CODE 0x1001f37b.. and 0x1001f53b..0x1001f8a0): the look-at is half way between
    // the two characters' aim points; camera input turns the eye scaled by 6 / its distance.
    const float dist = std::max(distance(), 0.01f);
    const float turn = in.yaw * tuning.yawPerTick * ticks * (6.0f / dist) + in.mouseYaw;
    const float rise = in.vertical * tuning.risePerTick * ticks + in.mouseUp * dist;
    if (turn != 0.0f)
    {
        rotateEye(turn);
        inputHold_ = 10.0f;
    }
    if (rise != 0.0f)
    {
        eye_.y += rise;
        inputHold_ = 10.0f;
    }
    const glm::vec3 self = s.feet + kUp * l.selfHeight, them = l.pos + kUp * l.aimHeight;
    at_ += ((self + them) * 0.5f - at_) * perTicks(kTargetEase, ticks);

    // The eye stays within a cone about the line from the target through us (half angle atan(4 / (near + sep)), x3
    // engaged), at near..far + sep / 2 from the look-at; near / far = (7.2 / 8.6 + sep / 8 + size) x zoom / 1000.
    const float     sep  = glm::distance(s.feet, l.pos);
    const float     size = l.engaged ? l.size : 0.0f;
    const float     nearD = (tuning.lockNear + sep * 0.125f + size) * in.zoom * 0.001f;
    const float     farD  = (tuning.lockFar + sep * 0.125f + size) * in.zoom * 0.001f;
    const float     cone  = std::atan2(tuning.lockCone / (nearD + sep), 1.0f) * (l.engaged ? 3.0f : 1.0f);
    glm::vec3       off   = eye_ - at_;
    float           d     = glm::length(off);
    const glm::vec3 back  = s.feet - l.pos;
    if (d > 1e-4f && glm::length(back) > 1e-4f)
    {
        glm::vec3       u = off / d;
        const glm::vec3 w = glm::normalize(back);
        const float     a = std::acos(std::clamp(glm::dot(u, w), -1.0f, 1.0f));
        if (a > cone)
        {
            glm::vec3 axis = glm::cross(u, w);
            if (glm::length(axis) < 1e-5f)
            {
                axis = kUp;
            }
            u = glm::angleAxis(a - cone, glm::normalize(axis)) * u;
        }
        const float lo = nearD + sep * 0.5f, hi = farD + sep * 0.5f;
        const float to = std::clamp(d, lo, hi);
        d += (to - d) * perTicks(0.25f, ticks);
        eye_ = at_ + u * d;
    }
    // The line from 1 above our feet to the eye: 0.2 in front of a hit (CODE 0x1001f6a7..).
    if (ray)
    {
        const glm::vec3 from = s.feet + kUp;
        const glm::vec3 v    = eye_ - from;
        const float     lv   = glm::length(v);
        if (lv > 1e-4f)
        {
            if (const auto hit = ray(from, eye_); hit && *hit < lv)
            {
                eye_ = from + v / lv * std::max(*hit - 0.2f, 0.1f);
            }
        }
    }
    settle_ = 20.0f;
}

void GameCamera::firstPersonView(const Subject& s, const Input& in, float ticks)
{
    // CODE 0x10020ad0: the eye 0.5 behind the feet at 0.91 of the head point's height, looking 5 ahead and 0.25
    // down along the facing; the camera keys only look aside, up to 1.885 rad left / right and 1.414 up / down, and
    // the view comes back when they are let go (each offset closes a quarter of the gap a frame: 30 frames a
    // second, INFERRED). CODE 0x10021040 then eases the camera there, 1/8 of the gap a tick.
    const float frames = ticks * 0.5f;
    fpYaw_ += (in.yaw * 1.8849001f - fpYaw_) * perTicks(0.25f, frames) + in.mouseYaw;
    fpPitch_ += (in.vertical * 1.413675f - fpPitch_) * perTicks(0.25f, frames) + in.mouseUp;
    fpYaw_                = std::clamp(fpYaw_, -1.8849001f, 1.8849001f);
    fpPitch_              = std::clamp(fpPitch_, -1.413675f, 1.413675f);
    const glm::vec3 f     = glm::length(flat(s.facing)) > 1e-4f ? glm::normalize(flat(s.facing)) : glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 eye   = s.feet - f * 0.5f + kUp * (0.91f * s.headHeight);
    const float     yaw   = std::atan2(f.x, -f.z) + fpYaw_;
    const float     pitch = fpPitch_ - std::atan2(0.25f, 5.0f);
    const glm::vec3 at    = eye + viewDir(yaw, pitch) * std::sqrt(25.0f + 0.0625f);
    const float     k     = perTicks(0.125f, ticks);
    if (glm::distance(at, at_) > 0.01f)
    {
        at_ += (at - at_) * k;
    }
    if (glm::distance(eye, eye_) > 0.01f)
    {
        eye_ += (eye - eye_) * k;
    }
    settle_   = 8.0f;
    holdRise_ = std::abs(eye_.y - at_.y);
}

} // namespace xi::client
