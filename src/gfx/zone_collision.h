// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/collision.h"
#include "dat/zone.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

namespace xi::gfx
{

// A spatial grid over XZ of triangles in render space (3 vertices each).
struct TriGrid
{
    static constexpr float kCell = 8.0f;

    std::vector<glm::vec3>             tris;
    std::vector<std::vector<uint32_t>> cells;
    glm::vec2                          min{ 0.0f };
    int                                w = 0, h = 0;

    void build();
    // Calls f(const glm::vec3* tri) for each triangle in the cells overlapping the XZ box; a triangle spanning several
    // cells may come more than once (callers are idempotent).
    template <typename F>
    void visit(float x0, float z0, float x1, float z1, F&& f) const
    {
        if (cells.empty())
        {
            return;
        }
        const int cx0 = std::max(0, static_cast<int>((x0 - min.x) / kCell));
        const int cz0 = std::max(0, static_cast<int>((z0 - min.y) / kCell));
        const int cx1 = std::min(w - 1, static_cast<int>((x1 - min.x) / kCell));
        const int cz1 = std::min(h - 1, static_cast<int>((z1 - min.y) / kCell));
        for (int z = cz0; z <= cz1; ++z)
        {
            for (int x = cx0; x <= cx1; ++x)
            {
                for (const auto t : cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)])
                {
                    f(&tris[t * 3]);
                }
            }
        }
    }
};

// The zone's walkable world on the CPU, in render space (Y up): floors and walls from the collision mesh (or the render
// mesh's up-facing triangles when a zone has none), door boxes, and the queries movement, the camera, footsteps and
// lens flares use. No GPU state, so tests and tools can load it alone (dat-format §7).
class ZoneCollision
{
public:
    // The game's player is a sphere of radius 0.5 centred 0.5 above the feet (CODE 0x1016e840): it rides over edges up to
    // about its radius; the ground probe reaches 40 below (CODE 0x10169a50). Step-up 0.75, not 0.5: Davoi and Bastok Mines
    // stairs have 0.6 - 0.7 risers that 0.5 cannot climb (xi-dat reach, 2026-10-18).
    static constexpr float kRadius  = 0.5f;
    static constexpr float kStepUp  = 0.75f;
    static constexpr float kMaxDrop = 40.0f;
    // Gravity while falling (CODE 0x100a4e00): the fall speed grows 0.0408 per 1/60 s tick, at most 1 per tick.
    static constexpr float kGravityPerTick = 0.040833335f;
    static constexpr float kMaxFallPerTick = 1.0f;

    // Adds the zone's collision mesh (game space): up-facing triangles become floors, near-vertical ones walls,
    // ceilings are dropped.
    void addCollision(const std::vector<dat::CollisionTriangle>& tris);
    // A render-mesh floor triangle (render space), for zones without collision data.
    void addFloor(const glm::vec3* v);
    void addDoorBox(const dat::ZoneLine& box)
    {
        doorBoxes_.push_back({ box, true });
    }
    // Solid boxes that change while the zone is loaded (Mog House furniture), game space like the door boxes; replaces
    // the previous set.
    void setSolidBoxes(std::vector<dat::ZoneLine> boxes)
    {
        solidBoxes_ = std::move(boxes);
    }
    // Config Misc. 3 Mog House furniture camera collision (cfg 153 bit 6 clear, the default; CODE 0x1016bc33): the
    // camera ray also stops at the furniture's camera boxes (setCameraSolidBoxes). The game tests the furniture object's
    // own collision (0x106306b4 -> 0x100c4af0); full-height footprint boxes stand in (INFERRED).
    void setCameraSolidBoxes(std::vector<dat::ZoneLine> boxes)
    {
        cameraSolidBoxes_ = std::move(boxes);
    }
    void setCameraBoxes(bool on)
    {
        cameraBoxes_ = on;
    }
    void build();

    // Highest walkable surface at (x, z) whose height is <= maxY. nullopt if none.
    [[nodiscard]] std::optional<float> groundHeight(float x, float z, float maxY) const;
    // move()'s fallback where no floor triangle covers (x, z): an upward-facing steep face in [minY, maxY].
    [[nodiscard]] std::optional<float> seamHeight(float x, float z, float minY, float maxY) const;
    // A floor 0.3 along dir from p, in [minY, maxY] (move() crosses a seam only toward one).
    [[nodiscard]] bool floorAhead(const glm::vec3& p, glm::vec2 dir, float minY, float maxY) const;
    // A seam in [minY, maxY] with floor within 0.5 of its height 0.3 to both sides of it along x, z or a diagonal: a sliver
    // between floor triangles (a stair riser's 0.02 gap), which the game's probe stands on even over a lower floor;
    // a ledge's lip has floor on one side only.
    [[nodiscard]] std::optional<float> sliverHeight(float x, float z, float minY, float maxY) const;
    [[nodiscard]] uint8_t              mapIdAt(float x, float z, float maxY) const;
    [[nodiscard]] uint8_t              materialAt(float x, float z, float maxY) const;
    [[nodiscard]] bool                 roofedAt(float x, float z, float maxY) const;
    [[nodiscard]] bool                 rayBlocked(const glm::vec3& from, const glm::vec3& dir, float maxDist) const;
    // `skipBarriers`: the invisible barrier walls do not count (the camera's ray).
    [[nodiscard]] std::optional<float> rayHit(const glm::vec3& from, const glm::vec3& dir, float maxDist, bool skipBarriers = false) const;
    // Character movement: horizontal step, wall push-out with sliding, step-up of at most kStepUp, then floor snap.
    // Without collision data only the floor is followed. Walls use the game's swept sphere (sweepSphere) unless
    // XI_MOVE_OLD=1 picks the earlier wall slices (setSphere for tools).
    [[nodiscard]] glm::vec3 move(const glm::vec3& feet, const glm::vec3& delta) const;
    // The game's move resolver (CODE 0x1016e6f0 / 0x1016e840 / 0x1016eff0) with the step-up hybrid: the sphere of radius
    // kRadius is centred max(kRadius, stepUp) above the feet and meets wall triangles clipped to the band from the
    // step-up to 2 * kRadius over the feet (lower parts are steps the floor snap climbs). The move is split into
    // sub-steps of at most 4, each marched in 0.15 steps (0.3 r) to the first contact, where the sphere is pushed out
    // sideways along the contact normals (kPushPasses; still inside -> the last free position). A wall the move
    // starts inside asks only the start's distance + 0.05 (eases out), and a path crossing a wall is refused.
    [[nodiscard]] glm::vec3 sweepSphere(const glm::vec3& feet, const glm::vec3& target) const;
    void setSphere(bool on)
    {
        sphere_ = on;
    }
    [[nodiscard]] bool sphere() const
    {
        return sphere_;
    }
    // The highest step the character climbs (kStepUp by default; tools compare other values).
    void setStepUp(float h)
    {
        stepUp_ = h;
    }
    [[nodiscard]] float stepUp() const
    {
        return stepUp_;
    }
    // Does the straight path between two foot positions cross a wall taller than a step (above the step-up)?
    [[nodiscard]] bool crossesWall(const glm::vec3& from, const glm::vec3& to) const;
    // Third-person camera (CODE 0x1001ed90): cameraDistance pulls the eye in to 0.2 short of the first non-barrier
    // triangle between `target` and `dist` back along -forward (with `rayDrop`, the ray starts that far below the target
    // and the eye is then pulled in until the target sees it); cameraClearance then keeps the eye kCamClear from the
    // ground below it, a ceiling above it and walls to its left and right.
    static constexpr float kCamClear = 0.17f;
    [[nodiscard]] float     cameraDistance(const glm::vec3& target, const glm::vec3& forward, float dist, float rayDrop = 0.0f) const;
    [[nodiscard]] glm::vec3 cameraClearance(glm::vec3 eye, const glm::vec3& forward) const;
    // The game's camera ray (CODE 0x101814c0): the distance from `from` toward `to` to the first triangle that is not
    // a barrier, or Mog House furniture box (setCameraSolidBoxes), within their distance; client::GameCamera's ray.
    [[nodiscard]] std::optional<float> cameraRay(const glm::vec3& from, const glm::vec3& to) const;

    [[nodiscard]] bool hasCollision() const
    {
        return !walls_.tris.empty();
    }
    [[nodiscard]] const TriGrid& floors() const
    {
        return floors_;
    }
    [[nodiscard]] const TriGrid& walls() const
    {
        return walls_;
    }
    [[nodiscard]] std::optional<glm::vec3> doorBoxCentre(uint32_t door) const
    {
        for (const auto& b : doorBoxes_)
        {
            if (b.box.id == door)
            {
                return glm::vec3(b.box.pos[0], b.box.pos[1], b.box.pos[2]);
            }
        }
        return std::nullopt;
    }
    // A door's Rid collision box blocks move() while solid (closed; the default).
    void setDoorSolid(uint32_t door, bool solid)
    {
        for (auto& b : doorBoxes_)
        {
            if (b.box.id == door)
            {
                b.solid = solid;
            }
        }
    }

private:
    [[nodiscard]] glm::vec3 moveOnce(const glm::vec3& feet, const glm::vec3& delta) const; // move() without the cycle test
    [[nodiscard]] std::optional<std::size_t> floorTriangleAt(float x, float z, float maxY) const;

    TriGrid              floors_;
    std::vector<uint8_t> floorMapIds_;    // per floors_ triangle: collision map id (0xFF = render-mesh floor)
    std::vector<uint8_t> floorMaterials_; // per floors_ triangle: collision material (0 = render-mesh floor)
    std::vector<uint8_t> floorRoofed_;    // per floors_ triangle: under a roof
    TriGrid              walls_;
    std::vector<uint8_t> wallBarrier_; // per walls_ triangle: a barrier (blocks the player, not the camera)
    std::vector<uint8_t> wallUp_;      // per walls_ triangle: faces up and is not a barrier (a ground fallback in seams)
    struct DoorBox
    {
        dat::ZoneLine box; // game space
        bool          solid = true;
    };
    [[nodiscard]] glm::vec3 wallSlices(const glm::vec3& feet, glm::vec3 next, const glm::vec3& delta) const;
    // Pushes `c` (the sphere centre) sideways out of the band-clipped walls; false if it is still inside after kPushPasses.
    bool pushOut(glm::vec3& c, const glm::vec3& start, float y0, float y1, const glm::vec3& dir) const;
    [[nodiscard]] bool touches(const glm::vec3& c, const glm::vec3& start, float y0, float y1) const;

    std::vector<DoorBox> doorBoxes_;
    std::vector<dat::ZoneLine> solidBoxes_;
    std::vector<dat::ZoneLine> cameraSolidBoxes_; // the camera's (furniture at full height), game space
    bool                 cameraBoxes_ = true;
    // The nearest solid box the ray (render space, unit `dir`) enters within `maxDist`, if cameraBoxes_.
    [[nodiscard]] std::optional<float> boxHit(const glm::vec3& from, const glm::vec3& dir, float maxDist) const;
    float                topY_   = 0.0f;
    float                stepUp_ = kStepUp; // highest collision vertex: a rising ray past it hits nothing
    bool                 sphere_ = std::getenv("XI_MOVE_OLD") == nullptr;
};

} // namespace xi::gfx
