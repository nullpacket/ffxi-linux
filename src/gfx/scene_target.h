// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/context.h"

namespace xi::gfx
{

// FFXI's "background resolution": the 3D scene renders offscreen at its own size and is scaled to the window.
// Larger than the window = supersampling: the image is box-filtered down a mip chain until it is at most twice the
// window size, then blitted with linear filtering. Smaller = cheaper, softer.
class SceneTarget
{
public:
    explicit SceneTarget(Context& ctx);
    ~SceneTarget();
    SceneTarget(const SceneTarget&)            = delete;
    SceneTarget& operator=(const SceneTarget&) = delete;

    // (Re)creates the images when the size changes. Waits for the GPU when it has to.
    void ensure(VkExtent2D extent);
    // DLSS reads the colour and depth as sampled images (goal 15): adds SAMPLED usage to both. Off by default.
    void setSampled(bool on)
    {
        if (on != sampled_)
        {
            sampled_ = on;
            extent_  = {}; // the next ensure() remakes them
        }
    }

    [[nodiscard]] VkExtent2D extent() const
    {
        return extent_;
    }
    [[nodiscard]] VkImageView colorView() const
    {
        return mip0_;
    }
    [[nodiscard]] VkImage colorImage() const
    {
        return color_.image;
    }
    [[nodiscard]] const Image& color() const
    {
        return color_;
    }
    [[nodiscard]] const Image& depth() const
    {
        return depth_;
    }

    // After the scene pass (color mip 0 in COLOR_ATTACHMENT_OPTIMAL): scales into `dst` (any layout, its content is
    // replaced) and leaves `dst` in COLOR_ATTACHMENT_OPTIMAL for the interface pass.
    void resolve(VkCommandBuffer cmd, VkImage dst, VkExtent2D dstExtent);

private:
    void destroy();

    Context&    ctx_;
    VkExtent2D  extent_{};
    Image       color_;
    Image       depth_;
    VkImageView mip0_ = VK_NULL_HANDLE;
    bool        sampled_ = false;
};

} // namespace xi::gfx
