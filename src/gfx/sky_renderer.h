// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/context.h"

#include <glm/glm.hpp>

#include <array>

namespace xi::gfx
{

// Sky background: an 8-stop vertical gradient (zone Weather record +0x6C colours / +0x8C positions, §9.2),
// stop 0 at the horizon. Drawn first, without depth.
class SkyRenderer
{
public:
    struct Gradient
    {
        std::array<glm::vec3, 8> colors{}; // linear 0..1 as stored (sRGB-encoded bytes / 255)
        std::array<float, 8>     positions{};
    };

    explicit SkyRenderer(Context& ctx);
    ~SkyRenderer();
    SkyRenderer(const SkyRenderer&)            = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;

    // Camera basis in render space and vertical field of view; aspect = width / height.
    void draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::vec3& forward, const glm::vec3& right, const glm::vec3& up, float fovY, float aspect,
              const Gradient& g);

private:
    struct GradientGpu
    {
        glm::vec4 colors[8];
        glm::vec4 positions[2];
    };
    Context&                                     ctx_;
    std::array<Buffer, Context::kFramesInFlight> buffers_;
    VkPipelineLayout                             layout_   = VK_NULL_HANDLE;
    VkPipeline                                   pipeline_ = VK_NULL_HANDLE;
};

} // namespace xi::gfx
