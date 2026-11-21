// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/mog_house.h"
#include "gfx/camera.h"
#include "gfx/gamma_pass.h"
#include "gfx/zone_collision.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

using namespace xi;
using Catch::Approx;

namespace
{
// A game-space (y down) quad as two collision triangles with the given outward normal.
void quad(std::vector<dat::CollisionTriangle>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d,
          const glm::vec3& n)
{
    for (const auto& t : { std::array{ a, b, c }, std::array{ a, c, d } })
    {
        dat::CollisionTriangle tri;
        for (int k = 0; k < 3; ++k)
        {
            tri.v[k][0] = t[k].x;
            tri.v[k][1] = t[k].y;
            tri.v[k][2] = t[k].z;
        }
        tri.normal[0] = n.x;
        tri.normal[1] = n.y;
        tri.normal[2] = n.z;
        out.push_back(tri);
    }
}
} // namespace

TEST_CASE("camera: the eye is pulled in until the view target sees it (goal 22)")
{
    // Render space: the character at the origin looking down -z, 20 degrees down; the eye 8 back rises behind it (+z).
    // A plate hangs 2 behind at render y 2.0 .. 2.4: the game's ray from 1 above the feet passes under it (y 1.86
    // there), the view line from the 1.5 target does not (y 2.23). Game space: y and z negated.
    std::vector<dat::CollisionTriangle> tris;
    quad(tris, { -5.0f, -2.0f, -2.0f }, { 5.0f, -2.0f, -2.0f }, { 5.0f, -2.4f, -2.0f }, { -5.0f, -2.4f, -2.0f }, { 0.0f, 0.0f, 1.0f });
    gfx::ZoneCollision col;
    col.addCollision(tris);
    col.build();
    REQUIRE(col.hasCollision());
    const float     pitch = -0.34906584f;
    const glm::vec3 fwd(0.0f, std::sin(pitch), -std::cos(pitch));
    const glm::vec3 target(0.0f, 1.5f, 0.0f);
    // The head ray hits the plate: 0.2 short of it.
    CHECK(col.cameraDistance(target, fwd, 8.0f) < 2.2f);
    // From 1 above the feet the ray is clear, but the eye at 8 would be behind the plate: pulled in as well.
    const float low = col.cameraDistance(target, fwd, 8.0f, 0.5f);
    CHECK(low < 2.13f);
    CHECK(low > 0.5f);
    // Nothing in the way (looking the other way): the full distance.
    CHECK(col.cameraDistance(target, glm::vec3(0.0f, std::sin(pitch), std::cos(pitch)), 8.0f, 0.5f) == Approx(8.0f));
}

TEST_CASE("camera: the game's view angle is 2 atan(192 / zoom), near 0.1")
{
    // CODE 0x10015450 / 0x10015390: D3DXMatrixPerspectiveFovRH(fovY = 2 * atan2(192, zoom 350), near 0.1, far 65535).
    CHECK(gfx::gameFovY() == Approx(2.0 * std::atan2(192.0, 350.0)));
    CHECK(gfx::gameFovY() * 57.29578f == Approx(57.47f).epsilon(1e-3));
    CHECK(std::tan(gfx::gameFovY(700.0f) * 0.5f) == Approx(192.0f / 700.0f));
    CHECK(gfx::FlyCamera{}.nearZ == Approx(0.1f));
}

TEST_CASE("camera: Mog House furniture boxes stop the camera unless Config turns it off (cfg 153 bit 6)")
{
    // A 1 x 2 x 1 box 3 behind the character (render +z 3 = game z -3), centred 1 up (game y -1).
    gfx::ZoneCollision col;
    col.build();
    dat::ZoneLine box;
    box.pos[1]  = -1.0f;
    box.pos[2]  = -3.0f;
    box.yaw     = 0.3f;
    box.size[0] = 1.0f;
    box.size[1] = 2.0f;
    box.size[2] = 1.0f;
    col.setCameraSolidBoxes({ box });
    const glm::vec3 fwd(0.0f, 0.0f, -1.0f);
    const glm::vec3 target(0.0f, 1.5f, 0.0f);
    const float     d = col.cameraDistance(target, fwd, 8.0f);
    CHECK(d > 2.0f);
    CHECK(d < 2.5f);
    col.setCameraBoxes(false);
    CHECK(col.cameraDistance(target, fwd, 8.0f) == Approx(8.0f));
}

TEST_CASE("camera: a placed Armoire stops the camera (its full height, not the 1.0 movement box)")
{
    // Armoire (3 x 2, height 250) at 1F col 1 row 11, as placed live; the character 5 to its +x, camera looking +x.
    namespace mog = xi::client::mog;
    const dat::FurnitureRow armoire{ 2, 61, 3, 2, 250 };
    const auto              pose = mog::furniturePose(armoire, 1, 0, 11, 0, 0, 0);
    REQUIRE(pose);
    const auto cb = mog::furnitureCameraBox(armoire, *pose, 0);
    REQUIRE(cb);
    CHECK(cb->pos[1] + cb->size[1] * 0.5f == Approx(pose->pos.y));        // bottom on the floor
    CHECK(cb->pos[1] - cb->size[1] * 0.5f == Approx(pose->pos.y - 2.5f)); // top 2.5 up
    gfx::ZoneCollision col;
    col.build();
    const glm::vec3 fwd(1.0f, -0.25f, 0.0f);
    const glm::vec3 target(pose->pos.x + 5.0f, 1.5f, -pose->pos.z); // render space: z flipped
    // The movement box tops out level with the ray's start (1 above the feet), so it never stopped the camera.
    col.setCameraSolidBoxes({ *mog::furnitureBox(armoire, *pose, 0) });
    CHECK(col.cameraDistance(target, glm::normalize(fwd), 8.0f, 0.5f) == Approx(8.0f));
    col.setCameraSolidBoxes({ *cb });
    const float d = col.cameraDistance(target, glm::normalize(fwd), 8.0f, 0.5f);
    CHECK(d < 4.5f);
    CHECK(d > 3.0f);
}

TEST_CASE("camera: Config aspect ratio (cfg 67) replaces the window's shape")
{
    CHECK(gfx::aspectOption(3) == Approx(16.0f / 9.0f));
    CHECK(gfx::aspectOption(9) == Approx(5.0f / 4.0f));
    gfx::configuredAspect() = gfx::aspectOption(0);
    CHECK(gfx::gameAspect(16.0f / 9.0f) == Approx(4.0f / 3.0f * 0.75f / 0.791f));
    gfx::configuredAspect() = 0.0f;
    CHECK(gfx::gameAspect(16.0f / 9.0f) == Approx(16.0f / 9.0f * 0.75f / 0.791f));
}

TEST_CASE("gamma: Config 50 is the look as drawn; the exponent is 1.5 / (1 + v / 100)")
{
    CHECK(gfx::GammaPass::exponent(50) == Approx(1.0f));
    CHECK(gfx::GammaPass::exponent(0) == Approx(1.5f));
    CHECK(gfx::GammaPass::exponent(100) == Approx(0.75f));
}
