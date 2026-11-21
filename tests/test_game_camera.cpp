// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/game_camera.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <cmath>
#include <optional>

using namespace xi;
using Catch::Approx;
using client::GameCamera;

namespace
{
// Render space, y up; the character faces -z.
GameCamera::Subject subject(const glm::vec3& feet)
{
    GameCamera::Subject s;
    s.feet   = feet;
    s.facing = glm::vec3(0.0f, 0.0f, -1.0f);
    s.height = 1.8f;
    return s;
}

const GameCamera::Ray kOpen = [](const glm::vec3&, const glm::vec3&) -> std::optional<float> { return std::nullopt; };

// A wall across z = wallZ (the plane), blocking any segment that crosses it.
GameCamera::Ray wallAt(float wallZ)
{
    return [wallZ](const glm::vec3& a, const glm::vec3& b) -> std::optional<float> {
        if ((a.z - wallZ) * (b.z - wallZ) >= 0.0f)
        {
            return std::nullopt;
        }
        const float t = (wallZ - a.z) / (b.z - a.z);
        return t * glm::distance(a, b);
    };
}

void run(GameCamera& c, const GameCamera::Subject& s, const GameCamera::Input& in, int frames, const GameCamera::Ray& ray = kOpen,
         std::optional<GameCamera::Lock> lock = std::nullopt, bool fp = false)
{
    for (int i = 0; i < frames; ++i)
    {
        c.update(s, in, lock, fp, 1.0f, ray, {});
    }
}
} // namespace

TEST_CASE("game camera: placed 4 behind and 2 up, the look-at 0.5 up (CODE 0x100215b0)")
{
    GameCamera c;
    c.reset(subject(glm::vec3(0.0f)));
    CHECK(c.eye().x == Approx(0.0f));
    CHECK(c.eye().y == Approx(2.0f));
    CHECK(c.eye().z == Approx(4.0f));
    CHECK(c.at().y == Approx(0.5f));
    // The look-at eases to 0.6 of the model height.
    run(c, subject(glm::vec3(0.0f)), {}, 60);
    CHECK(c.at().y == Approx(0.6f * 1.8f).margin(1e-3));
}

TEST_CASE("game camera: the eye trails the character and settles at 6 (CODE 0x1001fc2f)")
{
    GameCamera c;
    auto       s = subject(glm::vec3(0.0f));
    c.reset(s);
    // Run away from the eye (-z) at 5 yalms a second for 4 seconds, then stop.
    for (int i = 0; i < 240; ++i)
    {
        s.feet.z -= 5.0f / 60.0f;
        c.update(s, {}, std::nullopt, false, 1.0f, kOpen, {});
    }
    CHECK(c.distance() > 6.2f); // trailing while running
    CHECK(c.distance() < 7.5f);
    run(c, s, {}, 600);
    CHECK(c.distance() == Approx(6.0f).margin(0.05));
    // Walking back toward the eye: it holds its ground until 3 (level), then backs off.
    for (int i = 0; i < 60; ++i)
    {
        s.feet.z += 4.0f / 60.0f;
        c.update(s, {}, std::nullopt, false, 1.0f, kOpen, {});
    }
    run(c, s, {}, 600);
    CHECK(glm::length(glm::vec2(c.eye().x - c.at().x, c.eye().z - c.at().z)) == Approx(3.0f).margin(0.05));
}

TEST_CASE("game camera: the keys turn the eye 0.0279 rad a tick and raise it 0.1067")
{
    GameCamera c;
    const auto s = subject(glm::vec3(0.0f));
    c.reset(s);
    run(c, s, {}, 60);
    const float     yaw0 = c.yaw();
    GameCamera::Input in;
    in.yaw = 1.0f;
    run(c, s, in, 30);
    CHECK(std::remainder(c.yaw() - yaw0, 6.2831853f) == Approx(30 * GameCamera::kYawPerTick).margin(1e-3));
    const float rise0 = c.eye().y;
    in                = {};
    in.vertical       = 1.0f;
    run(c, s, in, 10);
    CHECK(c.eye().y - rise0 == Approx(10 * GameCamera::kRisePerTick).margin(0.05));
}

TEST_CASE("game camera: left alone, the eye keeps its height over the look-at while following")
{
    GameCamera c;
    auto       s = subject(glm::vec3(0.0f));
    c.reset(s);
    run(c, s, {}, 120);
    const float rise = c.eye().y - c.at().y;
    for (int i = 0; i < 300; ++i)
    {
        s.feet.z -= 5.0f / 60.0f;
        c.update(s, {}, std::nullopt, false, 1.0f, kOpen, {});
    }
    CHECK(c.eye().y - c.at().y == Approx(rise).margin(0.02));
}

TEST_CASE("game camera: the eye stops 0.5 in front of a wall between it and the look-at")
{
    GameCamera c;
    const auto s = subject(glm::vec3(0.0f));
    c.reset(s);
    run(c, s, {}, 5);
    // A wall 2 behind the character (+z): the eye (4 behind) ends 0.5 in front of it along the view line.
    run(c, s, {}, 1, wallAt(2.0f));
    CHECK(c.eye().z < 2.0f);
    CHECK(c.eye().z > 1.0f);
}

TEST_CASE("game camera: locked behind swings the eye behind the character (cfg 145)")
{
    GameCamera c;
    auto       s = subject(glm::vec3(0.0f));
    c.reset(s);
    s.facing = glm::vec3(1.0f, 0.0f, 0.0f); // turned to face +x: the eye (at +z) is now to its side
    GameCamera::Input in;
    in.lockedBehind = true;
    run(c, s, in, 600);
    CHECK(c.eye().x < c.at().x - 2.0f); // behind = -x
    CHECK(std::abs(c.eye().z - c.at().z) < 0.5f);
}

TEST_CASE("game camera: /lockon keeps the eye behind us, looking at the middle")
{
    GameCamera c;
    const auto s = subject(glm::vec3(0.0f));
    c.reset(s);
    GameCamera::Lock l;
    l.pos = glm::vec3(10.0f, 0.0f, 0.0f); // the target to the side (+x)
    GameCamera::Input in;
    run(c, s, in, 600, kOpen, l);
    // The eye is on the far side of us from the target (-x), the look-at half way.
    CHECK(c.eye().x < 0.0f);
    CHECK(c.at().x == Approx(5.0f).margin(0.05));
    const glm::vec3 u = glm::normalize(c.eye() - c.at()), w(-1.0f, 0.0f, 0.0f);
    CHECK(std::acos(glm::dot(u, w)) <= std::atan2(4.0f / ((7.2f + 10.0f * 0.125f) * 0.35f + 10.0f), 1.0f) + 0.02f);
}

TEST_CASE("game camera: first person looks along the facing; the look-aside comes back")
{
    GameCamera c;
    const auto s = subject(glm::vec3(0.0f));
    c.reset(s);
    run(c, s, {}, 300, kOpen, std::nullopt, true);
    CHECK(c.yaw() == Approx(0.0f).margin(0.01)); // facing -z
    CHECK(c.eye().y == Approx(0.91f * s.headHeight).margin(0.02));
    GameCamera::Input in;
    in.yaw = 1.0f;
    run(c, s, in, 120, kOpen, std::nullopt, true);
    CHECK(c.yaw() == Approx(1.8849f).margin(0.05));
    run(c, s, {}, 300, kOpen, std::nullopt, true);
    CHECK(c.yaw() == Approx(0.0f).margin(0.01));
}
