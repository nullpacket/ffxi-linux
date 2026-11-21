// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"
#include "dat/zone.h"
#include "gfx/mesh_renderer.h"
#include "gfx/texture_set.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace xi::gfx
{

// Moving models that come and go while the zone stays (ships and airships, kind 4 entities; ship_re.md): each prop
// file's Mmb (`fune`, else `_fun`, else the first; CODE 0x100c4340) is built once into its own small renderer with a
// few instance slots, and a prop showing up or leaving only takes or frees a slot. Before this the props were
// placements of the ZoneScene, so a ship arriving rebuilt the whole zone.
class PropLayer
{
public:
    PropLayer(Context& ctx, const dat::DatIndex& index, const dat::ZoneKeys& keys);

    // Shows `fileId`'s model for `key` (an entity id) at a game-space position and yaw (radians about the vertical);
    // the first call for a key takes a free slot, building the file's model when it is new. False if the file has no
    // model.
    bool pose(uint32_t key, uint32_t fileId, const glm::vec3& gamePos, float yaw);
    // Hides and frees every key not in `keep`.
    void keepOnly(const std::vector<uint32_t>& keep);
    // Records moved instances; call outside a render pass, before draw().
    void flush(VkCommandBuffer cmd);
    void draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const SceneParams& params);

    [[nodiscard]] std::size_t models() const
    {
        return files_.size();
    }

    // --rt (goal 38): every built ship / prop model (hidden slots have a zero-scale transform).
    void forEachMeshes(const std::function<void(const MeshRenderer&)>& f) const
    {
        for (const auto& [id, m] : files_)
        {
            if (m && m->meshes)
            {
                f(*m->meshes);
            }
        }
    }

private:
    struct FileModel
    {
        std::unique_ptr<TextureSet>   textures;
        std::unique_ptr<MeshRenderer> meshes;
        std::vector<uint32_t>         slots;  // instance per slot
        std::vector<uint32_t>         owners; // key per slot, 0 = free
    };
    // Builds `fileId`'s model with `slots` instance slots (all hidden); nullptr if the file has no Mmb.
    std::unique_ptr<FileModel> build(uint32_t fileId, std::size_t slots) const;

    Context&                                             ctx_;
    const dat::DatIndex&                                 index_;
    const dat::ZoneKeys&                                 keys_;
    std::unordered_map<uint32_t, std::unique_ptr<FileModel>> files_;  // by file id (nullptr: no model)
    std::unordered_map<uint32_t, std::pair<uint32_t, std::size_t>> keySlot_; // key -> file, slot
};

} // namespace xi::gfx
