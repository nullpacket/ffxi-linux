// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "client/session.h"
#include "dat/dat_index.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

// Ships and airships (S2C 0x00E look kind 4; RE notes ship_re.md): the id at +0x34 picks DAT 31004 + id (hull Mmb `fune`,
// schedulers `seq0`..`seq7`, Vtx paths). Animation 18..25 runs `seq(anim - 18)` from the packet's Time (earth seconds
// since the Vana'diel epoch): its op 0x26 / 0x27 legs drive the hull along the paths (CODE 0x100c4660, task 0x1005e8f0).
namespace xi::client
{

class Ships
{
public:
    struct Pose
    {
        glm::vec3 pos{ 0.0f }; // game space
        float     yaw = 0.0f;  // radians about the vertical (+X of the hull faces the travel)
    };

    // The ship id of an entity (kind 4 with a small id; LSB's cutscene-only props use animation 55), or nothing.
    static std::optional<uint32_t> shipId(const Entity& e);
    static uint32_t                fileFor(uint32_t ship)
    {
        return 31004 + ship;
    }
    // The pose of `e` now: on a path leg of its sequence, else where the server put it (or the last leg's end).
    Pose pose(const dat::DatIndex& index, const Entity& e);

private:
    struct Leg
    {
        float                  start = 0, duration = 0; // ticks (60 per second) from the sequence start
        std::vector<glm::vec3> path;
        uint32_t               flags = 0;
        int                    ease  = 0;
        float                  yawOffset = 0;
    };
    // Op 0xA9 (CMoActorRotationDriveTask, CODE 0x1005b2c2 -> 0x1005f950, tick 0x1005fa60): the hull's rotation to the
    // angles at +8 / +0xC / +0x10 (degrees, absolute), at once with mode +0x14 = 0, else over the record's duration.
    struct Turn
    {
        float start = 0, duration = 0; // ticks
        float yaw   = 0;               // radians (rotation y, as Pose::yaw)
        bool  timed = false;
    };
    struct Data
    {
        std::array<std::vector<Leg>, 8>  seqs;
        std::array<std::vector<Turn>, 8> turns;
    };
    const Data& data(const dat::DatIndex& index, uint32_t ship);

    std::map<uint32_t, Data> data_;
};

} // namespace xi::client
