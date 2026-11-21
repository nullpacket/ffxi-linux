// SPDX-License-Identifier: GPL-3.0-or-later
// The game's player camera (FFXiMain.dll CODE 0x1001ed90 third person, 0x10020ad0 / 0x10021040 first person): an eye
// point that trails a look-at point, not an orbit at a fixed distance. dat-format.md §7.8b "Camera follow".
#pragma once

#include <glm/vec3.hpp>

#include <array>
#include <functional>
#include <optional>

namespace xi::client
{

class GameCamera
{
public:
    // Render space (y up). `facing` is the character's facing, a unit vector in x / z.
    struct Subject
    {
        glm::vec3 feet{ 0.0f };
        glm::vec3 facing{ 0.0f, 0.0f, -1.0f };
        float     height     = 1.8f;  // model height (the game's skeleton bounds, here the head bone + 0.2)
        float     headHeight = 1.6f;  // the head point above the feet (first person)
        bool      moving     = false; // the character moved this frame
        float     platformDy = 0.0f;  // the rise of a lift or ship under the feet this frame (the eye rides along)
    };
    // /lockon: the locked target (render space).
    struct Lock
    {
        glm::vec3 pos{ 0.0f };
        float     aimHeight  = 1.0f; // the target's aim point above its feet (the game's point 7, INFERRED)
        float     selfHeight = 1.5f; // ours (point 13, INFERRED)
        float     size       = 0.0f; // engaged: added to the near / far band (the game's bounds x 1.5, INFERRED)
        bool      engaged    = false;
    };
    struct Input
    {
        float yaw      = 0.0f; // camera keys / stick, -1..1, inversion applied (+ swings the eye to the right)
        float vertical = 0.0f; // -1..1 (+ raises the eye)
        float mouseYaw = 0.0f; // radians this frame (orbit drag)
        float mouseUp  = 0.0f; // radians this frame (+ raises the eye)
        bool  recentre = false;      // Home (action 0x51)
        bool  lockedBehind = false;  // Config Mouse/Cam. "locked behind" (cfg 145, the game's 0x91)
        float zoom         = 350.0f; // the projection zoom (lock-on's distance band scales with it)
    };
    // Collision: the distance from `from` toward `to` to the first camera-blocking surface (barriers skipped), and
    // the eye's clearance from ground, ceiling and side walls (ZoneCollision::cameraClearance).
    using Ray       = std::function<std::optional<float>(const glm::vec3& from, const glm::vec3& to)>;
    using Clearance = std::function<glm::vec3(const glm::vec3& eye, const glm::vec3& forward)>;

    // Behind the character as the game places it (CODE 0x100215b0: (-4, -2, 0) in the actor's frame, the look-at
    // 0.5 above the feet). With `yaw` / `pitch` / `dist` the test hooks' view instead (--yaw, --cam-pitch, --cam-dist).
    void reset(const Subject& s);
    void resetAt(const Subject& s, float yaw, float pitch, float dist);
    [[nodiscard]] bool placed() const { return placed_; }

    // One frame of `ticks` 1/60 s ticks. `firstPerson` (Change View): the eye at the head looking along the facing.
    void update(const Subject& s, const Input& in, const std::optional<Lock>& lock, bool firstPerson, float ticks, const Ray& ray,
                const Clearance& clearance);

    [[nodiscard]] const glm::vec3& eye() const { return eye_; }
    [[nodiscard]] const glm::vec3& at() const { return at_; }
    [[nodiscard]] float yaw() const;   // forward = (sin yaw, ., -cos yaw)
    [[nodiscard]] float pitch() const; // up positive
    [[nodiscard]] float distance() const;

    // The game's tuning (CODE 0x1001ed90; DATA 0x1032a3c0.. ; dat-format §7.8b).
    static constexpr float kYawPerTick    = 0.027924445f; // camera keys: radians a tick (1.68 rad/s)
    static constexpr float kRisePerTick   = 0.10666667f;  // camera keys: eye height a tick
    static constexpr float kFollow        = 6.0f;         // beyond this the eye is pulled after the look-at
    static constexpr float kFollowRate    = 0.012f;       // x (d - 6) of the eye-to-look-at vector a tick
    static constexpr float kFar           = 100.0f;       // beyond this (d - 100) x 0.5 a tick
    static constexpr float kNear          = 3.0f;         // nearer than this, with room behind, pushed out
    static constexpr float kNearRate      = 0.125f;       // x (3 - d) a tick
    static constexpr float kTargetEase    = 0.25f;        // the look-at closes this much of the gap a tick
    static constexpr float kTargetHeight  = 0.6f;         // x the model height
    static constexpr float kWallGap       = 0.5f;         // the eye stops this far in front of a hit
    static constexpr float kSwing         = 0.025f;       // locked behind: a tick (cfg 0x1035122c 50 x 0.01 x 0.05)
    static constexpr float kLockNear      = 7.2f;         // lock-on band (DATA 0x1032a3a8)
    static constexpr float kLockFar       = 8.6f;         // (DATA 0x1032a3a4)
    static constexpr float kLockCone      = 4.0f;         // the cone's half angle atan(this / ...) (DATA 0x1032a3a0)

    // The constants above that addons change in the game's memory (Ashita's xicamera; scan_registry.h NativeHooks):
    // the game's values unless one does.
    struct Tuning
    {
        float yawPerTick  = kYawPerTick;
        float risePerTick = kRisePerTick;
        float follow      = kFollow;
        float nearDist    = kNear;
        float lockNear    = kLockNear;
        float lockFar     = kLockFar;
        float lockCone    = kLockCone;
    };
    Tuning tuning;

private:
    void thirdPerson(const Subject& s, const Input& in, float ticks, const Ray& ray);
    void lockOn(const Subject& s, const Input& in, const Lock& l, float ticks, const Ray& ray);
    void firstPersonView(const Subject& s, const Input& in, float ticks);
    void rotateEye(float radians);
    void record(const glm::vec3& at);

    glm::vec3 eye_{ 0.0f }, at_{ 0.0f };
    bool      placed_ = false;
    float     holdRise_ = 0.0f;   // DAT_10456d78: the eye's height over the look-at at the end of the last frame
    float     inputHold_ = 0.0f;  // DAT_10456d74: ticks since camera input (10 after any)
    float     settle_ = 0.0f;     // DAT_10456d7c: ticks the follow rules wait (after first person 8, lock-on 20)
    float     swingHold_ = 0.0f;  // DAT_10456d8c: ticks before locked-behind swings again after input
    glm::vec3 swingFrom_{ 0.0f }; // DAT_10456d90: where the character was at the last camera input
    float     recentre_ = 0.0f;   // DAT_10456d88: Home's frames left
    bool      backtrack_ = false; // DAT_10456d70: walking the eye back along the look-at's trail
    std::array<glm::vec3, 4> trail_{}; // the camera's +0xbc..: the look-at's last positions 0.1 apart
    int       trailCount_ = 0;         // +0xf0
    // First person (DAT_10456d64 / 0x10456d68): the look offsets, eased toward the stick.
    float fpPitch_ = 0.0f, fpYaw_ = 0.0f;
    bool  wasFirst_ = false, wasLocked_ = false;
    // Ours: a standing eye that keeps turning back and forth is held (update()).
    glm::vec3 stillAt_{ 0.0f }, frozenEye_{ 0.0f };
    float     lastDist_ = 0.0f, lastDelta_ = 0.0f, turns_ = 0.0f;
    bool      frozen_   = false;
};

} // namespace xi::client
