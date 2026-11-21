// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/character_model.h"

#include "gfx/raytracing.h"
#include "gfx/zone_scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace xi::gfx
{

CharacterUpload prepareCharacterUpload(const dat::CharacterAsset& a)
{
    CharacterUpload up;
    // The model's textures (once each by name), decoded to RGBA here, plus the grey fallback.
    for (const auto& t : a.textures)
    {
        if (std::ranges::find(up.textureNames, t.name) == up.textureNames.end())
        {
            up.textureNames.push_back(t.name);
            up.textures.push_back(TextureSet::decode(t));
        }
    }
    up.textures.push_back(TextureSet::Decoded{ 4, 4, std::vector<uint8_t>(64, 0x80) });

    // Cloth particles take palette slots after the skeleton's; vertex bone indices are bytes.
    up.simulate = a.clothParticles > 0 && a.skeleton.bones.size() + a.clothParticles <= 256;
    up.vertices.reserve(a.vertices.size());
    for (const auto& v : a.vertices)
    {
        SkinnedVertex s;
        if (up.simulate && v.cloth >= 0)
        {
            s.p0       = glm::vec3(0.0f);
            s.p1       = glm::vec3(0.0f);
            s.n0       = glm::vec3(0.0f, 1.0f, 0.0f);
            s.n1       = glm::vec3(0.0f);
            s.uv       = { v.uv[0], v.uv[1] };
            s.bones[0] = s.bones[1] = static_cast<uint8_t>(a.skeleton.bones.size() + static_cast<std::size_t>(v.cloth));
            s.weight1  = 0.0f;
            up.vertices.push_back(s);
            continue;
        }
        s.p0       = { v.p0[0], v.p0[1], v.p0[2] };
        s.p1       = { v.p1[0], v.p1[1], v.p1[2] };
        s.n0       = { v.n0[0], v.n0[1], v.n0[2] };
        s.n1       = { v.n1[0], v.n1[1], v.n1[2] };
        s.uv       = { v.uv[0], v.uv[1] };
        s.bones[0] = v.bone0;
        s.bones[1] = v.bone1;
        s.weight1  = v.weight1;
        up.vertices.push_back(s);
    }
    up.indices.resize(up.vertices.size());
    for (uint32_t i = 0; i < up.indices.size(); ++i)
    {
        up.indices[i] = i;
    }
    return up;
}

CharacterModel beginCharacterUpload(const dat::CharacterAsset& a, const CharacterUpload& up, TextureSet& textures, SkinnedRenderer& renderer,
                                    uint64_t& ticket)
{
    // One command buffer for the textures (copies and mips) and the vertex / index buffers, submitted without a wait.
    Context&                    ctx = renderer.context();
    std::vector<const TextureSet::Decoded*> batch;
    for (const auto& t : up.textures)
    {
        batch.push_back(&t);
    }
    const VkDeviceSize vbytes = up.vertices.size() * sizeof(SkinnedVertex);
    const VkDeviceSize ibytes = up.indices.size() * sizeof(uint32_t);
    Buffer             vb     = ctx.createBuffer(vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtGeometryUsage(), false);
    Buffer             ib     = ctx.createBuffer(ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtGeometryUsage(), false);
    std::vector<Buffer> staging;
    Buffer              vs = ctx.createBuffer(vbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    Buffer              is = ctx.createBuffer(ibytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(vs.mapped, up.vertices.data(), vbytes);
    std::memcpy(is.mapped, up.indices.data(), ibytes);
    staging.push_back(vs);
    staging.push_back(is);
    std::vector<uint32_t> ids;
    ticket = ctx.submitAsync(
        [&](VkCommandBuffer cmd) {
            ids = textures.record(batch, cmd, staging);
            const VkBufferCopy vc{ 0, 0, vbytes };
            vkCmdCopyBuffer(cmd, vs.buffer, vb.buffer, 1, &vc);
            const VkBufferCopy ic{ 0, 0, ibytes };
            vkCmdCopyBuffer(cmd, is.buffer, ib.buffer, 1, &ic);
        },
        {});
    // submitAsync took an empty list (the textures' staging buffers are only known once recorded): hand them over now.
    ctx.adoptStaging(ticket, std::move(staging));
    textures.commit();

    std::unordered_map<std::string, uint32_t> texByName;
    for (std::size_t i = 0; i < up.textureNames.size(); ++i)
    {
        texByName.emplace(up.textureNames[i], ids[i]);
    }
    const uint32_t white = ids.back();
    std::vector<SubMesh> subs;
    for (const auto& p : a.parts)
    {
        SubMesh s;
        s.firstIndex = p.first;
        s.indexCount = p.count;
        const auto it = texByName.find(p.texture);
        s.texture    = it != texByName.end() ? it->second : white;
        s.alphaTest  = true; // hair/cloth cut-outs
        s.partTag    = p.wep;
        // Os2 material (dat-format §10.3): light scale, env / spec reflections with their strength.
        const auto scale = static_cast<uint32_t>(std::clamp(p.lightScale * 64.0f + 0.5f, 1.0f, 255.0f));
        const auto str   = static_cast<uint32_t>(std::clamp(p.strength * 255.0f + 0.5f, 0.0f, 255.0f));
        const auto env   = p.env ? texByName.find(a.envTexture) : texByName.end();
        const auto spec  = p.spec ? texByName.find(a.specTexture) : texByName.end();
        s.material       = scale << 16 | str << 24 | (env != texByName.end() ? 4u : 0u) | (spec != texByName.end() ? 8u : 0u);
        s.envTexture     = env != texByName.end() ? env->second : white;
        s.specTexture    = spec != texByName.end() ? spec->second : white;
        subs.push_back(s);
    }

    CharacterModel m;
    m.model = renderer.addModel(vb, ib, std::move(subs));
    float minY = 0.0f;
    for (const auto& b : a.skeleton.bindWorld)
    {
        m.bindWorld.push_back(glm::make_mat4(b.data())); // row-vector D3D matrix -> column-vector glm (transpose)
        minY = std::min(minY, b[13]);
    }
    m.height   = std::max(1.0f, -minY + 0.2f); // Y down: the head is the most negative bone
    m.skeleton = a.skeleton;
    if (up.simulate)
    {
        m.cloth.meshes    = a.cloth;
        m.cloth.spheres   = a.skeleton.spheres;
        m.cloth.base      = static_cast<uint32_t>(a.skeleton.bones.size());
        m.cloth.particles = a.clothParticles;
    }
    return m;
}

CharacterModel uploadCharacter(const dat::CharacterAsset& a, TextureSet& textures, SkinnedRenderer& renderer)
{
    uint64_t   ticket = 0;
    const auto up     = prepareCharacterUpload(a);
    auto       m      = beginCharacterUpload(a, up, textures, renderer, ticket);
    renderer.context().waitUpload(ticket);
    return m;
}

void characterPalette(const CharacterModel& m, const glm::vec3& gamePos, float dir, std::vector<glm::mat4>& out, const AnimState& anim)
{
    // Heading byte (LSB): facing = (cos a, 0, sin a) in game space with a = -dir * pi / 128; bind pose faces +x.
    const float a = -dir * 3.14159265f / 128.0f;
    const float c = std::cos(a), s = std::sin(a);
    glm::mat4   rot(1.0f);
    rot[0] = glm::vec4(c, 0, s, 0);
    rot[2] = glm::vec4(-s, 0, c, 0);
    const glm::mat4 root = ZoneScene::gameToRenderMatrix() * glm::translate(glm::mat4(1.0f), gamePos) * rot * glm::scale(glm::mat4(1.0f), glm::vec3(m.scale));
    if (anim.set && !anim.set->empty())
    {
        thread_local std::vector<dat::RowMatrix> posed;
        if (anim.from && anim.blend < 1.0f)
        {
            dat::poseSkeletonBlend(m.skeleton, anim.from, anim.fromFrame, anim.set, anim.frame, anim.blend, posed);
        }
        else
        {
            dat::poseSkeleton(m.skeleton, { anim.set }, anim.frame, posed);
        }
        out.resize(posed.size());
        for (std::size_t i = 0; i < posed.size(); ++i)
        {
            out[i] = root * glm::make_mat4(posed[i].data());
        }
        restCloth(m.cloth, out);
        return;
    }
    out.resize(m.bindWorld.size());
    for (std::size_t i = 0; i < m.bindWorld.size(); ++i)
    {
        out[i] = root * m.bindWorld[i];
    }
    restCloth(m.cloth, out);
}

} // namespace xi::gfx
