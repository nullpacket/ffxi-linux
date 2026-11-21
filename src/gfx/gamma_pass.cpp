// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/gamma_pass.h"

#include "gfx/pipeline.h"

#include <SDL3/SDL.h>

#include <array>
#include <cmath>

namespace xi::gfx
{

GammaPass::GammaPass(Context& ctx)
: ctx_(ctx)
{
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter    = VK_FILTER_NEAREST;
    sci.minFilter    = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx_.device(), &sci, nullptr, &sampler_));
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dli.bindingCount = 1;
    dli.pBindings    = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_.device(), &dli, nullptr, &setLayout_));
    VkDescriptorPoolSize       ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets       = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &ps;
    VK_CHECK(vkCreateDescriptorPool(ctx_.device(), &pci, nullptr, &pool_));
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &setLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx_.device(), &ai, &set_));
    VkPushConstantRange        pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec4) };
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 1;
    lci.pSetLayouts            = &setLayout_;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcr;
    VK_CHECK(vkCreatePipelineLayout(ctx_.device(), &lci, nullptr, &layout_));
    const auto   dir  = shaderDir();
    PipelineDesc pd;
    pd.vert        = loadShader(ctx_.device(), dir / "gamma.vert.spv");
    pd.frag        = loadShader(ctx_.device(), dir / "gamma.frag.spv");
    pd.layout      = layout_;
    pd.colorFormat = ctx_.colorFormat();
    pd.depthFormat = VK_FORMAT_UNDEFINED;
    // An sRGB swap chain hands the shader linear values: it re-encodes before the ramp (the ramp works on the bytes).
    pd.fragSpec0 = ctx_.colorFormat() == VK_FORMAT_B8G8R8A8_SRGB || ctx_.colorFormat() == VK_FORMAT_R8G8B8A8_SRGB ? 1u : 0u;
    pipeline_    = createPipeline(ctx_.device(), pd, false);
    vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
}

GammaPass::~GammaPass()
{
    ctx_.waitIdle();
    ctx_.destroyImage(copy_);
    vkDestroyPipeline(ctx_.device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device(), layout_, nullptr);
    vkDestroyDescriptorPool(ctx_.device(), pool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_.device(), setLayout_, nullptr);
    vkDestroySampler(ctx_.device(), sampler_, nullptr);
}

void GammaPass::setValues(int r, int g, int b)
{
    exp_    = glm::vec4(exponent(r), exponent(g), exponent(b), 1.0f);
    active_ = r != 50 || g != 50 || b != 50;
}

void GammaPass::ensure(VkExtent2D extent)
{
    if (copy_.image && extent.width == extent_.width && extent.height == extent_.height)
    {
        return;
    }
    if (copy_.image)
    {
        ctx_.waitIdle(); // the set may be in use by frames in flight
        ctx_.destroyImage(copy_);
    }
    copy_   = ctx_.createImage(extent, ctx_.colorFormat(), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    extent_ = extent;
    VkDescriptorImageInfo info{ sampler_, copy_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet  w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet          = set_;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &info;
    vkUpdateDescriptorSets(ctx_.device(), 1, &w, 0, nullptr);
}

void GammaPass::apply(VkCommandBuffer cmd, VkImage image, VkImageView view, VkExtent2D extent)
{
    ensure(extent);
    Context::transition(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Context::transition(cmd, copy_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent         = { extent.width, extent.height, 1 };
    vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Context::transition(cmd, copy_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Context::transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView   = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea           = { { 0, 0 }, extent };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{ 0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1 };
    const VkRect2D   sc{ { 0, 0 }, extent };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(exp_), &exp_);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

} // namespace xi::gfx
