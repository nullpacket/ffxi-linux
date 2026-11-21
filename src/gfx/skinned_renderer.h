// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/context.h"
#include "gfx/mesh_renderer.h"
#include "gfx/texture_set.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <optional>
#include <tuple>
#include <vector>

namespace xi::gfx
{

// Vertex for skinned (character) meshes, FFXI style (docs/dat-format.md §10): up to two bones, each with its own
// bone-local position and normal. world = (1-w) * (B[bone0] * p0) + w * (B[bone1] * p1). 64 bytes.
struct SkinnedVertex
{
    glm::vec3 p0;
    glm::vec3 p1;
    glm::vec3 n0;
    glm::vec3 n1;
    glm::vec2 uv;
    uint8_t   bones[4]{}; // [0], [1] used
    float     weight1 = 0.0f;
};
static_assert(sizeof(SkinnedVertex) == 64);

// GPU-skinned character meshes. Each model owns its buffers (characters stream in and out); every frame the
// caller submits instances with a bone palette, which is uploaded once into a per-frame buffer.
class SkinnedRenderer
{
public:
    struct Instance
    {
        uint32_t                model = 0;
        std::vector<glm::mat4>  bones; // render-space bone matrices (instance root * bone world), one per skeleton bone
        float                   fade = 0.0f; // 0 = solid, 1 = gone (a dithered dissolve)
        // The actor's position (render space): the specular reflection's light look-at is centred on it (skinned.frag).
        // Unset: the first bone's.
        std::optional<glm::vec3> origin;
        glm::vec4               tint{ 1.0f }; // material colour, 1 = 0x80 = unchanged (effect ops 0x29 / 0x2A)
        uint16_t                hiddenTags = 0; // sub-meshes whose partTag bit is set are skipped (effect ops 0x75 / 0xA3)
        // A texture drawn over the whole model in a second pass (effect ops 0x40 / 0x41 / 0x54 / 0x55; CODE draw
        // 0x1002b885): sampled by the camera-space normal (0.5 + n / 2, the texture matrix 0x10457158 INFERRED),
        // moved by `overlayScroll` (ops 0x42-0x45), added (blend 0x48) or alpha-blended.
        const dat::Texture*     overlay = nullptr;
        glm::vec2               overlayScroll{ 0.0f };
        bool                    overlayAdditive = true;
        glm::vec4               overlayTint{ 1.0f }; // ops 0x46 / 0x47 (colour slot 1): multiplies the overlay
        // The screen-refraction pass strength (effect ops 0x48 / 0x49; 0 = off): drawRefraction().
        float                   refraction = 0.0f;
    };

    SkinnedRenderer(Context& ctx, TextureSet& textures);
    ~SkinnedRenderer();
    SkinnedRenderer(const SkinnedRenderer&)            = delete;
    SkinnedRenderer& operator=(const SkinnedRenderer&) = delete;

    uint32_t addModel(const std::vector<SkinnedVertex>& vertices, const std::vector<uint32_t>& indices, std::vector<SubMesh> subMeshes);
    // Adopts device-local vertex / index buffers filled elsewhere (an async upload: draw only once it is done).
    uint32_t addModel(Buffer vertices, Buffer indices, std::vector<SubMesh> subMeshes);
    void     removeModel(uint32_t model);
    [[nodiscard]] Context& context()
    {
        return ctx_;
    }

    // Uploads the instances' overlay textures not seen yet (before the frame's command buffer is recorded).
    void prepare(const std::vector<Instance>& instances);
    void draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const SceneParams& scene, const std::vector<Instance>& instances);
    // True when an instance has a refraction pass (the frame then needs the screen copy).
    [[nodiscard]] static bool needsScreen(const std::vector<Instance>& instances);
    // After draw() with the same instances, inside the rendering resumed after the screen copy: the instances with a
    // refraction strength again, textured with the screen copy (`screenSet` of layout `screenLayout`, binding 0 =
    // sampler2D[2], EffectRenderer's).
    void drawRefraction(VkCommandBuffer cmd, const glm::mat4& viewProj, const std::vector<Instance>& instances, VkDescriptorSetLayout screenLayout,
                        VkDescriptorSet screenSet);

    // Detailed shadows (shadow_re.md mode 1): after draw(), outside rendering, each job's instance flattened through
    // `viewProj` into the `cell` (x, y, size in pixels) of `target` (RGBA8 UNORM, `extent`), which is cleared first and
    // left in SHADER_READ_ONLY_OPTIMAL.
    struct SilhouetteJob
    {
        std::size_t instance = 0; // index into draw()'s instances
        glm::mat4   viewProj{ 1.0f };
        VkRect2D    cell{};
    };
    // DLSS motion vectors (goal 15): after draw() in the same frame, inside rendering into the motion image (R16G16)
    // with the scene's depth (tested, not written). `motionBlock` is MotionVectors::frameBlock's address. Each instance
    // is matched to last frame's by model and nearest root, for its previous bones; unmatched ones move with the camera.
    void drawMotion(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& jitteredViewProj, VkDeviceAddress motionBlock,
                    const std::vector<Instance>& instances);
    void drawSilhouettes(VkCommandBuffer cmd, const Image& target, const std::vector<Instance>& instances, const std::vector<SilhouetteJob>& jobs);
    // Makes the silhouette and refraction pipelines now (at start-up) instead of on their first draw.
    void prewarm(VkFormat silhouetteFormat, VkDescriptorSetLayout screenLayout);

    // --rt (goal 38): the characters the last draw() submitted, for acceleration structures: each model's buffers, its
    // palette (lastBones) and the opaque sub-meshes not hidden (first index, index count, vertex offset).
    struct RtActor
    {
        VkDeviceAddress vertices = 0, indices = 0, bones = 0;
        uint32_t        vertexCount = 0, boneBase = 0;
        glm::vec3       origin{ 0.0f };
        std::vector<std::tuple<uint32_t, uint32_t, int32_t>> parts;
    };
    void                                rtRemember(const std::vector<Instance>& instances);
    [[nodiscard]] const std::vector<RtActor>& rtActors() const
    {
        return rtActors_;
    }

private:
    void ensureSilhouettePipeline(VkFormat format);
    void ensureRefractPipeline(VkDescriptorSetLayout screenLayout);
    struct Model
    {
        Buffer               vertices, indices;
        std::vector<SubMesh> subMeshes;
        bool                 alive = false;
    };
    static constexpr std::size_t kMaxBones = 64 * 1024;

    Context&                                     ctx_;
    TextureSet&                                  textures_;
    std::vector<Model>                           models_;
    std::array<Buffer, Context::kFramesInFlight> bonePalettes_;
    std::array<Buffer, Context::kFramesInFlight> frameParams_;
    VkPipelineLayout                             layout_ = VK_NULL_HANDLE;
    VkPipeline                                   pipelines_[2][3]{}; // [blend][cull back/front/none]
    VkPipeline                                   overlayPipelines_[2]{}; // the overlay pass: [0] alpha, [1] additive
    std::unordered_map<const dat::Texture*, uint32_t> overlayIndex_;
    VkPipelineLayout                             refractLayout_   = VK_NULL_HANDLE; // made on first use (needs the screen layout)
    VkPipeline                                   refractPipeline_ = VK_NULL_HANDLE;
    VkPipeline                                   silhouettePipeline_ = VK_NULL_HANDLE; // prewarm(), else on first use
    VkDeviceAddress                              lastBones_ = 0, lastFrame_ = 0; // draw()'s palette and frame parameters
    struct PrevInstance
    {
        uint32_t               model = 0;
        glm::vec3              root{ 0.0f };
        std::vector<glm::mat4> bones;
    };
    std::vector<PrevInstance>                    prevInstances_;     // drawMotion's last frame
    std::array<Buffer, Context::kFramesInFlight> prevPalettes_{};    // made on first drawMotion
    VkPipeline                                   motionPipelines_[3]{}; // [cull back/front/none], made on first use
    std::size_t                                  lastUsed_  = 0;
    std::vector<RtActor>                         rtActors_;
};

} // namespace xi::gfx
