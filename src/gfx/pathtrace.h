// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Optional path-traced lighting (`--pathtrace`, goal 41 option 3; docs/enhanced-graphics.md). Without the flag nothing
// here exists. With it, the zones' baked vertex lighting is set aside and each pixel is lit by tracing: the sun / moon
// (the game's light 0, and light 1) with shadow rays, the sky (the zone's sky gradient at the game's ambient level),
// the zone's point lights (lamps, fires) with shadow rays, and 2-3 diffuse bounces. The geometry is RayTracing's
// (goal 38): its TLAS and records are shared, so --pathtrace makes a RayTracing too. A temporal + a-trous filter
// denoises, or DLSS Ray Reconstruction when --dlss is on. Fog and the effects pass stay the game's, on top.

#include "gfx/context.h"
#include "gfx/mesh_renderer.h"
#include "gfx/sky_renderer.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace xi::gfx
{

class RayTracing;

struct PtOptions
{
    int   bounces  = 2;    // diffuse bounces after the first hit (1..3)
    float blend    = 1.0f; // 0 = the game's baked look, 1 = path traced
    float exposure = 0.0f; // EV: the traced light x 2^exposure
    // How much of the vertex colours' baked shading (their luminance) is kept in the albedo: 0 = none (the traced
    // light does all the shading), 1 = all (as the game). Zone vertex colours are near grey: baked shade, not tint.
    float baked = 0.3f;
};
// "" (plain --pathtrace) = the defaults; else "N" bounces. Null on anything else.
[[nodiscard]] std::optional<PtOptions> parsePtOptions(std::string_view s);

class PathTracer
{
public:
    PathTracer(Context& ctx, RayTracing& rt, PtOptions opts);
    ~PathTracer();
    PathTracer(const PathTracer&)            = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    [[nodiscard]] const PtOptions& options() const
    {
        return opts_;
    }

    struct View
    {
        glm::mat4 viewProj{ 1.0f }, prevViewProj{ 1.0f }; // as the scene was drawn (jittered with --dlss)
        glm::vec3 eye{ 0.0f };
        bool      reset = false; // camera cut / new zone
        // How much the directional lights' traced shadows count (1 outdoors). Indoors (the player on a floor the game
        // marks roofed) light 0 is the room's fill light: shadowed by the ceiling it would darken the room (goal 38).
        float     sunShadow = 1.0f;
    };
    // After RayTracing::build this frame. depth: SHADER_READ_ONLY_OPTIMAL; color: the scene colour (mip 0 a colour
    // attachment), re-lit in place and left in COLOR_ATTACHMENT_OPTIMAL.
    void render(VkCommandBuffer cmd, uint32_t slot, const Image& color, VkImageView colorMip0, const Image& depth, VkExtent2D ext, const View& view,
                const SceneParams& sp, const SkyRenderer::Gradient& sky, VkDescriptorSet sceneTextures, VkDescriptorSet subTextures);

    // --dlss Ray Reconstruction denoises instead of the filter: the noisy result is composited, the guides made.
    void setRayReconstruction(bool on)
    {
        rr_ = on;
    }
    [[nodiscard]] bool rayReconstruction() const
    {
        return rr_;
    }
    // --post tone maps the scene (ACES, inverse on the way in): the composite then leaves the highlights' roll-off to it
    // and only clips at the inverse's limit instead of applying its own shoulder.
    void setToneMapped(bool on)
    {
        toneMapped_ = on;
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

private:
    void makePipelines();
    void ensureImages(VkExtent2D ext);
    void destroyImages();

    Context&    ctx_;
    RayTracing& rt_;
    PtOptions   opts_;
    bool        rr_    = false;
    bool        toneMapped_ = false;
    uint64_t    frame_ = 0;

    std::array<Buffer, Context::kFramesInFlight> params_{};
    Buffer     exposure_; // pt_expose.comp's automatic exposure (host-visible, starts at 0 = "take the first target")
    VkExtent2D extent_{};
    Image      rawE_, albedo_, aux_, histE_[2], histAux_[2], filtA_, filtB_, sceneCopy_;
    Image      guideAlbedo_, guideSpec_, guideNormal_;
    bool       historyValid_ = false;
    VkSampler  nearest_      = VK_NULL_HANDLE;

    VkDescriptorSetLayout textureLayout_ = VK_NULL_HANDLE, setLayout_ = VK_NULL_HANDLE, compSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_          = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, Context::kFramesInFlight> sets_{}, compSets_{};
    VkPipelineLayout traceLayout_ = VK_NULL_HANDLE, denoiseLayout_ = VK_NULL_HANDLE, compLayout_ = VK_NULL_HANDLE;
    VkPipeline       tracePipe_ = VK_NULL_HANDLE, denoisePipe_ = VK_NULL_HANDLE, compPipe_ = VK_NULL_HANDLE, exposePipe_ = VK_NULL_HANDLE;
};

} // namespace xi::gfx
