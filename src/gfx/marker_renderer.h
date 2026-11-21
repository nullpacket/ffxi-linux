// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/context.h"

#include <glm/glm.hpp>

#include <array>
#include <vector>

namespace xi::gfx
{

// Instanced coloured boxes for entities until character models exist. Instances are rewritten each frame.
class MarkerRenderer
{
public:
    struct Marker
    {
        glm::vec3 feet;           // render space
        float     height = 1.8f;
        glm::vec4 color{ 1.0f };
        float     width  = 0.6f;
    };

    static constexpr uint32_t kMaxMarkers = 4096;

    explicit MarkerRenderer(Context& ctx);
    ~MarkerRenderer();
    MarkerRenderer(const MarkerRenderer&)            = delete;
    MarkerRenderer& operator=(const MarkerRenderer&) = delete;

    // frameSlot: 0..Context::kFramesInFlight-1, so a buffer still in use by the GPU is never overwritten.
    void draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const std::vector<Marker>& markers);

private:
    struct Instance
    {
        glm::vec4 feetHeight; // xyz feet, w height
        glm::vec4 color;
        glm::vec4 size;       // x width
    };

    Context&                                         ctx_;
    std::array<Buffer, Context::kFramesInFlight>     buffers_;
    VkPipelineLayout                                 layout_   = VK_NULL_HANDLE;
    VkPipeline                                       pipeline_ = VK_NULL_HANDLE;
};

} // namespace xi::gfx
