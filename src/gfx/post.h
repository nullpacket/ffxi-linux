// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Optional post-processing (`--post`, goal 41; docs/enhanced-graphics.md). Without the flag nothing here exists. With
// it, the finished scene (before the interface) is lifted into a linear HDR working buffer through the inverse of an
// ACES tone map, light is added there (volumetric light shafts / fog, bloom), and it is tone mapped back. With nothing
// added the round trip returns the same image: `--post=hdr` alone keeps the look; the other effects imply it.

#include "gfx/context.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace xi::gfx
{

struct PostOptions
{
    bool  hdr     = false;
    bool  bloom   = false;
    bool  godrays = false; // light shafts from light 0 (sun / moon): ray-queried with --rt, screen-space otherwise
    bool  fog     = false; // volumetric (height) fog lit by the zone's fog colour
    float bloomStrength = 0.14f;
    float shaftStrength = 1.0f;
    float fogDensity    = 1.0f;
};
// "" or "all" = every effect; else a comma list of hdr, bloom, godrays, fog. Null on an unknown word.
[[nodiscard]] std::optional<PostOptions> parsePostOptions(std::string_view s);

class PostProcess
{
public:
    // rayQuery: the device has ray queries on (--rt), so the light shafts can test the TLAS.
    PostProcess(Context& ctx, PostOptions opts, bool rayQuery);
    ~PostProcess();
    PostProcess(const PostProcess&)            = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    [[nodiscard]] const PostOptions& options() const
    {
        return opts_;
    }

    struct View
    {
        glm::mat4                  viewProj{ 1.0f }; // the matrix the depth was drawn with
        glm::vec3                  eye{ 0.0f };
        glm::vec3                  lightDir{ 0.0f, 1.0f, 0.0f }; // toward light 0
        glm::vec3                  lightColor{ 0.0f };
        glm::vec3                  ambient{ 0.0f };
        glm::vec3                  fogColor{ 0.5f };
        float                      fogEnd = 200.0f;
        VkAccelerationStructureKHR tlas   = VK_NULL_HANDLE; // --rt's TLAS this frame, or none
        uint32_t                   frame  = 0;
    };
    // Outside rendering. `image` (TRANSFER_SRC | DST usage, mip 0) is in `layout` and is left in it, its content
    // replaced by the processed scene. `depth` (SAMPLED usage) is in `depthLayout` and is left in it; its extent may
    // differ from the image's (DLSS: the depth is at the render size).
    void apply(VkCommandBuffer cmd, uint32_t slot, VkImage image, VkImageLayout layout, VkExtent2D ext, const Image& depth, VkImageLayout depthLayout,
               const View& view);

private:
    static constexpr int kBloomLevels = 5;
    struct Pass
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    void ensure(VkExtent2D ext, const Image& depth);
    void destroyImages();
    void dispatch(VkCommandBuffer cmd, VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set, VkExtent2D size, const struct PostPush& push);

    Context&    ctx_;
    PostOptions opts_;
    bool        rayQuery_ = false;
    VkExtent2D  extent_{};
    VkImageView depthView_ = VK_NULL_HANDLE;
    Image       work_, vol_;
    std::array<Image, kBloomLevels> bloom_{};

    VkSampler             linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE, rqSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      layout_ = VK_NULL_HANDLE, rqLayout_ = VK_NULL_HANDLE;
    VkPipeline            volume_ = VK_NULL_HANDLE, volumeRq_ = VK_NULL_HANDLE, rays_ = VK_NULL_HANDLE, prep_ = VK_NULL_HANDLE,
               down_ = VK_NULL_HANDLE, up_ = VK_NULL_HANDLE, final_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet  volumeSet_ = VK_NULL_HANDLE, raysSet_ = VK_NULL_HANDLE, prepSet_ = VK_NULL_HANDLE, finalSet_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kBloomLevels>              downSets_{}, upSets_{};
    std::array<VkDescriptorSet, Context::kFramesInFlight>  volumeRqSets_{};
    std::array<Buffer, Context::kFramesInFlight>           view_{}; // PostView per frame in flight (post_common.glsl)
};

} // namespace xi::gfx
