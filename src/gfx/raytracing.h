// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Optional ray-traced lighting (`--rt`, goal 38; docs/raytracing.md). Without the flag nothing here exists: no
// extensions, acceleration structures or passes. With it, the zone, props, ships, Mog House furniture and the drawn
// characters go into a TLAS each frame, and compute passes trace ambient occlusion, sun / moon shadows, zone point-light
// shadows and one diffuse bounce from the scene's depth. The result re-weights the game's own lighting terms (ambient,
// light 0 / 1, point lights) per pixel, so the look stays the game's: same light colours and directions, same fog.

#include "gfx/context.h"
#include "gfx/mesh_renderer.h"

#include <glm/glm.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace xi::gfx
{

class SkinnedRenderer;

// A renderer that put geometry into acceleration structures is going away (MeshRenderer's destructor).
void rtForget(const void* owner);

struct RtOptions
{
    bool ao          = false;
    bool shadows     = false;
    bool points      = false; // zone point-light shadows (lamps, fires), with shadows
    bool gi          = false;
    bool reflections = false;
    float strength   = 0.7f; // how far the traced terms replace the game's (0 = as without --rt); 0.7 keeps it subtle
    // Tuning (goal 41 `--rt-boost`; the defaults are goal 38's look): the most of a pixel's lighting light 0's shadow may
    // remove, the water reflection's weight scale, the bounce's weight, the ambient occlusion radius (yalms).
    float shadowCap = 0.4f;
    float reflect   = 1.0f;
    float bounce    = 0.6f;
    float aoRadius  = 1.6f;
};
// `--rt-boost[=low|high|max]` (goal 41): goal 38's effects turned up. Applies the preset's strength and tuning to `o`;
// false on an unknown level. "" = high.
[[nodiscard]] bool applyRtBoost(std::string_view level, RtOptions& o);
// "" (plain --rt) = ao,shadows; else a comma list of ao, shadows, gi, reflections, all. Null on an unknown word.
[[nodiscard]] std::optional<RtOptions> parseRtOptions(std::string_view s);

// Extra usage for geometry buffers that go into acceleration structures: 0 unless --rt is on (set by RayTracing).
VkBufferUsageFlags& rtGeometryUsage();

class RayTracing
{
public:
    RayTracing(Context& ctx, RtOptions opts);
    ~RayTracing();
    RayTracing(const RayTracing&)            = delete;
    RayTracing& operator=(const RayTracing&) = delete;

    [[nodiscard]] bool ok() const
    {
        return ok_;
    }
    [[nodiscard]] const RtOptions& options() const
    {
        return opts_;
    }

    // --- Per frame: the geometry ---
    // textureSlot: which bound texture set its sub-meshes sample (0 the zone, 1 the event sub-map, 2 none: props).
    void beginFrame(uint32_t slot);
    void addMeshes(const MeshRenderer& meshes, uint32_t textureSlot);
    // The characters skinned this frame (SkinnedRenderer::draw has run): skinned again into a buffer and built.
    void addActors(const SkinnedRenderer& skinned, const glm::vec3& eye, float range);
    // Builds the new BLASes, the characters' and the TLAS. Outside a render pass, before trace().
    void build(VkCommandBuffer cmd);
    // A renderer going away (zone change): its acceleration structures go with it.
    void forget(const void* owner);

    // --- The passes ---
    struct View
    {
        glm::mat4 viewProj{ 1.0f }, prevViewProj{ 1.0f }; // the jittered matrices the scene was drawn with
        glm::vec3 eye{ 0.0f };
        bool      reset = false; // camera cut / new zone: no history
        // How much light 0's traced shadow counts (1 outdoors). Indoors (the player stands on a floor the game marks
        // roofed) the zone's directional light is the room's fill light: shadowing it by the ceiling would darken
        // the whole room, so it fades to a hint.
        float     sunShadow = 1.0f;
        // The zone's level water (ZoneCollision::waterPlanes): pairs of {x0, z0, x1, z1}, {y, 0, 0, 0}, at most
        // kMaxWaterPlanes. The reflections mirror off these as well as off blended level zone meshes.
        const std::vector<glm::vec4>* water = nullptr;
    };
    static constexpr std::size_t kMaxWaterPlanes = 16;
    // Traces and denoises at the scene's resolution. depth: the scene depth in SHADER_READ_ONLY_OPTIMAL.
    void trace(VkCommandBuffer cmd, const Image& depth, VkExtent2D ext, const View& view, const SceneParams& sp, VkDescriptorSet sceneTextures,
               VkDescriptorSet subTextures, VkDescriptorSetLayout textureLayout);
    // Re-weights the scene colour (COLOR_ATTACHMENT_OPTIMAL, mip 0) with the traced terms; leaves it in that layout.
    void composite(VkCommandBuffer cmd, const Image& color, VkImageView colorMip0, const Image& depth, VkExtent2D ext, const View& view,
                   const SceneParams& sp);
    // --rt=reflections: blends the water reflection over the colour (COLOR_ATTACHMENT_OPTIMAL, mip 0), after the
    // effects (a harbour's sea is drawn by the effect pass). Call outside a rendering, after composite this frame.
    void reflect(VkCommandBuffer cmd, VkImageView colorMip0, VkExtent2D ext);

    // --dlss Ray Reconstruction denoises instead of the passes' own filter: the raw traced terms are composited and the
    // guides (diffuse / specular albedo, normal + roughness) made for DLSS-RR.
    void setRayReconstruction(bool on)
    {
        rr_ = on;
    }
    [[nodiscard]] bool rayReconstruction() const
    {
        return rr_;
    }
    [[nodiscard]] const Image& guideAlbedo() const
    {
        return guideAlbedo_;
    }
    [[nodiscard]] const Image& guideSpecular() const
    {
        return guideSpec_;
    }
    [[nodiscard]] const Image& guideNormal() const
    {
        return guideNormal_;
    }
    [[nodiscard]] bool anyEffect() const
    {
        return opts_.ao || opts_.shadows || opts_.points || opts_.gi || opts_.reflections;
    }

    // For --pathtrace (goal 41, PathTracer): this frame's TLAS and geometry records, after build().
    [[nodiscard]] VkAccelerationStructureKHR tlas() const
    {
        return frames_[slot_].tlas;
    }
    [[nodiscard]] VkDeviceAddress recordsAddress() const
    {
        return frames_[slot_].records.buffer ? ctx_.address(frames_[slot_].records) : 0;
    }

private:
    struct Blas
    {
        VkAccelerationStructureKHR as      = VK_NULL_HANDLE;
        Buffer                     buffer;
        VkDeviceAddress            address = 0;
        uint32_t                   records = 0; // first record (re-numbered on forget)
        uint32_t                   count   = 0;
        bool                       built   = false;
    };
    struct Record // std430, 32 bytes (rt_trace.comp)
    {
        VkDeviceAddress vertices = 0;
        VkDeviceAddress indices  = 0;
        uint32_t        texture  = 0;
        uint32_t        flags    = 0; // bit0 alpha test, bits 1-2 texture set, bit3 positions only (stride 12)
        uint32_t        stride   = 36;
        uint32_t        pad      = 0;
    };
    struct PendingBlas
    {
        Blas*                                                 blas = nullptr;
        std::vector<VkAccelerationStructureGeometryKHR>       geoms;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        VkDeviceSize                                          scratch = 0;
    };
    struct PerFrame
    {
        Buffer                                  instances, tlasBuffer, scratch, records, params, skinned, actorAs, actorScratch;
        VkAccelerationStructureKHR              tlas = VK_NULL_HANDLE;
        uint32_t                                tlasCapacity = 0;
        std::vector<VkAccelerationStructureKHR> actorBlas;
    };
    struct ActorJob
    {
        VkDeviceAddress vertices = 0, indices = 0, bones = 0;
        uint32_t        vertexCount = 0, boneBase = 0;
        std::vector<std::pair<uint32_t, uint32_t>> ranges; // first index, index count (opaque sub-meshes)
        std::vector<int32_t>                       offsets; // vertex offset per range
    };

    void            load();
    void            makePipelines();
    void            ensureImages(VkExtent2D ext);
    void            destroyImages();
    Blas*           blasFor(const MeshRenderer& meshes, uint32_t mesh, uint32_t textureSlot);
    void            pushInstance(const glm::mat4& m, VkDeviceAddress blas, uint32_t record, uint8_t mask, bool opaque);
    Buffer          make(VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    VkDeviceAddress address(const Buffer& b) const
    {
        return ctx_.address(b);
    }

    Context&  ctx_;
    RtOptions opts_;
    bool      ok_ = false;
    bool      rr_ = false;
    uint32_t  slot_ = 0;
    uint64_t  frame_ = 0;

    PFN_vkCreateAccelerationStructureKHR              createAs_  = nullptr;
    PFN_vkDestroyAccelerationStructureKHR             destroyAs_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR       sizes_     = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR           buildAs_   = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR    asAddress_ = nullptr;
    uint32_t                                          scratchAlign_ = 256;

    std::map<std::pair<const void*, uint32_t>, std::unique_ptr<Blas>> blas_;
    std::vector<Record>                                               records_;   // the static BLASes' geometry
    std::vector<Record>                                               frameRecords_; // + this frame's characters
    std::vector<PendingBlas>                                          pending_;
    std::vector<VkAccelerationStructureInstanceKHR>                   instances_;
    std::vector<ActorJob>                                             actors_;
    std::array<PerFrame, Context::kFramesInFlight>                    frames_{};
    std::vector<std::pair<VkAccelerationStructureKHR, Buffer>>        retired_; // freed once the GPU is idle (forget)
    std::vector<std::pair<uint64_t, Buffer>>                          scratchFree_; // BLAS build scratch, by frame

    // Images at the scene's resolution: raw trace, history (ping-pong), filtered; and a copy of the scene colour.
    VkExtent2D extent_{};
    Image      rawD_; // water reflections (rgb, weight): a mirror ray, no noise, no filter
    Image      rawA_, rawB_, rawC_, histA_[2], histB_[2], histC_[2], filtA_, filtB_, filtC_, sceneCopy_;
    Image      guideAlbedo_, guideSpec_, guideNormal_; // Ray Reconstruction only
    bool       historyValid_ = false;
    VkSampler  sampler_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout traceSetLayout_ = VK_NULL_HANDLE, denoiseSetLayout_ = VK_NULL_HANDLE, compSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout guidesSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      guidesLayout_    = VK_NULL_HANDLE;
    VkPipeline            guidesPipe_      = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, Context::kFramesInFlight> guidesSets_{};
    VkDescriptorPool      pool_           = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, Context::kFramesInFlight> traceSets_{}, compSets_{};
    std::array<VkDescriptorSet, 2 * Context::kFramesInFlight> denoiseSets_{}; // [slot * 2 + pass]
    VkPipelineLayout      traceLayout_ = VK_NULL_HANDLE, denoiseLayout_ = VK_NULL_HANDLE, compLayout_ = VK_NULL_HANDLE, skinLayout_ = VK_NULL_HANDLE;
    VkPipeline            tracePipe_   = VK_NULL_HANDLE, denoisePipe_ = VK_NULL_HANDLE, compPipe_ = VK_NULL_HANDLE, skinPipe_ = VK_NULL_HANDLE,
                          reflPipe_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureLayout_ = VK_NULL_HANDLE; // the layout trace() was made against
};

} // namespace xi::gfx
