// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/context.h"

#include <glm/glm.hpp>

#include <array>

namespace xi::gfx
{

// DLSS's motion vectors (goal 15): an R16G16_SFLOAT image at the render size, previous minus current position in
// render pixels. drawCamera() fills every pixel from the depth and the camera's motion; moving objects then draw over
// it with their own motion (SkinnedRenderer::drawMotion). Only made with --dlss.
class MotionVectors
{
public:
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16_SFLOAT;

    explicit MotionVectors(Context& ctx);
    ~MotionVectors();
    MotionVectors(const MotionVectors&)            = delete;
    MotionVectors& operator=(const MotionVectors&) = delete;

    void ensure(VkExtent2D extent);
    [[nodiscard]] const Image& image() const
    {
        return image_;
    }

    // The per-frame block motion_skinned.vert reads (both unjittered matrices and the size); its device address.
    VkDeviceAddress frameBlock(uint32_t frameSlot, const glm::mat4& cur, const glm::mat4& prev);

    // Outside rendering. `depth` is in DEPTH_ATTACHMENT_OPTIMAL and stays so; the image ends in COLOR_ATTACHMENT_OPTIMAL.
    void drawCamera(VkCommandBuffer cmd, const Image& depth, const glm::mat4& jitteredViewProj, const glm::mat4& prevViewProj, glm::vec2 jitterNdc);
    // Inside rendering to a colour target of the context's format (not written) and a D32 depth (cleared to 0): that
    // depth becomes `depth` (render size, SHADER_READ_ONLY_OPTIMAL, the one drawCamera() last read) scaled to
    // `window`, for the effects drawn over DLSS's output with Ray Reconstruction (issue #101). `jitterPx` is this
    // frame's jitter in render pixels.
    void drawDepth(VkCommandBuffer cmd, const Image& depth, VkExtent2D window, glm::vec2 jitterPx);

private:
    void destroyImage();

    Context&                                     ctx_;
    Image                                        image_;
    VkSampler                                    sampler_   = VK_NULL_HANDLE;
    VkDescriptorSetLayout                        setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool                             pool_      = VK_NULL_HANDLE;
    VkDescriptorSet                              set_       = VK_NULL_HANDLE;
    VkImageView                                  boundDepth_ = VK_NULL_HANDLE;
    VkPipelineLayout                             layout_    = VK_NULL_HANDLE;
    VkPipeline                                   pipeline_  = VK_NULL_HANDLE;
    VkPipeline                                   depthPipeline_ = VK_NULL_HANDLE; // drawDepth()
    std::array<Buffer, Context::kFramesInFlight> blocks_{};
};

} // namespace xi::gfx
