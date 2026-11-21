// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/context.h"
#include "gfx/texture_set.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>
#include <utility>

namespace xi::gfx
{

// Canonical vertex every asset format is converted into (36 bytes).
struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    uint32_t  color; // RGBA8 (vertex colour; FFXI tints zone geometry with it)
    glm::vec2 uv;
};
static_assert(sizeof(Vertex) == 36);

// One drawable piece of a mesh: an index range using a single texture, plus its render state
// (FFXI zone rules: docs/dat-format.md §6).
struct SubMesh
{
    uint32_t firstIndex   = 0;
    uint32_t indexCount   = 0;
    int32_t  vertexOffset = 0;
    uint32_t texture      = 0; // index into the TextureSet
    bool     blended      = false; // alpha blend, no depth write, depth bias
    bool     doubleSided  = false; // no culling
    bool     alphaTest    = false; // discard when final alpha <= 0x60/255
    int8_t   partTag      = -1;    // characters: the wepN part it came from (SkinnedRenderer::Instance::hiddenTags)
    // Characters: the Os2 material (skinned.frag flags: bit2 env, bit3 spec, 16..23 light scale x 64, 24..31 strength)
    // and the textures its reflections sample.
    uint32_t material     = 0;
    uint32_t envTexture   = 0;
    uint32_t specTexture  = 0;
};

// Scene-wide shading inputs: the zone environment (docs/dat-format.md §9) evaluated for the current
// weather and Vana'diel time. Directions are in render space and point *toward* the light.
struct SceneParams
{
    glm::vec3 eye{ 0.0f };
    glm::vec3 fogColor{ 0.6f, 0.7f, 0.85f };
    float     fogStart = 150.0f;
    float     fogEnd   = 600.0f;
    glm::vec3 ambient{ 0.65f };
    glm::vec3 light0Dir{ 0.4f, 0.8f, -0.3f };
    glm::vec3 light0Color{ 0.45f };
    glm::vec3 light1Dir{ 0.0f, 1.0f, 0.0f };
    glm::vec3 light1Color{ 0.0f };
    glm::vec3 skyColor{ 0.6f, 0.7f, 0.85f }; // clear colour behind the zone
    // The camera's basis in render space (character env / specular reflections use camera-space normals).
    glm::vec3 camRight{ 1.0f, 0.0f, 0.0f };
    glm::vec3 camUp{ 0.0f, 1.0f, 0.0f };
    glm::vec3 camForward{ 0.0f, 0.0f, -1.0f };
    // Zone point lights by Mzb light-table index (effect-format §Z): render-space position and range, colour x
    // intensity (0 = off). Meshes add colour·max(N·L,0)/d² for up to 4 of them within range.
    struct PointLight
    {
        glm::vec3 pos{ 0.0f };
        float     range = 0.0f;
        glm::vec3 colour{ 0.0f };
    };
    std::vector<PointLight> pointLights; // at most kMaxPointLights
    static constexpr std::size_t kMaxPointLights = 256;
};

// Static scene geometry (a whole zone): every mesh lives in one vertex/index buffer pair, instances are
// transforms in a storage buffer, and each pass is a single multi-draw-indirect call.
class MeshRenderer
{
public:
    struct Stats
    {
        uint32_t draws     = 0;
        uint64_t triangles = 0;
    };

    MeshRenderer(Context& ctx, TextureSet& textures);
    ~MeshRenderer();
    MeshRenderer(const MeshRenderer&)            = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;

    // Build phase (CPU side). Returns the mesh id.
    uint32_t addMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, std::vector<SubMesh> subMeshes);
    // mirrored: the instance transform flips handedness (front faces swap).
    // `lights`: up to 4 point-light indices, 1-based into SceneParams::pointLights, one per byte (0 = none).
    // Returns the instance index (for setTransform).
    uint32_t addInstance(uint32_t mesh, const glm::mat4& transform, bool mirrored = false, uint32_t lights = 0);
    // Level of detail (zone placements with h / m / l meshes, dat-format §6.3): a group is a placement's centre (render
    // space) and its squared band limits; each of its instances draws in the bands of `bands` (bit 0 near: d² <= near2,
    // bit 1 middle: d² <= mid2, bit 2 far), chosen per frame by updateLod.
    uint32_t addLodGroup(const glm::vec3& centre, float near2, float mid2);
    void     setInstanceLod(uint32_t instance, uint32_t group, uint8_t bands);
    // Picks each group's band for the eye (render space); the changes are recorded by flush().
    void updateLod(const glm::vec3& eye);
    // Moves an instance after upload (zone doors); recorded by flush(), which must run outside a render pass.
    void setTransform(uint32_t instance, const glm::mat4& transform);
    void flush(VkCommandBuffer cmd);

    // Uploads everything to the GPU. Call once after building.
    void upload();

    // frameSlot: frame-in-flight index (per-frame parameter buffer).
    void draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const SceneParams& scene);

    [[nodiscard]] Stats stats() const
    {
        return stats_;
    }

    // --rt (goal 38): a mesh's sub-meshes for acceleration structures (blended and alpha-tested ones are tested against
    // their texture's alpha per hit), the buffers they index, and every instance drawn now (doors as moved, the LOD
    // band picked).
    struct RtPart
    {
        uint32_t firstIndex = 0, indexCount = 0;
        int32_t  vertexOffset = 0;
        uint32_t maxVertex = 0, texture = 0;
        bool     alphaTest = false;
        bool     blended   = false; // alpha-blended (foliage, glass, water): its texture alpha decides each hit
    };
    [[nodiscard]] std::vector<RtPart> rtParts(uint32_t mesh) const;
    [[nodiscard]] VkDeviceAddress     rtVertices() const;
    [[nodiscard]] VkDeviceAddress     rtIndices() const;
    void rtInstances(const std::function<void(uint32_t mesh, const glm::mat4& transform)>& f) const;

private:
    struct MeshRecord
    {
        std::vector<SubMesh> subMeshes;
    };
    struct DrawData // per indirect draw, indexed by gl_DrawID (std430)
    {
        uint32_t transform;
        uint32_t texture;
        uint32_t flags; // bit0 = alpha test
        uint32_t lights; // 4 x u8 point-light indices (1-based)
    };
    // Draws are bucketed by pipeline state: [blend][cull], cull = back / front / none.
    static constexpr int kCullModes = 3;
    struct Bucket
    {
        uint32_t first = 0;
        uint32_t count = 0;
    };

    Context&    ctx_;
    TextureSet& textures_;

    std::vector<Vertex>     vertices_;
    std::vector<uint32_t>   indices_;
    std::vector<MeshRecord> meshes_;
    std::vector<glm::mat4>  transforms_;
    struct Instance
    {
        uint32_t mesh;
        uint32_t transform;
        bool     mirrored;
        uint32_t lights = 0;
        uint32_t lodGroup = UINT32_MAX;
        uint8_t  bands    = 7;
    };
    std::vector<Instance> instances_;
    struct LodGroup
    {
        glm::vec3 centre;
        float     near2 = 0, mid2 = 0;
        int       band  = 0; // the band its draws are set for (0 near, 1 middle, 2 far)
        std::vector<std::pair<uint32_t, uint8_t>> draws; // indirect command index, bands
    };
    std::vector<LodGroup> lodGroups_;
    std::vector<std::pair<uint32_t, uint32_t>> pendingCounts_; // indirect command index, instance count
    std::vector<std::pair<uint32_t, glm::mat4>> pending_; // transform index, matrix

    Buffer vertexBuffer_, indexBuffer_, transformBuffer_, cmdBuffer_, dataBuffer_;
    std::array<Buffer, Context::kFramesInFlight> frameParams_;
    Bucket buckets_[2][kCullModes]{};
    Stats  stats_;

    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline       pipelines_[2][kCullModes]{};
};

} // namespace xi::gfx
