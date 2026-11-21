// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace xi::dat
{

// Zone collision mesh from the Mzb collision block (docs/dat-format.md §7), flattened to world-space
// triangles in game coordinates (Y down).
struct CollisionTriangle
{
    float   v[3][3];   // world positions
    float   normal[3]; // world normal, pointing out of the solid (floors: (0,-1,0))
    uint8_t material = 0;
    bool    barrier  = false;
    uint8_t mapId    = 0; // from the placement flags (+0xA4): (f >> 26 & 3) << 3 | (f >> 3 & 7); picks the zone map (§14)
    bool    roofed   = false; // placement flags bit 2: under a roof (the indoor ambience; CODE 0x1001444a)
};

// Parses the collision block of a de-obfuscated Mzb payload. Empty if the Mzb has none (or an unsupported version).
std::vector<CollisionTriangle> parseCollision(std::span<const std::byte> mzbPayload);

} // namespace xi::dat
