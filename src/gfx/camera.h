// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdlib>

namespace xi::gfx
{

// The game's view angle (CODE 0x10015450): D3DXMatrixPerspectiveFovRH with fovY = 2 * atan2(192, zoom), zoom 350
// normally (57.5 degrees; event shots change it), near 0.1, far 65535 (CODE 0x10015390).
[[nodiscard]] inline float gameFovY(float zoom = 350.0f)
{
    return 2.0f * std::atan2(192.0f, zoom);
}

// The game's projection aspect for a window of aspect x / y (CODE 0x10002e80 -> 0x10015360 -> 0x10015450): 1 /
// (y / (0.75 x) * 0.791), the 0.791 being the camera object's +0x2e8 (ctor 0x1001086b; its only other writer,
// 0x100153f0, is never called). So 0.948 of the window's shape: the picture is drawn ~5.5% wider than true.
// XI_ASPECT_WINDOW=1 uses the window's own aspect.
// Config Misc. 2 "aspect ratio" (cfg 67, CODE 0x10002e60 -> 0x10002e80 -> 0x10015360): the game takes x / y from table
// 0x1034f4a0 (0 4:3, 1 15:9, 2 15:10, 3 16:9, 4 16:10, 5 5:4), the monitor's shape, never the back buffer's. Here
// the window's shape stands in until the player picks one (configuredAspect() = 0).
[[nodiscard]] inline float& configuredAspect()
{
    static float a = 0.0f;
    return a;
}
// The table's x / y for cfg 67 value v (clamped to 0..5).
[[nodiscard]] inline float aspectOption(int v)
{
    static constexpr float kRatio[6][2] = { { 4, 3 }, { 15, 9 }, { 15, 10 }, { 16, 9 }, { 16, 10 }, { 5, 4 } };
    const int              i            = v < 0 ? 0 : v > 5 ? 5 : v;
    return kRatio[i][0] / kRatio[i][1];
}
// An addon's value for the camera's +0x2F0, y / (0.75 x) (Ashita's aspect addon); 0 = none, the shape above decides.
[[nodiscard]] inline float& aspectFactorOverride()
{
    static float v = 0.0f;
    return v;
}
[[nodiscard]] inline float gameAspect(float windowAspect)
{
    static const bool window = std::getenv("XI_ASPECT_WINDOW") != nullptr;
    const float       shape  = configuredAspect() > 0.0f ? configuredAspect() : windowAspect;
    if (!window && aspectFactorOverride() > 0.0f)
    {
        return 1.0f / (aspectFactorOverride() * 0.791f);
    }
    return window ? windowAspect : shape * 0.75f / 0.791f;
}

// Free-fly camera. World is Y-up, right-handed. Projection is reverse-Z with an infinite far plane
// (depth 1 at the near plane, 0 at infinity) for precision over large zones.
struct FlyCamera
{
    glm::vec3 position{ 0.0f, 10.0f, 0.0f };
    float     yaw   = 0.0f; // radians, 0 = looking down -Z
    float     pitch = 0.0f;
    float     fovY  = glm::radians(60.0f);
    float     nearZ = 0.1f;

    [[nodiscard]] glm::vec3 forward() const
    {
        return { std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw) };
    }
    [[nodiscard]] glm::vec3 right() const
    {
        return glm::normalize(glm::cross(forward(), glm::vec3(0, 1, 0)));
    }
    [[nodiscard]] glm::mat4 view() const
    {
        return glm::lookAt(position, position + forward(), glm::vec3(0, 1, 0));
    }
    [[nodiscard]] glm::mat4 projection(float aspect) const
    {
        const float f = 1.0f / std::tan(fovY * 0.5f);
        glm::mat4   p(0.0f);
        p[0][0] = f / aspect;
        p[1][1] = -f; // Vulkan clip space has +Y down
        p[2][3] = -1.0f;
        p[3][2] = nearZ;
        return p;
    }
};

} // namespace xi::gfx
