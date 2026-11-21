// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/zone_collision.h"

#include <cmath>

namespace xi::gfx
{

void ZoneCollision::addCollision(const std::vector<dat::CollisionTriangle>& tris)
{
    // Game space (Y down) -> render space. Normals point out of the solid.
    for (const auto& t : tris)
    {
        const glm::vec3 n(t.normal[0], -t.normal[1], -t.normal[2]);
        auto&           grid = n.y > 0.35f ? floors_ : (std::abs(n.y) < 0.7f ? walls_ : floors_);
        if (&grid == &floors_ && n.y <= 0.35f)
        {
            continue; // ceilings / overhangs
        }
        for (const auto& v : t.v)
        {
            grid.tris.emplace_back(v[0], -v[1], -v[2]);
        }
        if (&grid == &floors_)
        {
            floorMapIds_.push_back(t.mapId);
            floorMaterials_.push_back(t.material);
            floorRoofed_.push_back(t.roofed ? 1 : 0);
        }
        else
        {
            wallBarrier_.push_back(t.barrier ? 1 : 0);
            wallUp_.push_back(n.y > 0.02f && !t.barrier ? 1 : 0);
        }
    }
}

void ZoneCollision::addFloor(const glm::vec3* v)
{
    for (int k = 0; k < 3; ++k)
    {
        floors_.tris.push_back(v[k]);
    }
    floorMapIds_.push_back(0xFF);
    floorMaterials_.push_back(0);
    floorRoofed_.push_back(0);
}

void ZoneCollision::build()
{
    floors_.build();
    walls_.build();
    topY_ = -1e30f;
    for (const auto* g : { &floors_, &walls_ })
    {
        for (const auto& v : g->tris)
        {
            topY_ = std::max(topY_, v.y);
        }
    }
}

void TriGrid::build()
{
    if (tris.empty())
    {
        return;
    }
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (const auto& v : tris)
    {
        lo = glm::min(lo, glm::vec2(v.x, v.z));
        hi = glm::max(hi, glm::vec2(v.x, v.z));
    }
    min = lo;
    w   = static_cast<int>((hi.x - lo.x) / kCell) + 1;
    h   = static_cast<int>((hi.y - lo.y) / kCell) + 1;
    cells.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), {});
    for (uint32_t t = 0; t < tris.size() / 3; ++t)
    {
        const auto& a  = tris[t * 3];
        const auto& b  = tris[t * 3 + 1];
        const auto& c  = tris[t * 3 + 2];
        const int   x0 = static_cast<int>((std::min({ a.x, b.x, c.x }) - min.x) / kCell);
        const int   x1 = static_cast<int>((std::max({ a.x, b.x, c.x }) - min.x) / kCell);
        const int   z0 = static_cast<int>((std::min({ a.z, b.z, c.z }) - min.y) / kCell);
        const int   z1 = static_cast<int>((std::max({ a.z, b.z, c.z }) - min.y) / kCell);
        for (int z = z0; z <= z1; ++z)
        {
            for (int x = x0; x <= x1; ++x)
            {
                cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)].push_back(t);
            }
        }
    }
}

bool ZoneCollision::rayBlocked(const glm::vec3& from, const glm::vec3& dir, float maxDist) const
{
    return rayHit(from, dir, maxDist).has_value();
}

std::optional<float> ZoneCollision::rayHit(const glm::vec3& from, const glm::vec3& dir, float maxDist, bool skipBarriers) const
{
    // March the ray through the XZ grids in half-cell steps; Möller-Trumbore against each triangle on the way. The
    // nearest hit wins; the march stops once it has passed the best hit so far.
    std::optional<float> best;
    auto test = [&](const glm::vec3* v) {
        // The camera's ray skips barrier walls (CODE 0x1016b0e0); the player is still stopped by them.
        if (skipBarriers && !walls_.tris.empty() && v >= walls_.tris.data() && v < walls_.tris.data() + walls_.tris.size() &&
            wallBarrier_[static_cast<std::size_t>(v - walls_.tris.data()) / 3])
        {
            return;
        }
        const glm::vec3 e1 = v[1] - v[0], e2 = v[2] - v[0];
        const glm::vec3 pv = glm::cross(dir, e2);
        const float     det = glm::dot(e1, pv);
        if (std::abs(det) < 1e-7f)
        {
            return;
        }
        const float     inv = 1.0f / det;
        const glm::vec3 tv  = from - v[0];
        const float     u   = glm::dot(tv, pv) * inv;
        if (u < 0.0f || u > 1.0f)
        {
            return;
        }
        const glm::vec3 qv = glm::cross(tv, e1);
        const float     w  = glm::dot(dir, qv) * inv;
        const float     t  = glm::dot(e2, qv) * inv;
        if (w >= 0.0f && u + w <= 1.0f && t > 0.05f && t < maxDist && (!best || t < *best))
        {
            best = t;
        }
    };
    const float step = TriGrid::kCell * 0.5f;
    for (float t = 0.0f; t < maxDist && (!best || t <= *best); t += step)
    {
        const glm::vec3 a = from + dir * t, b = from + dir * std::min(t + step, maxDist);
        if (dir.y > 0.0f && a.y > topY_)
        {
            break;
        }
        const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), z0 = std::min(a.z, b.z), z1 = std::max(a.z, b.z);
        floors_.visit(x0, z0, x1, z1, test);
        walls_.visit(x0, z0, x1, z1, test);
    }
    return best;
}

float ZoneCollision::cameraDistance(const glm::vec3& target, const glm::vec3& forward, float dist, float rayDrop) const
{
    // The game casts from 1 above the feet (CODE 0x1001ed90: the actor position less 1.0 in Y-down), not from the
    // view target: `rayDrop` is how far the target sits above that point. The eye ends 0.2 short of the hit along
    // the ray (CODE 0x1001f8aa); the same fraction of `dist` is kept along the view line.
    const glm::vec3 origin = target - glm::vec3(0.0f, rayDrop, 0.0f);
    const glm::vec3 full   = target - forward * dist;
    const glm::vec3 d      = full - origin;
    const float     len    = glm::length(d);
    if (len < 1e-4f)
    {
        return dist;
    }
    float out = dist;
    auto  hit = rayHit(origin, d / len, len + 0.2f, true); // barriers skipped
    if (const auto box = boxHit(origin, d / len, len + 0.2f); box && (!hit || *box < *hit))
    {
        hit = box; // Mog House furniture
    }
    if (hit)
    {
        out = std::max(dist * std::min((*hit - 0.2f) / len, 1.0f), 0.4f);
    }
    if (rayDrop <= 0.0f)
    {
        return out;
    }
    // The eye sits on the view line, not on the ray, so with the ray from below the target it can end up behind a
    // surface the ray missed (a ledge or roof edge between the two lines; xi-dat camcheck "cut off"). Pull it in
    // until the view target sees it.
    auto clear = [&](float t) {
        const glm::vec3 eye = target - forward * t;
        const glm::vec3 e   = eye - target;
        const float     l   = glm::length(e);
        return l < 0.06f || !rayHit(target, e / l, l + 0.2f, true).has_value();
    };
    // Down to 0.1 when even the 0.4 minimum is behind a wall at the character's back.
    for (int i = 0; i < 12 && out > 0.1f && !clear(out); ++i)
    {
        out = std::max(out * 0.7f, 0.1f);
    }
    return out;
}

std::optional<float> ZoneCollision::cameraRay(const glm::vec3& from, const glm::vec3& to) const
{
    const glm::vec3 d   = to - from;
    const float     len = glm::length(d);
    if (len < 1e-4f)
    {
        return std::nullopt;
    }
    auto hit = rayHit(from, d / len, len, true); // barriers skipped
    if (const auto box = boxHit(from, d / len, len); box && (!hit || *box < *hit))
    {
        hit = box; // Mog House furniture
    }
    return hit;
}

std::optional<float> ZoneCollision::boxHit(const glm::vec3& from, const glm::vec3& dir, float maxDist) const
{
    if (!cameraBoxes_)
    {
        return std::nullopt;
    }
    std::optional<float> best;
    for (const auto& b : cameraSolidBoxes_)
    {
        // Into the box's frame (game space: z and y flipped, turned by yaw as move()'s outOfBox does); slab test.
        const float c = std::cos(b.yaw), s = std::sin(b.yaw);
        const float px = from.x - b.pos[0], py = -from.y - b.pos[1], pz = -from.z - b.pos[2];
        const float o[3] = { px * c - pz * s, py, px * s + pz * c };
        const float v[3] = { dir.x * c + dir.z * s, -dir.y, dir.x * s - dir.z * c };
        float       t0 = 0.0f, t1 = maxDist;
        for (int k = 0; k < 3 && t0 <= t1; ++k)
        {
            const float h = b.size[k] * 0.5f;
            if (std::abs(v[k]) < 1e-6f)
            {
                if (std::abs(o[k]) > h)
                {
                    t0 = t1 + 1.0f;
                }
                continue;
            }
            float a = (-h - o[k]) / v[k], e = (h - o[k]) / v[k];
            if (a > e)
            {
                std::swap(a, e);
            }
            t0 = std::max(t0, a);
            t1 = std::min(t1, e);
        }
        if (t0 <= t1 && t0 > 0.0f && (!best || t0 < *best))
        {
            best = t0; // starting inside a box (t0 = 0) is no hit: the camera never pins to furniture it is in
        }
    }
    return best;
}

glm::vec3 ZoneCollision::cameraClearance(glm::vec3 eye, const glm::vec3& forward) const
{
    // The eye stays kCamClear above the ground under it, else below a ceiling over it (probes 10 straight down / up,
    // CODE 0x10181120 / 0x10181280), and kCamClear from walls to its left and right (a segment across the eye along
    // the view's right axis). Rays start 0.06 back: rayHit ignores hits nearer than 0.05.
    constexpr float k = kCamClear, o = 0.06f;
    if (const auto d = rayHit(eye + glm::vec3(0.0f, o, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f); d && *d - o < k)
    {
        eye.y += k - (*d - o);
    }
    else if (const auto u = rayHit(eye - glm::vec3(0.0f, o, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 10.0f, true); u && *u - o < k)
    {
        eye.y -= k - (*u - o);
    }
    glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));
    if (glm::dot(right, right) > 1e-6f)
    {
        right = glm::normalize(right);
        if (const auto r = rayHit(eye - right * o, right, k + o, true))
        {
            eye -= right * (k + o - *r);
        }
        else if (const auto l = rayHit(eye + right * o, -right, k + o, true))
        {
            eye += right * (k + o - *l);
        }
    }
    return eye;
}

std::optional<float> ZoneCollision::groundHeight(float x, float z, float maxY) const
{
    std::optional<float> best;
    floors_.visit(x, z, x, z, [&](const glm::vec3* v) {
        const auto& a = v[0];
        const auto& b = v[1];
        const auto& c = v[2];
        // Barycentric test in XZ, then interpolate Y.
        const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::abs(d) < 1e-9f)
        {
            return;
        }
        const float w0 = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
        const float w1 = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
        {
            return;
        }
        const float y = w0 * a.y + w1 * b.y + w2 * c.y;
        if (y <= maxY && (!best || y > *best))
        {
            best = y;
        }
    });
    return best;
}

bool ZoneCollision::floorAhead(const glm::vec3& p, glm::vec2 dir, float minY, float maxY) const
{
    // A sliver is crossed, not stood on: a floor 0.3 further along the move (a steep hillside with no floor beyond it
    // is still no ground).
    if (glm::length(dir) < 1e-6f)
    {
        return false;
    }
    dir = glm::normalize(dir) * 0.3f;
    const auto g = groundHeight(p.x + dir.x, p.z + dir.y, maxY);
    return g && *g >= minY;
}

std::optional<float> ZoneCollision::seamHeight(float x, float z, float minY, float maxY) const
{
    // Where no floor triangle covers (x, z): the highest upward-facing steep face there within [minY, maxY]. The sides
    // of Gusgen's rail sleepers leave ~0.05 slivers between floor triangles that refused short steps at high frame
    // rates; the game's ground probe is a vertical segment against every triangle (CODE 0x10169a50), so it never sees
    // the sliver.
    std::optional<float> best;
    walls_.visit(x, z, x, z, [&](const glm::vec3* v) {
        const auto t = static_cast<std::size_t>((v - walls_.tris.data()) / 3);
        if (t >= wallUp_.size() || !wallUp_[t])
        {
            return;
        }
        const auto& a = v[0];
        const auto& b = v[1];
        const auto& c = v[2];
        const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::abs(d) < 1e-9f)
        {
            return;
        }
        const float w0 = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
        const float w1 = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
        {
            return;
        }
        const float y = w0 * a.y + w1 * b.y + w2 * c.y;
        if (y >= minY && y <= maxY && (!best || y > *best))
        {
            best = y;
        }
    });
    return best;
}

std::optional<float> ZoneCollision::sliverHeight(float x, float z, float minY, float maxY) const
{
    const auto seam = seamHeight(x, z, minY, maxY);
    if (!seam)
    {
        return std::nullopt;
    }
    const glm::vec3 p(x, *seam, z);
    for (const glm::vec2 d : { glm::vec2(1, 0), glm::vec2(0, 1), glm::vec2(1, 1), glm::vec2(1, -1) })
    {
        if (floorAhead(p, d, *seam - 0.5f, *seam + 0.5f) && floorAhead(p, -d, *seam - 0.5f, *seam + 0.5f))
        {
            return seam;
        }
    }
    return std::nullopt;
}

uint8_t ZoneCollision::materialAt(float x, float z, float maxY) const
{
    const auto tri = floorTriangleAt(x, z, maxY);
    return tri && *tri < floorMaterials_.size() ? floorMaterials_[*tri] : 0;
}

bool ZoneCollision::roofedAt(float x, float z, float maxY) const
{
    const auto tri = floorTriangleAt(x, z, maxY);
    return tri && *tri < floorRoofed_.size() && floorRoofed_[*tri] != 0;
}

uint8_t ZoneCollision::mapIdAt(float x, float z, float maxY) const
{
    const auto tri = floorTriangleAt(x, z, maxY);
    return tri ? floorMapIds_[*tri] : 0;
}

std::optional<std::size_t> ZoneCollision::floorTriangleAt(float x, float z, float maxY) const
{
    // As groundHeight, keeping the winning collision triangle (render-mesh floors carry no map id or material).
    std::optional<float>       best;
    std::optional<std::size_t> id;
    floors_.visit(x, z, x, z, [&](const glm::vec3* v) {
        const auto  tri = static_cast<std::size_t>(v - floors_.tris.data()) / 3;
        const auto& a   = v[0];
        const auto& b   = v[1];
        const auto& c   = v[2];
        const float d   = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::abs(d) < 1e-9f || tri >= floorMapIds_.size() || floorMapIds_[tri] == 0xFF)
        {
            return;
        }
        const float w0 = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
        const float w1 = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
        {
            return;
        }
        const float y = w0 * a.y + w1 * b.y + w2 * c.y;
        if (y <= maxY && (!best || y > *best))
        {
            best = y;
            id   = tri;
        }
    });
    return id;
}

namespace
{

// Intersection of a triangle with the horizontal plane y = h, as a 2D segment in XZ.
bool sliceAt(const glm::vec3* v, float h, glm::vec2& p0, glm::vec2& p1)
{
    glm::vec2 pts[3];
    int       n = 0;
    for (int i = 0; i < 3 && n < 2; ++i)
    {
        const glm::vec3& a = v[i];
        const glm::vec3& b = v[(i + 1) % 3];
        if ((a.y - h) * (b.y - h) <= 0.0f && a.y != b.y)
        {
            const float t = (h - a.y) / (b.y - a.y);
            pts[n++]      = glm::vec2(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t);
        }
    }
    if (n < 2)
    {
        return false;
    }
    p0 = pts[0];
    p1 = pts[1];
    return true;
}

glm::vec2 closestOnSegment(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b)
{
    const glm::vec2 ab = b - a;
    const float     l2 = glm::dot(ab, ab);
    const float     t  = l2 > 1e-12f ? std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

} // namespace

bool ZoneCollision::crossesWall(const glm::vec3& from, const glm::vec3& to) const
{
    const float top = std::max(from.y, to.y) + stepUp_ + 0.05f; // walls lower than this are steps
    bool        hit = false;
    for (const float h : { stepUp_ + 0.1f, 0.8f, 1.0f })
    {
        if (h < stepUp_ + 0.1f)
        {
            continue;
        }
        const glm::vec3 a = from + glm::vec3(0.0f, h, 0.0f), b = to + glm::vec3(0.0f, h, 0.0f), d = b - a;
        walls_.visit(std::min(a.x, b.x), std::min(a.z, b.z), std::max(a.x, b.x), std::max(a.z, b.z), [&](const glm::vec3* v) {
            if (hit || std::max({ v[0].y, v[1].y, v[2].y }) <= top)
            {
                return;
            }
            const glm::vec3 e1 = v[1] - v[0], e2 = v[2] - v[0], pv = glm::cross(d, e2);
            const float     det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-9f)
            {
                return;
            }
            const glm::vec3 tv = a - v[0];
            const float     u  = glm::dot(tv, pv) / det;
            const glm::vec3 qv = glm::cross(tv, e1);
            const float     w = glm::dot(d, qv) / det, t = glm::dot(e2, qv) / det;
            hit               = u >= 0.0f && w >= 0.0f && u + w <= 1.0f && t > 0.0f && t < 1.0f;
        });
        if (hit)
        {
            return true;
        }
    }
    return false;
}

namespace
{

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
glm::vec3 closestOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float     d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
    {
        return a;
    }
    const glm::vec3 bp = p - b;
    const float     d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
    {
        return b;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        return a + ab * (d1 / (d1 - d3));
    }
    const glm::vec3 cp = p - c;
    const float     d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
    {
        return c;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        return a + ac * (d2 / (d2 - d6));
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// Closest point to p on the part of triangle v with y0 <= y <= y1 (the triangle clipped by both planes, as a fan).
std::optional<glm::vec3> closestInBand(const glm::vec3& p, const glm::vec3* v, float y0, float y1)
{
    if (std::max({ v[0].y, v[1].y, v[2].y }) < y0 || std::min({ v[0].y, v[1].y, v[2].y }) > y1)
    {
        return std::nullopt;
    }
    glm::vec3 poly[5] = { v[0], v[1], v[2] };
    int       n       = 3;
    auto      clip    = [&](float h, float sign) { // keeps sign * (y - h) >= 0
        glm::vec3 out[5];
        int       m = 0;
        for (int i = 0; i < n; ++i)
        {
            const glm::vec3& a  = poly[i];
            const glm::vec3& b  = poly[(i + 1) % n];
            const float      da = sign * (a.y - h), db = sign * (b.y - h);
            if (da >= 0.0f && m < 5)
            {
                out[m++] = a;
            }
            if ((da >= 0.0f) != (db >= 0.0f) && m < 5)
            {
                out[m++] = a + (b - a) * (da / (da - db));
            }
        }
        n = m;
        std::copy(out, out + m, poly);
    };
    clip(y0, 1.0f);
    if (n >= 3)
    {
        clip(y1, -1.0f);
    }
    if (n < 3)
    {
        return std::nullopt;
    }
    std::optional<glm::vec3> best;
    float                    bestD = 0.0f;
    for (int i = 1; i + 1 < n; ++i)
    {
        const glm::vec3 q = closestOnTriangle(p, poly[0], poly[i], poly[i + 1]);
        const float     d = glm::dot(q - p, q - p);
        if (!best || d < bestD)
        {
            best  = q;
            bestD = d;
        }
    }
    return best;
}

// The clearance a triangle asks of the sphere centre: kRadius, or less where the move started nearer than that (a wall
// that entered the band as the feet went down a step or along a slanted cave wall, or a spawn inside geometry): the move
// eases out by at most kEaseOut a step instead of being thrown out (through whatever is behind).
constexpr float kEaseOut = 0.05f;
// The game makes at most 3 push-out passes (CODE 0x1016eff0). The sideways-only push here settles more slowly in corners
// built of many small triangles: 3 passes refused 13,719 of the zone 234 walkcheck's steps (98,937 moved, the slices
// 107,540), 8 passes 8,117 (107,888 moved).
constexpr int kPushPasses = 8;

float clearance(const glm::vec3& start, const glm::vec3* v, float y0, float y1)
{
    const auto q = closestInBand(start, v, y0, y1);
    return q ? std::min(ZoneCollision::kRadius, glm::distance(*q, start) + kEaseOut) : ZoneCollision::kRadius;
}

} // namespace

bool ZoneCollision::touches(const glm::vec3& c, const glm::vec3& start, float y0, float y1) const
{
    bool hit = false;
    walls_.visit(c.x - kRadius, c.z - kRadius, c.x + kRadius, c.z + kRadius, [&](const glm::vec3* v) {
        if (hit)
        {
            return;
        }
        if (const auto q = closestInBand(c, v, y0, y1))
        {
            const float r = clearance(start, v, y0, y1) - 1e-3f;
            hit           = glm::dot(*q - c, *q - c) < r * r;
        }
    });
    return hit;
}

bool ZoneCollision::pushOut(glm::vec3& c, const glm::vec3& start, float y0, float y1, const glm::vec3& dir) const
{
    // CODE 0x1016f230 per contact: the centre moves out along the contact normal. Here only sideways (the floor snap
    // sets the height), to the horizontal distance at which the sphere just clears the contact point.
    for (int pass = 0; pass < kPushPasses; ++pass)
    {
        bool moved = false;
        walls_.visit(c.x - kRadius, c.z - kRadius, c.x + kRadius, c.z + kRadius, [&](const glm::vec3* v) {
            const auto q = closestInBand(c, v, y0, y1);
            if (!q)
            {
                return;
            }
            const float r = clearance(start, v, y0, y1);
            if (glm::dot(*q - c, *q - c) >= (r - 1e-3f) * (r - 1e-3f))
            {
                return;
            }
            const float dy   = c.y - q->y;
            const float need = std::sqrt(std::max(r * r - dy * dy, 0.0f)) + 1e-3f;
            glm::vec2   d(c.x - q->x, c.z - q->z);
            const float l = glm::length(d);
            if (l > 1e-5f)
            {
                d /= l;
            }
            else
            {
                // On the contact point: the triangle's normal sideways, facing back along the move.
                const glm::vec3 nrm = glm::cross(v[1] - v[0], v[2] - v[0]);
                d                   = glm::dot(glm::vec2(nrm.x, nrm.z), glm::vec2(nrm.x, nrm.z)) > 1e-10f ? glm::normalize(glm::vec2(nrm.x, nrm.z)) : -glm::vec2(dir.x, dir.z);
                if (glm::dot(d, glm::vec2(dir.x, dir.z)) > 0.0f)
                {
                    d = -d;
                }
            }
            c.x   = q->x + d.x * need;
            c.z   = q->z + d.y * need;
            moved = true;
        });
        if (!moved)
        {
            return true;
        }
    }
    return !touches(c, start, y0, y1);
}

glm::vec3 ZoneCollision::sweepSphere(const glm::vec3& feet, const glm::vec3& target) const
{
    // The band the sphere meets: from the step-up to the sphere's top over the feet; centre at the band's bottom or the
    // game's 0.5, whichever is higher, so walls stop it kRadius away as in the game.
    const float     y0  = feet.y + stepUp_, y1 = feet.y + 2.0f * kRadius;
    const glm::vec3 d   = glm::vec3(target.x - feet.x, 0.0f, target.z - feet.z);
    const float     len = glm::length(d);
    if (y0 >= y1 || len < 1e-6f)
    {
        return target;
    }
    const float     cy    = feet.y + std::max(kRadius, stepUp_);
    const glm::vec3 dir   = d / len;
    const glm::vec3 start = glm::vec3(feet.x, cy, feet.z);
    const int       n     = 1 + static_cast<int>(len / 4.0f); // CODE 0x1016e6f0: sub-steps of at most 4
    glm::vec3       pos   = start;
    auto            out   = [&](const glm::vec3& p) {
        // The path may not cross a wall: pushes around a corner can carry the centre through a thin one, and a wall the
        // move started inside (clearance 0) does not stop it. Then the move is refused.
        const glm::vec3 f(p.x, target.y, p.z);
        return crossesWall(feet, f) ? glm::vec3(feet.x, target.y, feet.z) : f;
    };
    for (int i = 1; i <= n; ++i)
    {
        const glm::vec3 end   = start + d * (static_cast<float>(i) / static_cast<float>(n));
        const glm::vec3 from  = pos;
        const int       marks = std::max(1, static_cast<int>(std::ceil(glm::distance(from, end) / (0.3f * kRadius)))); // CODE 0x1016eff0
        for (int j = 1; j <= marks; ++j)
        {
            glm::vec3 p = from + (end - from) * (static_cast<float>(j) / static_cast<float>(marks));
            if (touches(p, start, y0, y1))
            {
                // The first contact: pushed out there, and the move ends; still inside -> the last free position.
                return pushOut(p, start, y0, y1, dir) ? out(p) : glm::vec3(pos.x, target.y, pos.z);
            }
            pos = p;
        }
    }
    return out(pos);
}

glm::vec3 ZoneCollision::wallSlices(const glm::vec3& feet, glm::vec3 next, const glm::vec3& delta) const
{
    // Walls: slice nearby wall triangles at knee and chest height and push the character's circle out of the
    // resulting segments. Repeated passes resolve corners; the push is along the contact normal, so the
    // remaining motion slides along the wall.
    // The game moves a sphere of radius 0.5 centred 0.5 above the feet (CODE 0x1016e840): walls block between the
    // step it rides over (~0.5) and 1.0; higher ones (lintels, arches) do not. Sliced 0.15 apart over that band, so
    // thin rails between two slices are not missed.
    std::vector<float> heights{ feet.y + stepUp_ + 0.05f };
    for (const float h : { 0.7f, 0.85f, 1.0f })
    {
        if (h > stepUp_ + 0.05f)
        {
            heights.push_back(feet.y + h);
        }
    }
    for (int iter = 0; iter < 4; ++iter)
    {
        bool pushed = false;
        walls_.visit(next.x - kRadius, next.z - kRadius, next.x + kRadius, next.z + kRadius, [&](const glm::vec3* v) {
            for (const float hgt : heights)
            {
                glm::vec2 a, b;
                if (!sliceAt(v, hgt, a, b))
                {
                    continue;
                }
                const glm::vec2 p(next.x, next.z);
                const glm::vec2 c = closestOnSegment(p, a, b);
                const glm::vec2 d = p - c;
                const float     l = glm::length(d);
                if (l < kRadius)
                {
                    // Exactly on the segment: push back against the step direction.
                    const glm::vec2 dir = l > 1e-5f ? d / l : -glm::normalize(glm::vec2(delta.x, delta.z) + glm::vec2(1e-6f));
                    const glm::vec2 out = c + dir * (kRadius + 1e-3f);
                    next.x               = out.x;
                    next.z               = out.y;
                    pushed               = true;
                }
            }
        });
        if (!pushed)
        {
            break;
        }
    }
    return next;
}

glm::vec3 ZoneCollision::move(const glm::vec3& feet, const glm::vec3& delta) const
{
    const glm::vec3 next = moveOnce(feet, delta);
    // A two-spot cycle: pressing into a rock face, the step-up lifts the sphere into it and the ease-out pushes it back
    // down, so the character swaps between two spots 0.08 apart in height every frame (Gusgen game (6.5, -181.7)). A
    // step that goes against the input and changes height is tried up to three more times from where it lands: if that
    // comes back, the character stays put, as the game refuses a move it cannot resolve. Stairs and the ease-out
    // out of a wall are not cycles and still apply.
    // XI_MOVE_NOCYCLE=1 turns the test off (comparisons).
    static const bool noCycle = std::getenv("XI_MOVE_NOCYCLE") != nullptr;
    if (sphere_ && !noCycle && glm::dot(glm::vec2(next.x - feet.x, next.z - feet.z), glm::vec2(delta.x, delta.z)) < -1e-6f &&
        std::abs(next.y - feet.y) > 0.01f)
    {
        glm::vec3 p = next;
        for (int i = 0; i < 3; ++i) // two- and three-spot cycles (Gusgen game (23.1, -245.9) bobs 0.2 every 3 frames)
        {
            p = moveOnce(p, delta);
            if (glm::distance(p, feet) < 0.02f)
            {
                return feet;
            }
        }
    }
    return next;
}

glm::vec3 ZoneCollision::moveOnce(const glm::vec3& feet, const glm::vec3& delta) const
{
    glm::vec3 next = feet + glm::vec3(delta.x, 0.0f, delta.z);
    if (!walls_.cells.empty())
    {
        next = sphere_ ? sweepSphere(feet, next) : wallSlices(feet, next, delta);
    }
    // Closed doors: the circle out of each solid Rid box (§5.8), in the box's frame (yaw as ZoneLine::contains), along
    // the shallower axis. The box y is its centre (Selbina `_6w2`: 1.44 over the floor, size y 2.88).
    auto outOfBox = [&](const dat::ZoneLine& b) {
        const float dx = next.x - b.pos[0], dz = -next.z - b.pos[2], dy = -next.y - b.pos[1];
        if (std::abs(dy) > b.size[1] * 0.5f + 0.5f)
        {
            return;
        }
        const float c = std::cos(b.yaw), s = std::sin(b.yaw);
        float       lx = dx * c - dz * s, lz = dx * s + dz * c;
        const float hx = b.size[0] * 0.5f + kRadius, hz = b.size[2] * 0.5f + kRadius;
        if (std::abs(lx) >= hx || std::abs(lz) >= hz)
        {
            return;
        }
        if (hx - std::abs(lx) < hz - std::abs(lz))
        {
            lx = std::copysign(hx + 1e-3f, lx);
        }
        else
        {
            lz = std::copysign(hz + 1e-3f, lz);
        }
        next.x = b.pos[0] + c * lx + s * lz;
        next.z = -(b.pos[2] - s * lx + c * lz);
    };
    for (const auto& db : doorBoxes_)
    {
        if (db.solid)
        {
            outOfBox(db.box);
        }
    }
    for (const auto& b : solidBoxes_)
    {
        outOfBox(b);
    }

    // Swept guard: the step's path may not cross a wall above the step-up (squeezing through a corner, or a wall thinner
    // than the slices). Checked at three heights in the sphere's band.
    if (!sphere_ && !walls_.cells.empty() && crossesWall(feet, next))
    {
        return feet;
    }

    // Floor: the highest surface no more than kStepUp above the feet and at most kMaxDrop (40) below; none -> refuse the
    // step (CODE 0x10169a50: the ground probe from 0.5 above the feet, 40 down). Lower floors are fallen to by the caller
    // (gravity), not refused.
    // A sliver between floor triangles shows only a steep face; the game's probe hits that face, so the character stays on
    // the steps. That holds when a lower floor lies under the sliver too: Port Bastok's harbour stairs (game (74.5, -23.8))
    // have a 0.02 gap between treads over the sea floor 2.2 below, and the step fell into the water.
    // Over a lower floor it must be a sliver, with floor on both sides of it (sliverHeight): a ledge's lip has floor on
    // one side only, and the step falls.
    const auto h = groundHeight(next.x, next.z, feet.y + stepUp_);
    std::optional<float> seam;
    if (!h)
    {
        seam = seamHeight(next.x, next.z, feet.y - 0.5f, feet.y + stepUp_);
        seam = seam && floorAhead(next, glm::vec2(delta.x, delta.z), feet.y - 0.5f, feet.y + stepUp_) ? seam : std::nullopt;
    }
    else if (*h < feet.y - 0.5f)
    {
        seam = sliverHeight(next.x, next.z, feet.y - 0.5f, feet.y + stepUp_);
    }
    if (seam)
    {
        next.y = *seam;
    }
    else if (h && *h >= feet.y - kMaxDrop)
    {
        next.y = *h;
    }
    else if (!walls_.cells.empty())
    {
        return feet; // no floor under the new spot (off the mesh): refuse the step
    }
    return next;
}


} // namespace xi::gfx
