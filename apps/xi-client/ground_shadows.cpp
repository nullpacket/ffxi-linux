// SPDX-License-Identifier: GPL-3.0-or-later
#include "ground_shadows.h"

#include "dat/chunk.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace xi::app
{
namespace
{

constexpr float kNear     = 20.0f;         // camera distance below which the shadow is a ground decal
constexpr float kStrength = 0x60 / 128.0f; // vertex colour 0x60 under MODULATE2X (INFERRED stage op)
// The traced size (5/9 of the light-space box, / 3) looks far smaller than the game's blobs here: about shoulder width
// needs twice that (tuned by eye; the light-space box is approximated, INFERRED).
constexpr float kSizeTune = 2.0f;

} // namespace

GroundShadows::GroundShadows(const dat::DatIndex& index)
{
    if (std::getenv("XI_NO_SHADOWS"))
    {
        return;
    }
    try
    {
        std::vector<dat::Chunk> chunks;
        const auto              data = index.read(7052); // the chunks point into it
        dat::parseChunks(data, chunks);
        for (const auto& c : chunks)
        {
            if (c.type == 0x20)
            {
                auto t = dat::parseTexture(c.payload);
                if (t.name.find("kage") != std::string::npos)
                {
                    texture_ = std::move(t);
                    break;
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        spdlog::warn("shadow texture (file 7052): {}", e.what());
    }
}

float GroundShadows::halfSize(const gfx::CharacterModel& model)
{
    // CODE 0x100cc2f0: the bounding box seen from the zone light, its larger side * 5/9, then / 3. Here: the bind
    // pose's footprint, or 0.7 of the height for a light at about 45 degrees (INFERRED), whichever is larger.
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (const auto& b : model.bindWorld)
    {
        const glm::vec2 p(b[3].x, b[3].z);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const float foot = model.bindWorld.empty() ? 0.0f : std::max(hi.x - lo.x, hi.y - lo.y);
    const float size = std::max(foot, model.height * 0.7f) * 5.0f / 9.0f;
    return std::max(size, 0.01f) / 3.0f * kSizeTune;
}

void GroundShadows::cast(const glm::vec3& feet, const gfx::CharacterModel& model, float alpha, std::size_t instance, bool player)
{
    if (texture_ && enabled_ && alpha > 0.0f)
    {
        casts_.push_back({ feet, halfSize(model), alpha, instance, player, model.height });
    }
}

glm::mat4 GroundShadows::silhouetteMatrix(const glm::vec3& feet, const glm::vec3& towardLight, float h)
{
    // The light's travel, kept at least ~45 degrees from the ground so the flattened model stays in its cell
    // (INFERRED: the game fits a light-space box instead, 0x100cc2f0).
    glm::vec3 l = -towardLight;
    if (!(l.y < -0.7f))
    {
        glm::vec2 d(l.x, l.z);
        d = glm::dot(d, d) > 1e-8f ? glm::normalize(d) : glm::vec2(0.0f);
        l = glm::vec3(d.x * 0.714f, -0.7f, d.y * 0.714f);
    }
    // x' = x - lx / ly (y - fy), z' = z - lz / ly (y - fy); clip x = (x' - fx) / h, clip y = (z' - fz) / h, z = 0.5.
    const float ax = l.x / l.y, az = l.z / l.y;
    glm::mat4   m(0.0f);
    m[0][0] = 1.0f / h;
    m[1][0] = -ax / h;
    m[3][0] = (ax * feet.y - feet.x) / h;
    m[1][1] = -az / h;
    m[2][1] = 1.0f / h;
    m[3][1] = (az * feet.y - feet.z) / h;
    m[3][2] = 0.5f;
    m[3][3] = 1.0f;
    return m;
}

void GroundShadows::build(const gfx::ZoneScene* scene, const glm::vec3& camera, std::vector<client::EffectSystem::Batch>& out)
{
    jobs_.clear();
    if (!texture_ || casts_.empty())
    {
        return;
    }
    client::EffectSystem::Batch b, sil;
    b.texture   = &*texture_;
    b.blend     = client::EffectSystem::Batch::Blend::Multiply;
    sil.texture = &silhouetteKey_;
    sil.blend   = client::EffectSystem::Batch::Blend::Multiply;
    constexpr uint32_t kPerRow = kAtlas / kCell;
    std::vector<glm::vec3> tris;
    for (const auto& c : casts_)
    {
        if (c.feet.y - camera.y > 2.0f) // more than 2 above the camera (CODE 0x100cbe9b..)
        {
            continue;
        }
        const float k    = kStrength * c.alpha;
        const float dist = glm::distance(c.feet, camera);
        // Mode 1 (silhouette): config "detailed" within 40, or the local player (shadow_re.md §2a); while cells last.
        const bool detailed = c.instance != SIZE_MAX && ((mode_ == 2 && dist < 40.0f) || c.player) && jobs_.size() < kPerRow * kPerRow;
        auto&      batch    = detailed ? sil : b;
        // The silhouette's square: the game scales its projection by size * 2.4; here also wide enough for the whole
        // flattened model (the light is at least ~45 degrees up), INFERRED.
        const float s = detailed ? std::max(c.s * 2.4f, c.height * 1.1f) : c.s;
        glm::vec2   uv0(0.0f), uvSize(1.0f);
        if (detailed)
        {
            const auto     n = static_cast<uint32_t>(jobs_.size());
            const VkRect2D cell{ { static_cast<int32_t>((n % kPerRow) * kCell), static_cast<int32_t>((n / kPerRow) * kCell) }, { kCell, kCell } };
            jobs_.push_back({ c.instance, silhouetteMatrix(c.feet, light_, s), cell });
            // Half a texel in from the cell's edges, so linear filtering never reads the neighbour.
            uv0    = (glm::vec2(cell.offset.x, cell.offset.y) + 0.5f) / static_cast<float>(kAtlas);
            uvSize = glm::vec2(static_cast<float>(kCell) - 1.0f) / static_cast<float>(kAtlas);
        }
        // UVs stay unclamped: a ground triangle reaching past the square samples `kage`'s empty border (the sampler
        // clamps). A silhouette's cell has neighbours, so its triangles are first cut to the square (clipSquare).
        auto put = [&](const glm::vec3& r, float kk) {
            const glm::vec2 uv((r.x - c.feet.x) / (2.0f * s) + 0.5f, (r.z - c.feet.z) / (2.0f * s) + 0.5f);
            batch.vertices.push_back({ gfx::ZoneScene::renderToGame(r), uv0 + uv * uvSize, glm::vec4(glm::vec3(kk), 1.0f) });
        };
        // Sutherland-Hodgman in x / z against feet +- s; y interpolated.
        auto clipSquare = [&](std::vector<glm::vec3> poly) {
            for (int edge = 0; edge < 4 && !poly.empty(); ++edge)
            {
                const int   axis = edge < 2 ? 0 : 2;
                const float sign = (edge % 2) ? -1.0f : 1.0f;
                const float lim  = (axis == 0 ? c.feet.x : c.feet.z) + sign * s; // keep v * sign <= lim * sign
                auto inside = [&](const glm::vec3& v) { return v[axis] * sign <= lim * sign; };
                std::vector<glm::vec3> clipped;
                for (std::size_t i = 0; i < poly.size(); ++i)
                {
                    const glm::vec3& a  = poly[i];
                    const glm::vec3& b2 = poly[(i + 1) % poly.size()];
                    if (inside(a))
                    {
                        clipped.push_back(a);
                    }
                    if (inside(a) != inside(b2))
                    {
                        const float t = (lim - a[axis]) / (b2[axis] - a[axis]);
                        clipped.push_back(a + (b2 - a) * t);
                    }
                }
                poly = std::move(clipped);
            }
            return poly;
        };
        if (scene && dist < kNear)
        {
            tris.clear();
            scene->groundTriangles(c.feet.x - s, c.feet.z - s, c.feet.x + s, c.feet.z + s, tris);
            // Per vertex: 0x60 - 48 * |dy|, so it is gone 2 units above or below the feet (CODE 0x10184ef0).
            auto weight = [&](const glm::vec3& v) { return std::max(0.0f, 1.0f - std::abs(v.y - c.feet.y) * 0.5f); };
            for (std::size_t i = 0; detailed && i + 2 < tris.size(); i += 3)
            {
                const auto poly = clipSquare({ tris[i], tris[i + 1], tris[i + 2] });
                for (std::size_t j = 1; j + 1 < poly.size(); ++j)
                {
                    if (weight(poly[0]) + weight(poly[j]) + weight(poly[j + 1]) > 0.0f)
                    {
                        put(poly[0], k * weight(poly[0]));
                        put(poly[j], k * weight(poly[j]));
                        put(poly[j + 1], k * weight(poly[j + 1]));
                    }
                }
            }
            for (std::size_t i = 0; !detailed && i + 2 < tris.size(); i += 3)
            {
                float w[3];
                for (int j = 0; j < 3; ++j)
                {
                    w[j] = weight(tris[i + static_cast<std::size_t>(j)]);
                }
                if (w[0] + w[1] + w[2] <= 0.0f)
                {
                    continue;
                }
                for (int j = 0; j < 3; ++j)
                {
                    put(tris[i + static_cast<std::size_t>(j)], k * w[j]);
                }
            }
            continue;
        }
        // The far blob: a flat quad 0.001 above the feet (the strip at 0x10377360).
        const glm::vec3 p0 = c.feet + glm::vec3(-s, 0.001f, -s), p1 = c.feet + glm::vec3(s, 0.001f, -s);
        const glm::vec3 p2 = c.feet + glm::vec3(-s, 0.001f, s), p3 = c.feet + glm::vec3(s, 0.001f, s);
        for (const auto& p : { p0, p1, p2, p1, p3, p2 })
        {
            put(p, k);
        }
    }
    for (auto* x : { &b, &sil })
    {
        if (!x->vertices.empty())
        {
            out.push_back(std::move(*x));
        }
    }
}

} // namespace xi::app
