// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/scene_target.h"

#include <algorithm>

namespace xi::gfx
{

SceneTarget::SceneTarget(Context& ctx)
: ctx_(ctx)
{
}

SceneTarget::~SceneTarget()
{
    destroy();
}

void SceneTarget::destroy()
{
    if (!color_.image)
    {
        return;
    }
    ctx_.waitIdle();
    vkDestroyImageView(ctx_.device(), mip0_, nullptr);
    ctx_.destroyImage(color_);
    ctx_.destroyImage(depth_);
    color_  = {};
    depth_  = {};
    mip0_   = VK_NULL_HANDLE;
    extent_ = {};
}

void SceneTarget::ensure(VkExtent2D extent)
{
    if (extent.width == extent_.width && extent.height == extent_.height)
    {
        return;
    }
    destroy();
    uint32_t mips = 1;
    for (uint32_t d = std::max(extent.width, extent.height); d > 1; d >>= 1)
    {
        ++mips;
    }
    const VkImageUsageFlags sampled = sampled_ ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT) : 0u;
    color_ = ctx_.createImage(extent, ctx_.colorFormat(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | sampled,
                              VK_IMAGE_ASPECT_COLOR_BIT, mips);
    depth_ = ctx_.createImage(extent, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | sampled, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image            = color_.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = ctx_.colorFormat();
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VK_CHECK(vkCreateImageView(ctx_.device(), &vci, nullptr, &mip0_));
    extent_ = extent;
}

void SceneTarget::resolve(VkCommandBuffer cmd, VkImage dst, VkExtent2D dstExtent)
{
    // Halve until the next halving would drop below the window size (box filter: 2x2 linear blits).
    int32_t  w = static_cast<int32_t>(extent_.width), h = static_cast<int32_t>(extent_.height);
    uint32_t level = 0;
    Context::transition(cmd, color_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    while (w / 2 >= static_cast<int32_t>(dstExtent.width) && h / 2 >= static_cast<int32_t>(dstExtent.height) && level + 1 < color_.mipLevels)
    {
        const int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        Context::transition(cmd, color_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, level + 1, 1);
        VkImageBlit b{};
        b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        b.srcOffsets[1]  = { w, h, 1 };
        b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level + 1, 0, 1 };
        b.dstOffsets[1]  = { nw, nh, 1 };
        vkCmdBlitImage(cmd, color_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        Context::transition(cmd, color_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, level + 1, 1);
        ++level;
        w = nw;
        h = nh;
    }
    Context::transition(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
    b.srcOffsets[1]  = { w, h, 1 };
    b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.dstOffsets[1]  = { static_cast<int32_t>(dstExtent.width), static_cast<int32_t>(dstExtent.height), 1 };
    vkCmdBlitImage(cmd, color_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    Context::transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

} // namespace xi::gfx
