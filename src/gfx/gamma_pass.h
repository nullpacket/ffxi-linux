// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/context.h"

#include <glm/glm.hpp>

namespace xi::gfx
{

// Config Misc. "gamma" R / G / B (cfg 55 / 56 / 57, 0..100, default 50; config_misc_re.md §55-57). The game sets a
// fullscreen gamma ramp, ramp[i] = (i / 255)^(1 / g) with g = 1 + v / 100 (CODE 0x10016fd0 / 0x100170e0, applied with
// SetGammaRamp only when the device has D3DCAPS2_FULLSCREENGAMMA; windowed it greys the sliders). Vulkan has no
// ramp, so a last full-screen pass raises the finished frame (scene and interface, as a ramp would) to 1.5 / g per
// channel: the default 50 (g = 1.5) is the look everything here is tuned to, so it changes nothing (INFERRED).
class GammaPass
{
public:
    explicit GammaPass(Context& ctx);
    ~GammaPass();
    GammaPass(const GammaPass&)            = delete;
    GammaPass& operator=(const GammaPass&) = delete;

    // The Config values (0..100) per channel.
    void setValues(int r, int g, int b);
    [[nodiscard]] bool active() const
    {
        return active_;
    }
    // Outside rendering, `image` in COLOR_ATTACHMENT_OPTIMAL (TRANSFER_SRC usage): copies it, then redraws it through
    // the ramp. Leaves it in COLOR_ATTACHMENT_OPTIMAL.
    void apply(VkCommandBuffer cmd, VkImage image, VkImageView view, VkExtent2D extent);

    // The exponent for a Config value (tests).
    [[nodiscard]] static float exponent(int value)
    {
        return 1.5f / (1.0f + static_cast<float>(value) / 100.0f);
    }

private:
    void ensure(VkExtent2D extent);

    Context&              ctx_;
    Image                 copy_;
    VkExtent2D            extent_{};
    VkSampler             sampler_  = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_     = VK_NULL_HANDLE;
    VkDescriptorSet       set_      = VK_NULL_HANDLE;
    VkPipelineLayout      layout_   = VK_NULL_HANDLE;
    VkPipeline            pipeline_ = VK_NULL_HANDLE;
    glm::vec4             exp_{ 1.0f };
    bool                  active_ = false;
};

} // namespace xi::gfx
