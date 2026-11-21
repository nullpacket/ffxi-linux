// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/cloth.h"

#include <algorithm>
#include <cmath>

namespace xi::gfx
{

namespace
{

// The particle where the skinned shader would place it (pre-weighted two-bone form).
glm::vec3 skinParticle(const dat::ClothMesh::Particle& q, const std::vector<glm::mat4>& pal)
{
    const glm::vec4 w = pal[q.bone0] * glm::vec4(q.p0[0], q.p0[1], q.p0[2], 1.0f - q.weight1) +
                        pal[q.bone1] * glm::vec4(q.p1[0], q.p1[1], q.p1[2], q.weight1);
    return glm::vec3(w);
}

// Particle matrix: translation = position, y axis = normal (cloth vertices carry n0 = +y and p0 = 0).
glm::mat4 particleMatrix(const glm::vec3& pos, glm::vec3 n)
{
    const float l = glm::length(n);
    n             = l > 1e-6f ? n / l : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 t = glm::normalize(std::abs(n.x) < 0.9f ? glm::cross(n, glm::vec3(1, 0, 0)) : glm::cross(n, glm::vec3(0, 0, 1)));
    const glm::vec3 b = glm::cross(t, n);
    return glm::mat4(glm::vec4(t, 0), glm::vec4(n, 0), glm::vec4(b, 0), glm::vec4(pos, 1));
}

// Area-weighted vertex normals from the triangles (CODE 0x10024830). Clockwise fronts: outward = e2 x e1.
void clothNormals(const dat::ClothMesh& c, const glm::vec3* pos, std::vector<glm::vec3>& n)
{
    n.assign(c.particles.size(), glm::vec3(0.0f));
    for (const auto& t : c.triangles)
    {
        const glm::vec3 e1 = pos[t[1]] - pos[t[0]], e2 = pos[t[2]] - pos[t[0]];
        const glm::vec3 f  = glm::cross(e2, e1);
        n[t[0]] += f;
        n[t[1]] += f;
        n[t[2]] += f;
    }
}

} // namespace

void restCloth(const ClothModel& m, std::vector<glm::mat4>& palette)
{
    if (m.base == 0 || palette.size() < m.base)
    {
        return;
    }
    palette.resize(m.base + m.particles);
    thread_local std::vector<glm::vec3> pos, nrm;
    for (const auto& c : m.meshes)
    {
        pos.resize(c.particles.size());
        for (std::size_t i = 0; i < c.particles.size(); ++i)
        {
            pos[i] = skinParticle(c.particles[i], palette);
        }
        clothNormals(c, pos.data(), nrm);
        for (std::size_t i = 0; i < c.particles.size(); ++i)
        {
            palette[m.base + c.first + i] = particleMatrix(pos[i], nrm[i]);
        }
    }
}

// A collision sphere's centre: its offset in the bone's posed frame (CODE 0x1002b2c0), render space via the palette.
static glm::vec3 sphereCentre(const dat::Skeleton::Sphere& sp, const std::vector<glm::mat4>& palette)
{
    return glm::vec3(palette[static_cast<std::size_t>(sp.bone)] * glm::vec4(sp.offset[0], sp.offset[1], sp.offset[2], 1.0f));
}

// The sphere at `c` against the spring segment a-b (CODE 0x10024340, INFERRED from its return codes): from the closest
// point of the segment, a contact nearer than the radius pushes out the end it lies at, or both ends when it lies
// between them (codes 1 / 2 / 3); pinned ends stay.
static void pushOut(glm::vec3& a, glm::vec3& b, bool pinA, bool pinB, const glm::vec3& c, float r)
{
    const glm::vec3 ab = b - a;
    const float     l2 = glm::dot(ab, ab);
    const float     t  = l2 > 1e-12f ? std::clamp(glm::dot(c - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    const glm::vec3 q  = a + ab * t;
    glm::vec3       d  = q - c;
    const float     l  = glm::length(d);
    if (l >= r)
    {
        return;
    }
    if (l < 1e-6f) // through the centre: out sideways from the segment
    {
        d = std::abs(ab.y) < 0.9f * std::sqrt(l2) ? glm::cross(ab, glm::vec3(0.0f, 1.0f, 0.0f)) : glm::cross(ab, glm::vec3(1.0f, 0.0f, 0.0f));
        if (glm::dot(d, d) < 1e-12f)
        {
            return;
        }
    }
    const glm::vec3 push = glm::normalize(d) * (r - l);
    if (t < 1.0f && !pinA)
    {
        a += push;
    }
    if (t > 0.0f && !pinB)
    {
        b += push;
    }
}

void stepClothWind(ClothWind& w, float dt)
{
    constexpr float kTick = 1.0f / 30.0f;
    auto            rnd   = [&]() { // xorshift: the game's rand() (0x10314520) is not reproduced, only its ranges
        w.rng ^= w.rng << 13, w.rng ^= w.rng >> 17, w.rng ^= w.rng << 5;
        return static_cast<int>(w.rng >> 1);
    };
    w.acc += std::clamp(dt, 0.0f, 0.25f);
    while (w.acc >= kTick)
    {
        w.acc -= kTick;
        w.timer -= 2.0f; // 1/60 s ticks per 30 fps frame
        if (w.timer <= 0.0f)
        {
            w.gust  = !w.gust;
            w.timer = static_cast<float>(rnd() % 60) * (w.gust ? 1.2f : 1.0f);
        }
        w.strength = w.gust ? static_cast<float>(rnd() % 10 + 4) * 0.0025f : std::max(w.strength - 0.01f, 0.0f);
    }
}

void simulateCloth(const ClothModel& m, ClothSim& sim, std::vector<glm::mat4>& palette, const glm::vec3& rootRender, float dt, float wind)
{
    if (m.base == 0 || palette.size() < m.base)
    {
        return;
    }
    palette.resize(m.base + m.particles);
    const std::size_t n = m.particles;
    // Gravity is +y in game space (y down); render space flips y (ZoneScene::gameToRenderMatrix).
    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    // A first frame, a new model or a jump (zoning, a warp): every particle starts skinned at rest, still (the game's
    // reset branch, CODE 0x100231c0 when the draw context's flag +0x3c is set).
    const bool reset = !sim.init || sim.p.size() != n || glm::distance(sim.root, rootRender) > 8.0f;
    if (reset)
    {
        sim.p.resize(n);
        for (const auto& c : m.meshes)
        {
            for (std::size_t i = 0; i < c.particles.size(); ++i)
            {
                sim.p[c.first + i] = skinParticle(c.particles[i], palette);
            }
        }
        sim.p0   = sim.p;
        sim.prev = sim.p;
        sim.n.assign(n, glm::vec3(0.0f, 1.0f, 0.0f));
        // Collision ties (CODE 0x10023b00, first step): each spring meets the sphere its end a starts within 1.5 r^2 of
        // (the last such of the mesh's spheres), and every sphere's sweep starts where it is now.
        sim.tie.assign(m.meshes.size(), {});
        sim.centres.assign(m.meshes.size(), {});
        for (std::size_t mi = 0; mi < m.meshes.size(); ++mi)
        {
            const auto& c = m.meshes[mi];
            sim.tie[mi].assign(c.springs.size(), -1);
            for (const uint16_t id : c.spheres)
            {
                sim.centres[mi].push_back(id < m.spheres.size() ? sphereCentre(m.spheres[id], palette) : glm::vec3(0.0f));
                if (id >= m.spheres.size())
                {
                    continue;
                }
                const float r2 = m.spheres[id].radius * m.spheres[id].radius * 1.5f;
                for (std::size_t si = 0; si < c.springs.size(); ++si)
                {
                    const glm::vec3 d = sim.p[c.first + c.springs[si].a] - sim.centres[mi].back();
                    if (glm::dot(d, d) < r2)
                    {
                        sim.tie[mi][si] = id;
                    }
                }
            }
        }
        sim.root = rootRender;
        sim.acc  = 0.0f;
        sim.init = true;
    }
    constexpr float kTick = 1.0f / 30.0f;
    sim.acc += std::clamp(dt, 0.0f, 0.25f);
    int steps = 0;
    while (sim.acc >= kTick && steps < 4)
    {
        sim.acc -= kTick;
        ++steps;
        sim.prev = sim.p;
        // The actor's own motion this tick (CODE 0x100231c0, the vector at the draw context +0x34; INFERRED: the
        // actor's displacement): free particles are carried 0.75 of it less the inertia, their previous position 0.75
        // of it, so they mostly follow the body and lag by `inertia`.
        const glm::vec3 moved = rootRender - sim.root;
        sim.root              = rootRender;
        for (const auto& c : m.meshes)
        {
            for (std::size_t i = 0; i < c.particles.size(); ++i)
            {
                const std::size_t k = c.first + i;
                if (c.particles[i].anchor)
                {
                    sim.p[k] = sim.p0[k] = skinParticle(c.particles[i], palette);
                    continue;
                }
                sim.p[k] += (0.75f - c.inertia) * moved;
                sim.p0[k] += 0.75f * moved;
                // Verlet with damping, then gravity +g x 0.1 down (CODE 0x10023630).
                const glm::vec3 v = (sim.p[k] - sim.p0[k]) * c.damping;
                sim.p0[k]         = sim.p[k];
                sim.p[k] += v + down * (c.gravity * 0.1f);
                // Wind (CODE 0x100236c7): along the particle's normal plus the wind direction (the direction less the
                // normal when the sum is shorter than 0.1), times the strength x 0.1. Game (-1, 0, 0) is render (-1, 0, 0).
                if (wind > 0.0f)
                {
                    const glm::vec3 dir(-1.0f, 0.0f, 0.0f);
                    glm::vec3       w = sim.n[k] + dir;
                    if (glm::dot(w, w) < 0.01f)
                    {
                        w = dir - sim.n[k];
                    }
                    sim.p[k] += w * (wind * 0.1f);
                }
            }
            // Springs, four passes: set 0 then set 1 with their stiffness; a pinned end does not move.
            for (int pass = 0; pass < 4; ++pass)
            {
                for (const auto& s : c.springs)
                {
                    glm::vec3&  a = sim.p[c.first + s.a];
                    glm::vec3&  b = sim.p[c.first + s.b];
                    glm::vec3   d = b - a;
                    const float l = glm::length(d);
                    if (l > 1e-4f)
                    {
                        d *= (s.rest - l) * c.stiffness[s.set] / l;
                        if (!s.pinA)
                        {
                            a -= d;
                        }
                        if (!s.pinB)
                        {
                            b += d;
                        }
                    }
                }
            }
            // Body collision (CODE 0x10023b00 -> 0x10023f50): each of the mesh's spheres moves from its centre at the last
            // tick to now in radius-sized steps; at each the springs tied to it are pushed out of it.
            const std::size_t mi = static_cast<std::size_t>(&c - m.meshes.data());
            for (std::size_t j = 0; j < c.spheres.size() && j < sim.centres[mi].size(); ++j)
            {
                const uint16_t id = c.spheres[j];
                if (id >= m.spheres.size())
                {
                    continue;
                }
                const float     r    = m.spheres[id].radius;
                const glm::vec3 to   = sphereCentre(m.spheres[id], palette);
                glm::vec3       at   = sim.centres[mi][j];
                const glm::vec3 path = to - at;
                float           left = glm::length(path);
                const glm::vec3 dir  = left > 1e-6f ? path / left : glm::vec3(0.0f);
                for (int guard = 0; guard < 64; ++guard)
                {
                    for (std::size_t si = 0; si < c.springs.size(); ++si)
                    {
                        if (sim.tie[mi][si] == id)
                        {
                            const auto& sp = c.springs[si];
                            pushOut(sim.p[c.first + sp.a], sim.p[c.first + sp.b], sp.pinA, sp.pinB, at, r);
                        }
                    }
                    left -= r;
                    if (left <= 1e-6f)
                    {
                        if (left > -r) // the last step ends at the sphere's position now
                        {
                            at = to;
                            continue;
                        }
                        break;
                    }
                    at += dir * r;
                }
                sim.centres[mi][j] = to;
            }
        }
    }
    if (steps == 4)
    {
        sim.acc = 0.0f; // fell behind (a stall): drop the backlog
    }
    // Draw between the last two ticks; anchors exactly where the skeleton is this frame.
    const float alpha = std::clamp(sim.acc / kTick, 0.0f, 1.0f);
    thread_local std::vector<glm::vec3> pos, nrm;
    for (const auto& c : m.meshes)
    {
        pos.resize(c.particles.size());
        for (std::size_t i = 0; i < c.particles.size(); ++i)
        {
            const std::size_t k = c.first + i;
            pos[i] = c.particles[i].anchor ? skinParticle(c.particles[i], palette) : glm::mix(sim.prev[k], sim.p[k], alpha);
        }
        clothNormals(c, pos.data(), nrm);
        for (std::size_t i = 0; i < c.particles.size(); ++i)
        {
            palette[m.base + c.first + i] = particleMatrix(pos[i], nrm[i]);
            sim.n[c.first + i]            = nrm[i];
        }
    }
}

} // namespace xi::gfx
