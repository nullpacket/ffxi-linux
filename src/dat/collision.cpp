// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/collision.h"

#include "xi/bytes.h"

#include <cmath>
#include <set>
#include <stdexcept>

namespace xi::dat
{

namespace
{

// Row-vector transform: [x y z 1] * M (D3D layout, M stored row-major as 16 floats).
void transformPoint(std::span<const std::byte> p, std::size_t m, const float in[3], float out[3])
{
    for (int c = 0; c < 3; ++c)
    {
        out[c] = in[0] * load<float>(p, m + 4 * (0 * 4 + static_cast<std::size_t>(c))) + in[1] * load<float>(p, m + 4 * (1 * 4 + static_cast<std::size_t>(c))) +
                 in[2] * load<float>(p, m + 4 * (2 * 4 + static_cast<std::size_t>(c))) + load<float>(p, m + 4 * (3 * 4 + static_cast<std::size_t>(c)));
    }
}

} // namespace

std::vector<CollisionTriangle> parseCollision(std::span<const std::byte> p)
{
    std::vector<CollisionTriangle> out;
    if (p.size() < 0x20 || load<uint8_t>(p, 3) < 0x1A)
    {
        return out; // no collision support for old Mzb versions (zone 0 uses 0x17; §7.8)
    }
    const auto coll = load<uint32_t>(p, 0x08);
    if (coll == 0 || load<uint8_t>(p, 0x1C) != 0)
    {
        return out;
    }
    const auto listCount = load<uint32_t>(p, coll + 0x08);
    const auto listOff   = load<uint32_t>(p, coll + 0x0C);

    // Walk every cell list once and collect the unique (placement, mesh) pairs; the grid only indexes them.
    std::set<std::pair<uint32_t, uint32_t>> pairs;
    std::size_t                             l = listOff;
    for (uint32_t i = 0; i < listCount; ++i)
    {
        const auto hdr = load<uint32_t>(p, l);
        const auto n   = hdr & 0x7FF;
        if (!(hdr & (1u << 13)))
        {
            for (uint32_t e = 0; e < n; ++e)
            {
                pairs.emplace(load<uint32_t>(p, l + 4 + 8 * e), load<uint32_t>(p, l + 8 + 8 * e));
            }
        }
        l += 4 + 8 * static_cast<std::size_t>(n) + 4;
    }

    for (const auto& [place, mesh] : pairs)
    {
        const auto vertOff = load<uint32_t>(p, mesh + 0x00);
        const auto normOff = load<uint32_t>(p, mesh + 0x04);
        const auto triOff  = load<uint32_t>(p, mesh + 0x08);
        const auto nTri    = load<int16_t>(p, mesh + 0x0C);
        const auto nv      = (normOff - vertOff) / 12;
        const auto nn      = (triOff - normOff) / 12;
        for (int16_t t = 0; t < nTri; ++t)
        {
            const std::size_t tp = triOff + 8 * static_cast<std::size_t>(t);
            const auto        w0 = load<uint16_t>(p, tp), w1 = load<uint16_t>(p, tp + 2), w2 = load<uint16_t>(p, tp + 4), w3 = load<uint16_t>(p, tp + 6);
            const uint32_t    idx[3] = { w0 & 0x7FFFu, w1 & 0x3FFFu, w2 & 0x3FFFu };
            const uint32_t    ni     = w3 & 0x7FFFu;
            if (idx[0] >= nv || idx[1] >= nv || idx[2] >= nv || ni >= nn)
            {
                throw std::runtime_error("collision: index out of range");
            }
            CollisionTriangle tri;
            {
                const auto f = load<uint32_t>(p, place + 0xA4);
                tri.mapId    = static_cast<uint8_t>(((f >> 26) & 3) << 3 | ((f >> 3) & 7));
                tri.roofed   = ((f >> 2) & 1) != 0;
            }
            for (int k = 0; k < 3; ++k)
            {
                const std::size_t vp = vertOff + 12 * idx[k];
                const float       local[3] = { load<float>(p, vp), load<float>(p, vp + 4), load<float>(p, vp + 8) };
                transformPoint(p, place, local, tri.v[k]);
            }
            // Normal through the row-vector normal matrix at +0x80 (may be zero on unreferenced placements; we
            // only visit referenced ones). Recompute from geometry as a fallback.
            const std::size_t np = normOff + 12 * ni;
            const float       n[3] = { load<float>(p, np), load<float>(p, np + 4), load<float>(p, np + 8) };
            float             len  = 0;
            for (int c = 0; c < 3; ++c)
            {
                tri.normal[c] = n[0] * load<float>(p, place + 0x80 + 4 * (0 * 3 + static_cast<std::size_t>(c))) +
                                n[1] * load<float>(p, place + 0x80 + 4 * (1 * 3 + static_cast<std::size_t>(c))) +
                                n[2] * load<float>(p, place + 0x80 + 4 * (2 * 3 + static_cast<std::size_t>(c)));
                len += tri.normal[c] * tri.normal[c];
            }
            if (len < 1e-6f)
            {
                // Geometric normal; stored faces are clockwise-front, so the outward normal is -(b-a)x(c-a).
                const float a[3] = { tri.v[1][0] - tri.v[0][0], tri.v[1][1] - tri.v[0][1], tri.v[1][2] - tri.v[0][2] };
                const float b[3] = { tri.v[2][0] - tri.v[0][0], tri.v[2][1] - tri.v[0][1], tri.v[2][2] - tri.v[0][2] };
                tri.normal[0]    = -(a[1] * b[2] - a[2] * b[1]);
                tri.normal[1]    = -(a[2] * b[0] - a[0] * b[2]);
                tri.normal[2]    = -(a[0] * b[1] - a[1] * b[0]);
                len              = tri.normal[0] * tri.normal[0] + tri.normal[1] * tri.normal[1] + tri.normal[2] * tri.normal[2];
            }
            len = std::sqrt(std::max(len, 1e-12f));
            for (auto& c : tri.normal)
            {
                c /= len;
            }
            tri.material = static_cast<uint8_t>((w0 >> 15) | ((w1 >> 15) << 1) | ((w2 >> 15) << 2) | ((w3 >> 15) << 3));
            tri.barrier  = (w2 & 0x4000) != 0;
            out.push_back(tri);
        }
    }
    return out;
}

} // namespace xi::dat
