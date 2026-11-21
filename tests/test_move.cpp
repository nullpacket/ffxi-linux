// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/zone_collision.h"

#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using xi::gfx::ZoneCollision;
using Catch::Approx;

namespace
{

// A quad from render-space corners (Y up), as two collision triangles in game space (Y down, Z negated) with the normal
// of a -> b -> c.
void quad(std::vector<xi::dat::CollisionTriangle>& out, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, bool barrier = false)
{
    const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
    for (const auto& t : { std::array{ a, b, c }, std::array{ a, c, d } })
    {
        xi::dat::CollisionTriangle tri;
        for (int k = 0; k < 3; ++k)
        {
            tri.v[k][0] = t[k].x;
            tri.v[k][1] = -t[k].y;
            tri.v[k][2] = -t[k].z;
        }
        tri.normal[0] = n.x;
        tri.normal[1] = -n.y;
        tri.normal[2] = -n.z;
        tri.barrier   = barrier;
        out.push_back(tri);
    }
}

// Floor y = 0 over x, z in [-10, 10]; a wall of height `h` facing -x at x = 2 for z in [-10, 10] with a top at y = h
// behind it (a step when low); optionally the wall is a barrier.
ZoneCollision room(float h, bool barrier = false, bool sphere = true)
{
    std::vector<xi::dat::CollisionTriangle> t;
    quad(t, { -10, 0, -10 }, { -10, 0, 10 }, { 2, 0, 10 }, { 2, 0, -10 });                  // floor (normal +y)
    quad(t, { 2, 0, -10 }, { 2, 0, 10 }, { 2, h, 10 }, { 2, h, -10 }, barrier);             // wall face (normal -x)
    quad(t, { 2, h, -10 }, { 2, h, 10 }, { 10, h, 10 }, { 10, h, -10 });                    // top
    ZoneCollision col;
    col.addCollision(t);
    col.build();
    col.setSphere(sphere);
    return col;
}

glm::vec3 walk(const ZoneCollision& col, glm::vec3 p, glm::vec3 dir, int frames)
{
    for (int i = 0; i < frames; ++i)
    {
        p = col.move(p, dir * (5.0f / 60.0f));
    }
    return p;
}

} // namespace

TEST_CASE("swept sphere: walls stop the feet kRadius away and the move slides along them")
{
    for (const bool sphere : { true, false })
    {
        const auto col = room(3.0f, false, sphere);
        auto       p   = walk(col, { 0, 0, 0 }, { 1, 0, 0 }, 60);
        CHECK(p.x == Approx(2.0f - ZoneCollision::kRadius).margin(0.01f));
        CHECK(p.y == Approx(0.0f));
        // Into the wall at 45 degrees: x stays, z keeps going.
        const auto q = walk(col, p, glm::normalize(glm::vec3(1, 0, 1)), 30);
        CHECK(q.x == Approx(p.x).margin(0.01f));
        CHECK(q.z > p.z + 1.5f);
    }
}

TEST_CASE("swept sphere: steps up to the step-up are climbed, higher ones block")
{
    auto p = walk(room(0.7f), { 0, 0, 0 }, { 1, 0, 0 }, 60);
    CHECK(p.x > 2.5f);
    CHECK(p.y == Approx(0.7f));
    p = walk(room(0.8f), { 0, 0, 0 }, { 1, 0, 0 }, 60);
    CHECK(p.x == Approx(1.5f).margin(0.01f));
    CHECK(p.y == Approx(0.0f));
    // The game's own sphere (step-up 0.5 here) does not climb 0.7.
    auto col = room(0.7f);
    col.setStepUp(0.5f);
    p = walk(col, { 0, 0, 0 }, { 1, 0, 0 }, 60);
    CHECK(p.x < 2.0f);
}

TEST_CASE("swept sphere: one long move is marched, not jumped through a wall")
{
    const auto col = room(3.0f);
    const auto p   = col.move({ 0, 0, 0 }, { 6, 0, 0 }); // two sub-steps of 3, marched 0.15 at a time
    CHECK(p.x == Approx(1.5f).margin(0.16f));
    CHECK(p.x < 2.0f - ZoneCollision::kRadius + 0.01f);
}

TEST_CASE("swept sphere: barriers block the player but not the camera's ray")
{
    const auto col = room(3.0f, true);
    CHECK(walk(col, { 0, 0, 0 }, { 1, 0, 0 }, 60).x == Approx(1.5f).margin(0.01f));
    CHECK_FALSE(col.rayHit({ 0, 1, 0 }, { 1, 0, 0 }, 5.0f, true));
    CHECK(col.rayHit({ 0, 1, 0 }, { 1, 0, 0 }, 5.0f, false));
}

TEST_CASE("swept sphere: a beam above the sphere does not block; a start inside a wall eases out, not deeper")
{
    // A beam from 1.2 to 2 over the floor: above the sphere's top (1.0).
    std::vector<xi::dat::CollisionTriangle> t;
    quad(t, { -10, 0, -10 }, { -10, 0, 10 }, { 10, 0, 10 }, { 10, 0, -10 });
    quad(t, { 2, 1.2f, -10 }, { 2, 1.2f, 10 }, { 2, 2, 10 }, { 2, 2, -10 });
    ZoneCollision col;
    col.addCollision(t);
    col.build();
    CHECK(walk(col, { 0, 0, 0 }, { 1, 0, 0 }, 60).x > 4.0f);

    // Feet 0.2 from a wall: walking in is refused, walking out works.
    const auto w = room(3.0f);
    CHECK(w.move({ 1.8f, 0, 0 }, { 0.08f, 0, 0 }).x <= 1.8f + 1e-4f);
    CHECK(w.move({ 1.8f, 0, 0 }, { -0.08f, 0, 0 }).x == Approx(1.72f));
    // Along it: it eases out by at most 0.05 a move.
    const auto a = w.move({ 1.8f, 0, 0 }, { 0, 0, 0.08f });
    CHECK(a.z == Approx(0.08f).margin(0.01f));
    CHECK(a.x <= 1.8f + 1e-4f);
    CHECK(a.x >= 1.75f - 1e-3f);
}

TEST_CASE("ground probe: a seam covered only by a steep face is not a hole")
{
    // Two floors joined by a 0.2-high face 0.05 deep (n.y ~0.24: a wall, like the side of Gusgen's rail sleepers). A
    // short step (a 240 fps frame) that lands over the face still finds ground there instead of being refused.
    std::vector<xi::dat::CollisionTriangle> t;
    quad(t, { -10, 0, -10 }, { -10, 0, 0 }, { 10, 0, 0 }, { 10, 0, -10 });
    quad(t, { -10, 0, 0 }, { -10, 0.2f, 0.05f }, { 10, 0.2f, 0.05f }, { 10, 0, 0 });
    quad(t, { -10, 0.2f, 0.05f }, { -10, 0.2f, 10 }, { 10, 0.2f, 10 }, { 10, 0.2f, 0.05f });
    ZoneCollision col;
    col.addCollision(t);
    col.build();
    CHECK(!col.groundHeight(0.0f, 0.025f, 1.0f)); // no floor triangle there
    REQUIRE(col.seamHeight(0.0f, 0.025f, -0.5f, 0.75f));
    CHECK(*col.seamHeight(0.0f, 0.025f, -0.5f, 0.75f) == Approx(0.1f).margin(0.01f));
    glm::vec3 p(0.0f, 0.0f, -1.0f);
    for (int i = 0; i < 240; ++i)
    {
        p = col.move(p, glm::vec3(0.0f, 0.0f, 5.0f / 240.0f));
    }
    CHECK(p.z > 3.0f);
    CHECK(p.y == Approx(0.2f));
}

TEST_CASE("ground probe: a stair sliver over the sea floor holds the step; a ledge's lip still falls")
{
    // Port Bastok's harbour stairs (game (74.5, -23.8)): two treads 0.27 apart joined by a riser 0.02 deep, over the sea
    // floor 2.2 below. Crossing the gap, or walking along it, stays on the stairs; walking off a ledge with only the sea
    // beyond drops to the sea floor (the game has no drop limit).
    std::vector<xi::dat::CollisionTriangle> t;
    quad(t, { -10, 0, -10 }, { -10, 0, 0 }, { 10, 0, 0 }, { 10, 0, -10 });
    quad(t, { -10, 0, 0 }, { -10, 0.27f, 0.02f }, { 10, 0.27f, 0.02f }, { 10, 0, 0 });
    quad(t, { -10, 0.27f, 0.02f }, { -10, 0.27f, 10 }, { 10, 0.27f, 10 }, { 10, 0.27f, 0.02f });
    quad(t, { -30, -2.2f, -30 }, { -30, -2.2f, 30 }, { 30, -2.2f, 30 }, { 30, -2.2f, -30 }); // the sea floor
    ZoneCollision col;
    col.addCollision(t);
    col.build();
    REQUIRE(col.groundHeight(0.0f, 0.01f, 1.0f));
    CHECK(*col.groundHeight(0.0f, 0.01f, 1.0f) == Approx(-2.2f)); // only the sea floor under the gap
    REQUIRE(col.sliverHeight(0.0f, 0.01f, -0.4f, 0.2f)); // standing in the gap (the client's gravity): the riser
    CHECK(*col.sliverHeight(0.0f, 0.01f, -0.4f, 0.2f) == Approx(0.135f).margin(0.02f));
    CHECK(!col.sliverHeight(0.0f, 10.2f, -0.4f, 0.4f)); // past the top tread's far edge: nothing to stand on
    glm::vec3 up(0.0f, 0.0f, -1.0f);
    for (int i = 0; i < 240; ++i)
    {
        up = col.move(up, glm::vec3(0.0f, 0.0f, 5.0f / 240.0f));
        CHECK(up.y > -0.1f);
    }
    CHECK(up.y == Approx(0.27f));
    glm::vec3 along(-5.0f, 0.0f, 0.01f); // in the gap, walking along it
    along = col.move(along, glm::vec3(0.05f, 0.0f, 0.0f));
    CHECK(along.y > -0.1f);
    glm::vec3 off(0.0f, 0.27f, 9.0f); // off the far edge of the upper tread: the sea
    for (int i = 0; i < 60; ++i)
    {
        off = col.move(off, glm::vec3(0.0f, 0.0f, 5.0f / 60.0f));
    }
    CHECK(off.z > 10.5f);
    CHECK(off.y == Approx(-2.2f));
}
