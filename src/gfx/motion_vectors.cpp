// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/motion_vectors.h"

#include "gfx/pipeline.h"

#include <algorithm>

namespace xi::gfx
{

namespace
{

struct CameraPush
{
    glm::mat4 reproject; // prev * inverse(jittered), multiplied in double precision
    glm::vec4 jitter;
};
static_assert(sizeof(CameraPush) <=
              kClientPushConstantBytes); // Vulkan's guaranteed push-constant size (AMD's driver reports 128)

// motion_skinned.vert `Motion`.
struct MotionBlock
{
    glm::mat4 cur, prev;
    glm::vec4 size;
};

} // namespace

MotionVectors::MotionVectors(Context& ctx)
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
    VkPushConstantRange        pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CameraPush) };
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 1;
    lci.pSetLayouts            = &setLayout_;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcr;
    VK_CHECK(vkCreatePipelineLayout(ctx_.device(), &lci, nullptr, &layout_));
    const auto   dir = shaderDir();
    PipelineDesc pd;
    pd.vert        = loadShader(ctx_.device(), dir / "gamma.vert.spv"); // the full-screen triangle
    pd.frag        = loadShader(ctx_.device(), dir / "motion_camera.frag.spv");
    pd.layout      = layout_;
    pd.colorFormat = kFormat;
    pd.depthFormat = VK_FORMAT_UNDEFINED;
    pipeline_      = createPipeline(ctx_.device(), pd, false);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
    pd.frag        = loadShader(ctx_.device(), dir / "depth_upscale.frag.spv");
    pd.colorFormat = ctx_.colorFormat();
    pd.depthFormat = VK_FORMAT_D32_SFLOAT;
    pd.blend       = VkPipelineColorBlendAttachmentState{}; // depth only: no colour channel written
    depthPipeline_ = createPipeline(ctx_.device(), pd);     // depth test (all pass over the cleared 0) and writes
    vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
    for (auto& blk : blocks_)
    {
        blk = ctx_.createBuffer(sizeof(MotionBlock), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    }
}

MotionVectors::~MotionVectors()
{
    ctx_.waitIdle();
    destroyImage();
    for (auto& blk : blocks_)
    {
        ctx_.destroyBuffer(blk);
    }
    vkDestroyPipeline(ctx_.device(), pipeline_, nullptr);
    vkDestroyPipeline(ctx_.device(), depthPipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device(), layout_, nullptr);
    vkDestroyDescriptorPool(ctx_.device(), pool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_.device(), setLayout_, nullptr);
    vkDestroySampler(ctx_.device(), sampler_, nullptr);
}

void MotionVectors::destroyImage()
{
    if (image_.image)
    {
        ctx_.destroyImage(image_);
    }
    image_ = {};
}

void MotionVectors::ensure(VkExtent2D extent)
{
    if (image_.image && image_.extent.width == extent.width && image_.extent.height == extent.height)
    {
        return;
    }
    ctx_.waitIdle();
    destroyImage();
    image_ = ctx_.createImage(extent, kFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

VkDeviceAddress MotionVectors::frameBlock(uint32_t frameSlot, const glm::mat4& cur, const glm::mat4& prev)
{
    auto& blk = blocks_[frameSlot % blocks_.size()];
    *static_cast<MotionBlock*>(blk.mapped) = { cur, prev, glm::vec4(static_cast<float>(image_.extent.width), static_cast<float>(image_.extent.height), 0, 0) };
    return ctx_.address(blk);
}

void MotionVectors::drawCamera(VkCommandBuffer cmd, const Image& depth, const glm::mat4& jitteredViewProj, const glm::mat4& prevViewProj, glm::vec2 jitterNdc)
{
    if (boundDepth_ != depth.view)
    {
        ctx_.waitIdle(); // the set may be in use by a frame in flight (only on a resize)
        VkDescriptorImageInfo info{ sampler_, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet  w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet          = set_;
        w.dstBinding      = 0;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo      = &info;
        vkUpdateDescriptorSets(ctx_.device(), 1, &w, 0, nullptr);
        boundDepth_ = depth.view;
    }
    Context::transition(cmd, depth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    Context::transition(cmd, image_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView   = image_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea           = { { 0, 0 }, image_.extent };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{ 0, 0, static_cast<float>(image_.extent.width), static_cast<float>(image_.extent.height), 0, 1 };
    const VkRect2D   sc{ { 0, 0 }, image_.extent };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    const CameraPush p{ glm::mat4(glm::dmat4(prevViewProj) * glm::inverse(glm::dmat4(jitteredViewProj))),
                        glm::vec4(jitterNdc, 0, 0) };
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    Context::transition(cmd, depth.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void MotionVectors::drawDepth(VkCommandBuffer cmd, const Image& depth, VkExtent2D window, glm::vec2 jitterPx)
{
    if (boundDepth_ != depth.view)
    {
        return; // the set holds the depth drawCamera() read; without that there is nothing to scale
    }
    const glm::vec4 p(static_cast<float>(image_.extent.width) / static_cast<float>(std::max(1u, window.width)),
                      static_cast<float>(image_.extent.height) / static_cast<float>(std::max(1u, window.height)),
                      jitterPx);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace xi::gfx
