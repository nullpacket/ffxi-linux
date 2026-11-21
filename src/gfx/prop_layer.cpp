// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/prop_layer.h"

#include "dat/chunk.h"
#include "dat/texture.h"
#include "gfx/zone_scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace xi::gfx
{

namespace
{

constexpr std::size_t kSlots = 4; // ships of one file in a zone at once (INFERRED enough; more rebuild that file)

glm::mat4 hidden()
{
    return glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
}

} // namespace

PropLayer::PropLayer(Context& ctx, const dat::DatIndex& index, const dat::ZoneKeys& keys)
: ctx_(ctx)
, index_(index)
, keys_(keys)
{
}

std::unique_ptr<PropLayer::FileModel> PropLayer::build(uint32_t fileId, std::size_t slots) const
{
    if (!index_.path(fileId))
    {
        return nullptr;
    }
    const auto              t0   = std::chrono::steady_clock::now();
    const auto              file = index_.read(fileId);
    std::vector<dat::Chunk> chunks;
    dat::parseChunks(file, chunks);
    const dat::Chunk* pick = propModelChunk(chunks);
    if (!pick)
    {
        return nullptr;
    }
    std::vector<std::byte> payload(pick->payload.begin(), pick->payload.end());
    dat::decodeMmb(payload, keys_);
    const auto model = dat::parseMmb(payload);

    auto fm      = std::make_unique<FileModel>();
    fm->textures = std::make_unique<TextureSet>(ctx_);
    std::unordered_map<std::string, uint32_t> texByName;
    for (const auto& c : chunks)
    {
        if (c.type == 0x20 || c.type == 0x1B)
        {
            try
            {
                const auto tex = dat::parseTexture(c.payload);
                texByName.try_emplace(tex.name, fm->textures->add(tex));
            }
            catch (const std::exception& e)
            {
                spdlog::warn("prop {}: texture {}: {}", fileId, c.name, e.what());
            }
        }
    }
    dat::Texture white;
    white.width  = 4;
    white.height = 4;
    white.data.assign(4 * 4 * 4, std::byte{ 0x80 }); // FFXI scale: 0x80 = full intensity / opaque
    const uint32_t whiteTex = fm->textures->add(white);
    fm->textures->commit();

    fm->meshes   = std::make_unique<MeshRenderer>(ctx_, *fm->textures);
    const auto m = addModelMesh(*fm->meshes, "prop:" + std::to_string(fileId), model, texByName, whiteTex);
    if (!m)
    {
        return nullptr;
    }
    for (std::size_t i = 0; i < slots; ++i)
    {
        fm->slots.push_back(fm->meshes->addInstance(*m, hidden()));
    }
    fm->owners.assign(slots, 0);
    fm->meshes->upload();
    spdlog::info("prop {}: {} slots, {} triangles, built in {} ms", fileId, slots, fm->meshes->stats().triangles,
                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    return fm;
}

bool PropLayer::pose(uint32_t key, uint32_t fileId, const glm::vec3& g, float yaw)
{
    auto it = keySlot_.find(key);
    if (it != keySlot_.end() && it->second.first != fileId)
    {
        keepOnly({}); // a key changing file (never seen) starts again
        it = keySlot_.end();
    }
    if (it == keySlot_.end())
    {
        auto f = files_.find(fileId);
        if (f == files_.end())
        {
            f = files_.emplace(fileId, build(fileId, kSlots)).first;
        }
        if (!f->second)
        {
            return false;
        }
        auto&      fm   = *f->second;
        const auto free = std::find(fm.owners.begin(), fm.owners.end(), 0u);
        if (free == fm.owners.end())
        {
            // All slots taken: rebuild this file with twice as many, keeping the owners (they are re-posed each frame).
            const auto owners = fm.owners;
            f->second         = build(fileId, owners.size() * 2);
            if (!f->second)
            {
                return false;
            }
            std::copy(owners.begin(), owners.end(), f->second->owners.begin());
            return pose(key, fileId, g, yaw);
        }
        *free = key;
        it    = keySlot_.emplace(key, std::make_pair(fileId, static_cast<std::size_t>(free - fm.owners.begin()))).first;
    }
    auto& fm  = *files_.at(fileId);
    glm::mat4 m = glm::translate(glm::mat4(1.0f), g);
    m           = glm::rotate(m, yaw, glm::vec3(0, 1, 0));
    fm.meshes->setTransform(fm.slots[it->second.second], ZoneScene::gameToRenderMatrix() * m);
    return true;
}

void PropLayer::keepOnly(const std::vector<uint32_t>& keep)
{
    for (auto it = keySlot_.begin(); it != keySlot_.end();)
    {
        if (std::find(keep.begin(), keep.end(), it->first) != keep.end())
        {
            ++it;
            continue;
        }
        if (const auto f = files_.find(it->second.first); f != files_.end() && f->second)
        {
            f->second->owners[it->second.second] = 0;
            f->second->meshes->setTransform(f->second->slots[it->second.second], hidden());
        }
        it = keySlot_.erase(it);
    }
}

void PropLayer::flush(VkCommandBuffer cmd)
{
    for (auto& [id, fm] : files_)
    {
        if (fm)
        {
            fm->meshes->flush(cmd);
        }
    }
}

void PropLayer::draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const SceneParams& params)
{
    for (auto& [id, fm] : files_)
    {
        if (fm && std::find_if(fm->owners.begin(), fm->owners.end(), [](uint32_t o) { return o != 0; }) != fm->owners.end())
        {
            fm->meshes->draw(cmd, frameSlot, viewProj, params);
        }
    }
}

} // namespace xi::gfx
