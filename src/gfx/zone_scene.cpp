// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/zone_scene.h"

#include <unordered_set>

#include "dat/chunk.h"
#include "dat/texture.h"

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace xi::gfx
{

const dat::Chunk* propModelChunk(const std::vector<dat::Chunk>& chunks)
{
    const dat::Chunk* pick = nullptr;
    for (const auto& c : chunks)
    {
        if (c.type == 46 && (!pick || (c.name == "fune" && pick->name != "fune") || (c.name == "_fun" && pick->name != "fune" && pick->name != "_fun")))
        {
            pick = &c;
        }
    }
    return pick;
}

std::optional<uint32_t> addModelMesh(MeshRenderer& meshes, const std::string& name, const dat::Model& mdl,
                                     const std::unordered_map<std::string, uint32_t>& texByName, uint32_t whiteTex)
{
    std::vector<Vertex>   verts;
    std::vector<uint32_t> idx;
    std::vector<SubMesh>  subs;
    for (const auto& pc : mdl.pieces)
    {
        if (pc.triangles.empty())
        {
            continue;
        }
        const auto base = static_cast<uint32_t>(verts.size());
        for (const auto& v : pc.vertices)
        {
            // D3DCOLOR bytes are B,G,R,A; the vertex attribute wants R,G,B,A.
            const uint32_t c = v.color;
            verts.push_back({ { v.pos[0], v.pos[1], v.pos[2] }, { v.normal[0], v.normal[1], v.normal[2] },
                              ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000u), { v.uv[0], v.uv[1] } });
        }
        SubMesh sm;
        sm.firstIndex = static_cast<uint32_t>(idx.size());
        for (const auto t : pc.triangles)
        {
            idx.push_back(base + t);
        }
        sm.indexCount  = static_cast<uint32_t>(pc.triangles.size());
        const auto it  = texByName.find(pc.texture);
        sm.texture     = it != texByName.end() ? it->second : whiteTex;
        // Client render states (docs/dat-format.md §6).
        sm.blended     = (pc.flags & 0x8) != 0;
        sm.doubleSided = (pc.flags & 0x2) != 0;
        sm.alphaTest   = !name.empty() && name[0] == '_';
        subs.push_back(sm);
    }
    if (subs.empty())
    {
        return std::nullopt;
    }
    return meshes.addMesh(verts, idx, std::move(subs));
}

ZoneScene::ZoneScene(Context& ctx, const dat::DatIndex& index, const dat::ZoneKeys& keys, uint16_t zone, uint16_t model,
                     const std::vector<dat::ZoneProp>& props, uint64_t propsKey)
: zone_(zone)
, model_(model)
, propsKey_(propsKey)
{
    const auto t0   = std::chrono::steady_clock::now();
    auto       data = dat::loadZone(index, model, keys);
    // Props (furniture): each file's Mmb (`fune`, else `_fun`, else the first; CODE 0x100c4340) as one more placed
    // model, its textures beside the zone's.
    std::vector<std::vector<std::byte>> propFiles;
    std::vector<int>                    placementProp; // placement index - first prop placement -> prop index
    const std::size_t                   firstProp = data.placements.size();
    propInstances_.assign(props.size(), std::nullopt);
    for (std::size_t pi = 0; pi < props.size(); ++pi)
    {
        const auto& pr = props[pi];
        if (!index.path(pr.fileId))
        {
            continue;
        }
        propFiles.push_back(index.read(pr.fileId));
        std::vector<dat::Chunk> chunks;
        dat::parseChunks(propFiles.back(), chunks);
        const dat::Chunk* pick = propModelChunk(chunks);
        if (!pick)
        {
            continue;
        }
        std::vector<std::byte> payload(pick->payload.begin(), pick->payload.end());
        dat::decodeMmb(payload, keys);
        auto m        = dat::parseMmb(payload);
        const auto nm = "prop:" + std::to_string(pr.fileId);
        m.name        = nm;
        data.models.try_emplace(nm, std::move(m));
        dat::Placement pl;
        pl.name = nm;
        for (int a = 0; a < 3; ++a)
        {
            pl.pos[a]   = pr.pos[a];
            pl.rot[a]   = 0.0f;
            pl.scale[a] = 1.0f;
        }
        pl.rot[1] = pr.yaw;
        data.placements.push_back(pl);
        placementProp.push_back(static_cast<int>(pi));
    }
    zoneLines_      = data.zoneLines;
    for (const auto& b : data.doorBoxes)
    {
        collision_.addDoorBox(b);
    }
    for (const auto& l : data.lifts)
    {
        lifts_.push_back({ l, std::nullopt });
    }
    lightKeys_      = data.lightKeys;
    {
        std::unordered_set<std::string> placed;
        for (const auto& pl : data.placements)
        {
            placed.insert(pl.name);
        }
        for (const auto& [chunk, modelName] : data.modelByChunk)
        {
            if (const auto it = data.models.find(modelName); it != data.models.end() && !placed.contains(modelName))
            {
                effectMeshes_.emplace(chunk, dat::meshFromModel(it->second));
            }
        }
    }
    weather_        = dat::ZoneWeather::fromZoneFile(data.file);

    textures_ = std::make_unique<TextureSet>(ctx);
    std::unordered_map<std::string, uint32_t> texByName;
    {
        std::vector<dat::Chunk> chunks;
        dat::parseChunks(data.file, chunks);
        for (const auto& f : propFiles)
        {
            std::vector<dat::Chunk> more; // parseChunks clears its output
            dat::parseChunks(f, more);
            chunks.insert(chunks.end(), more.begin(), more.end());
        }
        for (const auto& c : chunks)
        {
            if (c.type == 0x20 || c.type == 0x1B)
            {
                try
                {
                    const auto tex = dat::parseTexture(c.payload);
                    texByName.try_emplace(tex.name, textures_->add(tex));
                }
                catch (const std::exception& e)
                {
                    spdlog::warn("zone {}: texture {}: {}", zone, c.name, e.what());
                }
            }
        }
    }
    dat::Texture white;
    white.width  = 4;
    white.height = 4;
    white.data.assign(4 * 4 * 4, std::byte{ 0x80 }); // FFXI scale: 0x80 = full intensity / opaque
    const uint32_t whiteTex = textures_->add(white);
    textures_->commit();

    meshes_ = std::make_unique<MeshRenderer>(ctx, *textures_);
    struct LocalMesh
    {
        uint32_t                    id;
        const dat::Model*           model;
    };
    std::unordered_map<std::string, LocalMesh> meshByName;
    for (const auto& [name, mdl] : data.models)
    {
        if (const auto id = addModelMesh(*meshes_, name, mdl, texByName, whiteTex))
        {
            meshByName[name] = { *id, &mdl };
        }
    }

    // Instances, and the ground triangles in render space.
    const glm::mat4 toRender = gameToRenderMatrix();
    static const bool noLod = std::getenv("XI_NO_LOD") != nullptr; // test hook: every placement draws the mesh it names
    for (std::size_t pli = 0; pli < data.placements.size(); ++pli)
    {
        const auto& pl = data.placements[pli];
        // LOD meshes (CODE 0x1017aef0): all three start as the named mesh; <stem>m replaces all three, <stem>h the near
        // one, <stem>l the far one. Doors and props keep the mesh they name (their parts move as one).
        std::string bandName[3] = { pl.name, pl.name, pl.name };
        const bool  isProp      = pli >= firstProp;
        const auto  doorChar    = static_cast<char>(pl.doorId & 0xFF);
        if (!noLod && !isProp && doorChar != '_' && doorChar != '@')
        {
            if (const auto mName = dat::lodVariant(pl.name, 'm'); mName && data.models.contains(*mName))
            {
                bandName[0] = bandName[1] = bandName[2] = *mName;
            }
            if (const auto hName = dat::lodVariant(pl.name, 'h'); hName && data.models.contains(*hName))
            {
                bandName[0] = *hName;
            }
            if (const auto lName = dat::lodVariant(pl.name, 'l'); lName && data.models.contains(*lName))
            {
                bandName[2] = *lName;
            }
        }
        const auto it = meshByName.find(bandName[0]);
        if (it == meshByName.end() && bandName[0] == bandName[2])
        {
            continue;
        }
        // Client: W = S * Rx * Ry * Rz * T (row vectors) == T * Rz * Ry * Rx * S (column vectors).
        glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(pl.pos[0], pl.pos[1], pl.pos[2]));
        m           = glm::rotate(m, pl.rot[2], glm::vec3(0, 0, 1));
        m           = glm::rotate(m, pl.rot[1], glm::vec3(0, 1, 0));
        m           = glm::rotate(m, pl.rot[0], glm::vec3(1, 0, 0));
        m           = glm::scale(m, glm::vec3(pl.scale[0], pl.scale[1], pl.scale[2]));
        const glm::mat4 world = toRender * m;
        const uint32_t lights = pl.lights[0] | pl.lights[1] << 8 | pl.lights[2] << 16 | static_cast<uint32_t>(pl.lights[3]) << 24;
        const bool     mirrored = glm::determinant(glm::mat3(m)) < 0.0f;
        uint32_t       inst     = UINT32_MAX;
        if (bandName[0] == bandName[1] && bandName[1] == bandName[2])
        {
            inst = meshes_->addInstance(it->second.id, world, mirrored, lights);
        }
        else
        {
            // One instance per distinct mesh, each drawn in its bands (d1 / max(d1, d2) from the record, §6.3).
            const float d1 = pl.lodDist[0], d12 = std::max(pl.lodDist[0], pl.lodDist[1]);
            const auto  group = meshes_->addLodGroup(glm::vec3(world[3]), d1 * d1, d12 * d12);
            for (int b = 0; b < 3; ++b)
            {
                if (b > 0 && (bandName[b] == bandName[0] || (b == 2 && bandName[2] == bandName[1])))
                {
                    continue; // already added with the earlier band
                }
                uint8_t bands = 0;
                for (int k = 0; k < 3; ++k)
                {
                    bands |= bandName[k] == bandName[b] ? static_cast<uint8_t>(1u << k) : uint8_t{ 0 };
                }
                if (const auto bm = meshByName.find(bandName[b]); bm != meshByName.end())
                {
                    const auto i = meshes_->addInstance(bm->second.id, world, mirrored, lights);
                    meshes_->setInstanceLod(i, group, bands);
                    inst = b == 0 ? i : inst;
                }
            }
            ++lodPlacements_;
        }
        if (it == meshByName.end())
        {
            continue; // no near mesh: nothing for the ground below
        }
        if (pli >= firstProp && pli - firstProp < placementProp.size())
        {
            propInstances_[static_cast<std::size_t>(placementProp[pli - firstProp])] = inst;
        }
        // Door parts: placements whose id starts with '_' or '@', up to 4 per id in record order (CODE 0x1017a860).
        if (const auto c = static_cast<char>(pl.doorId & 0xFF); (c == '_' || c == '@') && doors_[pl.doorId].size() < 4)
        {
            doors_[pl.doorId].push_back({ inst, m });
        }

        for (const auto& pc : it->second.model->pieces)
        {
            if (pc.flags & 0x8)
            {
                continue;
            }
            for (std::size_t t = 0; t + 2 < pc.triangles.size(); t += 3)
            {
                glm::vec3 v[3];
                for (int k = 0; k < 3; ++k)
                {
                    const auto& p = pc.vertices[pc.triangles[t + static_cast<std::size_t>(k)]].pos;
                    v[k]          = glm::vec3(world * glm::vec4(p[0], p[1], p[2], 1.0f));
                }
                const glm::vec3 n   = glm::cross(v[1] - v[0], v[2] - v[0]);
                const float     len = glm::length(n);
                if (len > 1e-6f && std::abs(n.y) / len >= 0.2f)
                {
                    ground_.tris.insert(ground_.tris.end(), v, v + 3);
                }
            }
        }
        if (!data.collision.empty())
        {
            continue; // the zone's collision mesh provides floors and walls
        }
        for (const auto& pc : it->second.model->pieces)
        {
            if (pc.flags & 0x8)
            {
                continue; // blended pieces (water surfaces, decals) are not floors
            }
            for (std::size_t t = 0; t + 2 < pc.triangles.size(); t += 3)
            {
                glm::vec3 v[3];
                for (int k = 0; k < 3; ++k)
                {
                    const auto& p = pc.vertices[pc.triangles[t + static_cast<std::size_t>(k)]].pos;
                    v[k]          = glm::vec3(world * glm::vec4(p[0], p[1], p[2], 1.0f));
                }
                const glm::vec3 n   = glm::cross(v[1] - v[0], v[2] - v[0]);
                const float     len = glm::length(n);
                if (len < 1e-6f || std::abs(n.y) / len < 0.35f)
                {
                    continue;
                }
                collision_.addFloor(v);
            }
        }
    }
    meshes_->upload();

    collision_.addCollision(data.collision);
    collision_.build();
    ground_.build();
    spdlog::info("zone {}: {} placements ({} by distance: h / m / l), {} draws, {} triangles, {} floor / {} wall collision triangles, loaded in {} ms", zone,
                 data.placements.size(), lodPlacements_, meshes_->stats().draws, meshes_->stats().triangles, collision_.floors().tris.size() / 3, collision_.walls().tris.size() / 3,
                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

void ZoneScene::draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const SceneParams& params)
{
    meshes_->draw(cmd, frameSlot, viewProj, params);
}

void ZoneScene::groundTriangles(float x0, float z0, float x1, float z1, std::vector<glm::vec3>& out) const
{
    thread_local std::vector<uint32_t> seen;
    seen.clear();
    ground_.visit(x0, z0, x1, z1, [&](const glm::vec3* v) {
        const auto t = static_cast<uint32_t>((v - ground_.tris.data()) / 3);
        if (std::find(seen.begin(), seen.end(), t) != seen.end())
        {
            return;
        }
        seen.push_back(t);
        out.insert(out.end(), v, v + 3);
    });
}

void ZoneScene::setDoorPart(uint32_t door, std::size_t part, const glm::vec3& rot, const glm::vec3& offset)
{
    const auto it = doors_.find(door);
    if (it == doors_.end() || part >= it->second.size())
    {
        return;
    }
    // Row vectors v * R(rot) * T(off) * W0 (CODE 0x100ace20): the part turns about its mesh origin, the hinge.
    const auto& p = it->second[part];
    glm::mat4   m = glm::translate(p.world0, offset);
    m             = glm::rotate(m, rot.z, glm::vec3(0, 0, 1));
    m             = glm::rotate(m, rot.y, glm::vec3(0, 1, 0));
    m             = glm::rotate(m, rot.x, glm::vec3(1, 0, 0));
    meshes_->setTransform(p.instance, gameToRenderMatrix() * m);
}

void ZoneScene::setPropPose(std::size_t i, const glm::vec3& g, float yaw, bool visible)
{
    if (i >= propInstances_.size() || !propInstances_[i])
    {
        return;
    }
    glm::mat4 m = glm::translate(glm::mat4(1.0f), g);
    m           = glm::rotate(m, yaw, glm::vec3(0, 1, 0));
    if (!visible)
    {
        m = glm::scale(m, glm::vec3(0.0f));
    }
    meshes_->setTransform(*propInstances_[i], gameToRenderMatrix() * m);
}

void ZoneScene::setLiftY(uint32_t lift, float gameY)
{
    // Every part's world y = the platform height (CODE 0x100c3850; lifts are unrotated, unscaled).
    if (const auto it = doors_.find(lift); it != doors_.end())
    {
        for (const auto& p : it->second)
        {
            glm::mat4 m = p.world0;
            m[3][1]     = gameY;
            meshes_->setTransform(p.instance, gameToRenderMatrix() * m);
        }
    }
    for (auto& l : lifts_)
    {
        if (l.region.box.id == lift)
        {
            l.y = gameY;
        }
    }
}

std::optional<float> ZoneScene::liftFloor(const glm::vec3& g) const
{
    // Anyone inside the region stands on the platform (CODE 0x100a547f -> 0x10176530): a height override.
    for (const auto& l : lifts_)
    {
        const auto& b = l.region.box;
        if (!l.y)
        {
            continue;
        }
        const float dx = g.x - b.pos[0], dz = g.z - b.pos[2];
        const float c = std::cos(b.yaw), s = std::sin(b.yaw);
        const float lx = dx * c - dz * s, lz = dx * s + dz * c;
        if (std::abs(lx) <= b.size[0] * 0.5f && std::abs(lz) <= b.size[2] * 0.5f && std::abs(g.y - b.pos[1]) <= b.size[1] * 0.5f + 1.0f)
        {
            return l.y;
        }
    }
    return std::nullopt;
}

void ZoneScene::flush(VkCommandBuffer cmd)
{
    meshes_->flush(cmd);
}

} // namespace xi::gfx
