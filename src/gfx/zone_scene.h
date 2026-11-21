// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/chunk.h"
#include "dat/dat_index.h"
#include "dat/effect.h"
#include "dat/weather.h"
#include "dat/zone.h"
#include "gfx/mesh_renderer.h"
#include "gfx/texture_set.h"
#include "gfx/zone_collision.h"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace xi::gfx
{

// A prop file's model chunk: its Mmb `fune`, else `_fun`, else the first (CODE 0x100c4340); nullptr if none.
const dat::Chunk* propModelChunk(const std::vector<dat::Chunk>& chunks);

// Adds an Mmb model's pieces to `meshes` as one mesh (textures by name, else `whiteTex`; the zone render states of
// dat-format §6). Returns the mesh id, or nothing when the model has no triangles.
std::optional<uint32_t> addModelMesh(MeshRenderer& meshes, const std::string& name, const dat::Model& mdl,
                                     const std::unordered_map<std::string, uint32_t>& texByName, uint32_t whiteTex);

// A loaded, renderable zone: textures + placed geometry on the GPU, plus a CPU triangle grid for ground
// queries. Render space is right-handed Y-up: render (x, y, z) = game (x, -y, -z), a 180-degree rotation about X
// (game +x = east, +z = north, +y = down). A rotation, not a reflection: negating only Y mirrors the world
// east-west. Zone pipelines therefore keep the data's clockwise front faces (docs/dat-format.md §5.5).
class ZoneScene
{
public:
    // `model`: the zone model id (dat::zoneModelId(zone), or a Mog House interior); `zone` names it.
    // `props`: extra static models (Mog House furniture), drawn like the zone's own placements.
    ZoneScene(Context& ctx, const dat::DatIndex& index, const dat::ZoneKeys& keys, uint16_t zone, uint16_t model,
              const std::vector<dat::ZoneProp>& props = {}, uint64_t propsKey = 0);

    void draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const SceneParams& params);

    // The walkable world (gfx::ZoneCollision): floors, walls, door boxes and their queries.
    [[nodiscard]] const ZoneCollision& collision() const
    {
        return collision_;
    }
    // Highest walkable surface at (x, z) whose height is <= maxY (render space). nullopt if none.
    [[nodiscard]] std::optional<float> groundHeight(float x, float z, float maxY) const
    {
        return collision_.groundHeight(x, z, maxY);
    }
    // Map id of the collision floor under a render-space point (the highest one at or below maxY); 0 if none (§14).
    [[nodiscard]] uint8_t mapIdAt(float x, float z, float maxY) const
    {
        return collision_.mapIdAt(x, z, maxY);
    }
    // Floor material of the collision floor under a render-space point (dat-format §7: 1 path, 2 grass, 3 sand, 4 snow,
    // 5 stone, 6 metal, 7 wood, 8 shallow / 9 deep water; LSB names); 0 if none. For footsteps.
    [[nodiscard]] uint8_t materialAt(float x, float z, float maxY) const
    {
        return collision_.materialAt(x, z, maxY);
    }
    // Is the collision floor under a render-space point roofed (placement flag bit 2)? The game casts a ray down and
    // takes whatever it hits first (CODE 0x1001444a); this asks the floor: for the indoor ambience.
    [[nodiscard]] bool roofedAt(float x, float z, float maxY) const
    {
        return collision_.roofedAt(x, z, maxY);
    }

    // Zone doors (docs/dat-format.md §5 doors): does door `id` (fourcc) have placed parts, and pose part `part` with
    // a rotation (radians, local X / Y / Z, INFERRED order as placements) and offset over its placed transform.
    [[nodiscard]] std::size_t doorParts(uint32_t door) const
    {
        const auto it = doors_.find(door);
        return it == doors_.end() ? 0 : it->second.size();
    }
    void setDoorPart(uint32_t door, std::size_t part, const glm::vec3& rot, const glm::vec3& offset);
    // Lifts (dat-format §5.8): the platform heights (game y) of each state, and setting the current one: every part
    // of the group moves to that height, and riders inside the region stand on it (liftFloor).
    [[nodiscard]] std::vector<float> liftHeights(uint32_t lift) const
    {
        for (const auto& l : lifts_)
        {
            if (l.region.box.id == lift)
            {
                return l.region.heights;
            }
        }
        return {};
    }
    void setLiftY(uint32_t lift, float gameY);
    // The platform height (game y) under a game-space point inside a lift region.
    [[nodiscard]] std::optional<float> liftFloor(const glm::vec3& gamePos) const;

    // A door box's centre (game space), e.g. the Mog House exit `_720`.
    [[nodiscard]] std::optional<glm::vec3> doorBoxCentre(uint32_t door) const
    {
        return collision_.doorBoxCentre(door);
    }
    // A door's Rid collision box blocks move() while solid (closed; the default).
    void setSolidBoxes(std::vector<dat::ZoneLine> boxes)
    {
        collision_.setSolidBoxes(std::move(boxes));
    }
    void setCameraSolidBoxes(std::vector<dat::ZoneLine> boxes)
    {
        collision_.setCameraSolidBoxes(std::move(boxes));
    }
    void setCameraBoxes(bool on)
    {
        collision_.setCameraBoxes(on); // ZoneCollision::setCameraBoxes
    }
    void setDoorSolid(uint32_t door, bool solid)
    {
        collision_.setDoorSolid(door, solid);
    }
    // Records moved parts and LOD changes; call outside a render pass, before draw().
    void flush(VkCommandBuffer cmd);
    // Picks each LOD placement's mesh for the camera (render space; dat-format §6.3). Before flush().
    void updateLod(const glm::vec3& eye)
    {
        meshes_->updateLod(eye);
    }

    // The render mesh's up-facing ground triangles (render space, 3 vertices each) overlapping an XZ box, each once.
    void groundTriangles(float x0, float z0, float x1, float z1, std::vector<glm::vec3>& out) const;
    // Does the collision mesh (floors and walls) cross the ray from `from` along unit `dir` within `maxDist`
    // (render space)? For the lens flares' sight test.
    [[nodiscard]] bool rayBlocked(const glm::vec3& from, const glm::vec3& dir, float maxDist) const
    {
        return collision_.rayBlocked(from, dir, maxDist);
    }
    // The distance along the ray to the nearest collision triangle within `maxDist`, if any (the camera's collision).
    [[nodiscard]] float cameraDistance(const glm::vec3& target, const glm::vec3& forward, float dist, float rayDrop = 0.0f) const
    {
        return collision_.cameraDistance(target, forward, dist, rayDrop);
    }
    [[nodiscard]] glm::vec3 cameraClearance(const glm::vec3& eye, const glm::vec3& forward) const
    {
        return collision_.cameraClearance(eye, forward);
    }
    [[nodiscard]] std::optional<float> cameraRay(const glm::vec3& from, const glm::vec3& to) const
    {
        return collision_.cameraRay(from, to);
    }
    [[nodiscard]] std::optional<float> rayHit(const glm::vec3& from, const glm::vec3& dir, float maxDist, bool skipBarriers = false) const
    {
        return collision_.rayHit(from, dir, maxDist, skipBarriers);
    }

    // Character movement against the collision mesh (render space): horizontal step, wall push-out with
    // sliding, step-up of at most kStepUp, then floor snap. Without collision data only the floor is followed.
    [[nodiscard]] glm::vec3 move(const glm::vec3& feet, const glm::vec3& delta) const
    {
        return collision_.move(feet, delta);
    }

    [[nodiscard]] bool hasCollision() const
    {
        return collision_.hasCollision();
    }

    [[nodiscard]] uint16_t zone() const
    {
        return zone_;
    }
    [[nodiscard]] uint16_t model() const
    {
        return model_;
    }
    // Moves prop `i` (the constructor's `props` order): a ship's hull on its path. Hidden props are not drawn.
    void setPropPose(std::size_t i, const glm::vec3& gamePos, float yaw, bool visible);
    // Identifies the props it was built with (the caller's hash).
    [[nodiscard]] uint64_t propsKey() const
    {
        return propsKey_;
    }
    // Mmbs the Mzb does not place, by chunk name (`sea0`, `wind`, `ltcu` ...): zone effects draw them as particles
    // (the sea, windmills, lamp glows; effect-format zone effects). nullptr when there is none.
    [[nodiscard]] const dat::FxMesh* effectMesh(const std::string& chunk) const
    {
        const auto it = effectMeshes_.find(chunk);
        return it == effectMeshes_.end() ? nullptr : &it->second;
    }
    // The zone's point-light table (Mzb): the light generators' 4-char names, by index (SceneParams::pointLights).
    [[nodiscard]] const std::vector<uint32_t>& lightKeys() const
    {
        return lightKeys_;
    }
    [[nodiscard]] const dat::ZoneWeather& weather() const
    {
        return weather_;
    }
    [[nodiscard]] const std::vector<dat::ZoneLine>& zoneLines() const
    {
        return zoneLines_;
    }
    // --rt (goal 38): the geometry and textures, for acceleration structures and hit shading.
    [[nodiscard]] const MeshRenderer& meshRenderer() const
    {
        return *meshes_;
    }
    [[nodiscard]] const TextureSet& textureSet() const
    {
        return *textures_;
    }
    [[nodiscard]] MeshRenderer::Stats stats() const
    {
        return meshes_->stats();
    }

    static glm::vec3 gameToRender(const glm::vec3& g)
    {
        return { g.x, -g.y, -g.z };
    }
    static glm::vec3 renderToGame(const glm::vec3& r)
    {
        return { r.x, -r.y, -r.z };
    }
    static glm::mat4 gameToRenderMatrix()
    {
        return glm::mat4(glm::vec4(1, 0, 0, 0), glm::vec4(0, -1, 0, 0), glm::vec4(0, 0, -1, 0), glm::vec4(0, 0, 0, 1));
    }

private:

    uint16_t                      zone_;
    uint16_t                      model_ = 0;
    uint64_t                      propsKey_ = 0;
    std::size_t                   lodPlacements_ = 0; // placements drawn with h / m / l meshes by distance
    std::vector<std::optional<uint32_t>> propInstances_; // per prop: its mesh instance
    std::unordered_map<std::string, dat::FxMesh> effectMeshes_;
    std::vector<uint32_t>                        lightKeys_;
    std::vector<dat::ZoneLine>    zoneLines_;
    dat::ZoneWeather              weather_;
    std::unique_ptr<TextureSet>   textures_;
    std::unique_ptr<MeshRenderer> meshes_;

    ZoneCollision collision_;
    TriGrid       ground_; // the render mesh's up-facing, unblended triangles (ground shadow decals)
    struct DoorPart
    {
        uint32_t  instance = 0;
        glm::mat4 world0{ 1.0f }; // the placement's game-space matrix
    };
    std::unordered_map<uint32_t, std::vector<DoorPart>> doors_;
    struct Lift
    {
        dat::LiftRegion      region;
        std::optional<float> y; // current platform height (game y), once set
    };
    std::vector<Lift> lifts_;
};

} // namespace xi::gfx
