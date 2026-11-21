// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/effect_renderer.h"

#include "gfx/pipeline.h"
#include "gfx/zone_scene.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>

namespace xi::gfx
{

namespace
{
struct Push
{
    glm::mat4       viewProj;
    VkDeviceAddress vertices;
};
static_assert(sizeof(Push) <= kClientPushConstantBytes); // Vulkan's guaranteed push-constant size (issue #139)
} // namespace

EffectRenderer::EffectRenderer(Context& ctx)
: ctx_(ctx)
, textures_(ctx)
{
    for (auto& b : buffers_)
    {
        b = ctx_.createBuffer(sizeof(Vertex) * kMaxVertices, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    }
    // Set 1: the two screen images (this frame's copy, the frozen one).
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(ctx_.device(), &sci, nullptr, &screenSampler_));
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 2;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dli{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dli.bindingCount = 1;
        dli.pBindings    = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx_.device(), &dli, nullptr, &screenLayout_));
        VkDescriptorPoolSize       ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets       = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes    = &ps;
        VK_CHECK(vkCreateDescriptorPool(ctx_.device(), &pci, nullptr, &screenPool_));
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool     = screenPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &screenLayout_;
        VK_CHECK(vkAllocateDescriptorSets(ctx_.device(), &ai, &screenSet_));
        ensureScreen({ 1, 1 });
    }
    VkPushConstantRange        pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push) };
    const std::array<VkDescriptorSetLayout, 2> sets{ textures_.layout(), screenLayout_ };
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 2;
    lci.pSetLayouts            = sets.data();
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcr;
    VK_CHECK(vkCreatePipelineLayout(ctx_.device(), &lci, nullptr, &layout_));

    const auto  dir  = shaderDir();
    PipelineDesc pd;
    pd.vert        = loadShader(ctx_.device(), dir / "fx.vert.spv");
    pd.frag        = loadShader(ctx_.device(), dir / "fx.frag.spv");
    pd.layout      = layout_;
    pd.colorFormat = ctx_.colorFormat();
    pd.depthFormat = VK_FORMAT_D32_SFLOAT;
    pd.cullMode    = VK_CULL_MODE_NONE;
    pd.alphaBlend  = true; // SRCALPHA / INVSRCALPHA (blend 0x44); no depth writes
    alpha_         = createPipeline(ctx_.device(), pd);
    layerAlpha_    = createPipeline(ctx_.device(), pd, false);
    VkPipelineColorBlendAttachmentState add{};
    add.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    add.blendEnable         = VK_TRUE;
    add.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; // blend 0x48: SRCALPHA / ONE
    add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    add.colorBlendOp        = VK_BLEND_OP_ADD;
    add.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    add.alphaBlendOp        = VK_BLEND_OP_ADD;
    pd.blend                = add;
    additive_               = createPipeline(ctx_.device(), pd);
    layerAdd_               = createPipeline(ctx_.device(), pd, false);
    add.colorBlendOp        = VK_BLEND_OP_REVERSE_SUBTRACT; // blend 0x41-0x43: dst - src * alpha
    pd.blend                = add;
    subtract_               = createPipeline(ctx_.device(), pd);
    add.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO; // ground shadows: ZERO / INVSRCCOLOR, biased toward the camera
    add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    add.colorBlendOp        = VK_BLEND_OP_ADD;
    pd.blend                = add;
    pd.depthBias            = true;
    pd.fragSpec0            = 1; // clamp UVs
    multiply_               = createPipeline(ctx_.device(), pd);
    vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
}

EffectRenderer::~EffectRenderer()
{
    ctx_.waitIdle();
    for (auto& b : buffers_)
    {
        ctx_.destroyBuffer(b);
    }
    vkDestroyPipeline(ctx_.device(), additive_, nullptr);
    vkDestroyPipeline(ctx_.device(), subtract_, nullptr);
    vkDestroyPipeline(ctx_.device(), alpha_, nullptr);
    vkDestroyPipeline(ctx_.device(), multiply_, nullptr);
    vkDestroyPipeline(ctx_.device(), layerAlpha_, nullptr);
    vkDestroyPipeline(ctx_.device(), layerAdd_, nullptr);
    for (auto& im : screen_)
    {
        ctx_.destroyImage(im);
    }
    vkDestroyDescriptorPool(ctx_.device(), screenPool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_.device(), screenLayout_, nullptr);
    vkDestroySampler(ctx_.device(), screenSampler_, nullptr);
    vkDestroyPipelineLayout(ctx_.device(), layout_, nullptr);
}

const Image& EffectRenderer::renderTexture(const dat::Texture* key, VkExtent2D extent)
{
    if (const auto it = renderTextures_.find(key); it != renderTextures_.end())
    {
        return it->second;
    }
    const Image img = ctx_.createImage(extent, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    textureIndex_[key] = static_cast<int>(textures_.addImage(img));
    textures_.commit();
    return renderTextures_[key] = img;
}

void EffectRenderer::prepare(const std::vector<client::EffectSystem::Batch>& batches)
{
    // This frame's new textures in one submit (one queue wait, not one each: goal 25's effect start-up frames).
    std::vector<const dat::Texture*> fresh;
    for (const auto& b : batches)
    {
        if (b.texture && !textureIndex_.contains(b.texture) && textures_.size() + fresh.size() < TextureSet::kMaxTextures &&
            std::ranges::find(fresh, b.texture) == fresh.end())
        {
            fresh.push_back(b.texture);
        }
    }
    if (fresh.empty())
    {
        return;
    }
    const auto ids = textures_.add(fresh);
    for (std::size_t i = 0; i < fresh.size(); ++i)
    {
        textureIndex_[fresh[i]] = static_cast<int>(ids[i]);
    }
    textures_.commit();
}

void EffectRenderer::draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const std::vector<client::EffectSystem::Batch>& batches)
{
    auto&    buf   = buffers_[frameSlot % buffers_.size()];
    auto*    out   = static_cast<Vertex*>(buf.mapped);
    uint32_t count = used_[frameSlot % used_.size()];
    // Four passes: ground shadows, subtractive (the client sorts them behind), normal alpha blending, then additive on top.
    using Blend = client::EffectSystem::Batch::Blend;
    static constexpr std::array<Blend, 4> kPass{ Blend::Multiply, Blend::Subtract, Blend::Alpha, Blend::Additive };
    std::array<std::pair<uint32_t, uint32_t>, 4> ranges{};
    for (int pass = 0; pass < 4; ++pass)
    {
        ranges[static_cast<std::size_t>(pass)].first = count;
        for (const auto& b : batches)
        {
            if (b.blend != kPass[static_cast<std::size_t>(pass)] || b.screen)
            {
                continue;
            }
            const auto it  = b.texture ? textureIndex_.find(b.texture) : textureIndex_.end();
            const float tex = it != textureIndex_.end() ? static_cast<float>(it->second) : -1.0f;
            for (const auto& v : b.vertices)
            {
                if (count >= kMaxVertices)
                {
                    break;
                }
                out[count++] = { glm::vec4(ZoneScene::gameToRender(v.pos), tex), v.color, glm::vec4(v.uv, 0, 0), v.stage };
            }
        }
        ranges[static_cast<std::size_t>(pass)].second = count - ranges[static_cast<std::size_t>(pass)].first;
    }
    const bool any                  = count > used_[frameSlot % used_.size()];
    used_[frameSlot % used_.size()] = count;
    if (!any)
    {
        return;
    }
    const std::array<VkDescriptorSet, 2> sets{ textures_.set(), screenSet_ };
    const Push p{ viewProj, ctx_.address(buf) };
    for (int pass = 0; pass < 4; ++pass)
    {
        const auto [first, n] = ranges[static_cast<std::size_t>(pass)];
        if (n == 0)
        {
            continue;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? multiply_ : pass == 1 ? subtract_ : pass == 2 ? alpha_ : additive_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(p), &p);
        vkCmdDraw(cmd, n, 1, first, 0);
    }
}

void EffectRenderer::ensureScreen(VkExtent2D extent)
{
    if (screen_[0].image && screenExtent_.width == extent.width && screenExtent_.height == extent.height)
    {
        return;
    }
    if (screen_[0].image)
    {
        ctx_.waitIdle(); // the set may be in use by frames in flight
    }
    for (auto& im : screen_)
    {
        ctx_.destroyImage(im);
        im = ctx_.createImage(extent, ctx_.colorFormat(), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    // Both copies start black and readable: the set binds the two, and the frozen one is only filled by a freeze
    // (validation: a bound image left in UNDEFINED layout).
    ctx_.immediateSubmit([&](VkCommandBuffer cmd) {
        const VkClearColorValue       black{};
        const VkImageSubresourceRange all{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (auto& im : screen_)
        {
            Context::transition(cmd, im.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            Context::transition(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });
    screenExtent_ = extent;
    frozenValid_  = false;
    std::array<VkDescriptorImageInfo, 2> infos{};
    for (std::size_t i = 0; i < 2; ++i)
    {
        infos[i] = { screenSampler_, screen_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    }
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet          = screenSet_;
    w.dstBinding      = 0;
    w.descriptorCount = 2;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = infos.data();
    vkUpdateDescriptorSets(ctx_.device(), 1, &w, 0, nullptr);
}

bool EffectRenderer::needsScreen(const std::vector<client::EffectSystem::Batch>& batches, const std::vector<client::EffectSystem::ScreenLayer>& layers,
                                 uint32_t freezeSerial) const
{
    return !layers.empty() || freezeSerial != frozenSerial_ || std::ranges::any_of(batches, [](const auto& b) { return b.screen && !b.vertices.empty(); });
}

void EffectRenderer::copyScene(VkCommandBuffer cmd, VkImage scene, VkExtent2D extent, uint32_t freezeSerial)
{
    ensureScreen(extent);
    const bool freeze = freezeSerial != frozenSerial_;
    Context::transition(cmd, scene, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    VkImageCopy region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent         = { extent.width, extent.height, 1 };
    for (std::size_t i = 0; i < (freeze ? 2u : 1u); ++i)
    {
        Context::transition(cmd, screen_[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyImage(cmd, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screen_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        Context::transition(cmd, screen_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (freeze)
    {
        frozenSerial_ = freezeSerial;
        frozenValid_  = true;
    }
    Context::transition(cmd, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
}

void EffectRenderer::drawScreen(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const std::vector<client::EffectSystem::Batch>& batches,
                                const std::vector<client::EffectSystem::ScreenLayer>& layers)
{
    auto&    buf   = buffers_[frameSlot % buffers_.size()];
    auto*    out   = static_cast<Vertex*>(buf.mapped);
    uint32_t count = used_[frameSlot % used_.size()];
    using Blend    = client::EffectSystem::Batch::Blend;
    // Screen batches by blend: alpha, subtract, additive.
    std::array<std::pair<uint32_t, uint32_t>, 3> ranges{};
    static constexpr std::array<Blend, 3>         kPass{ Blend::Alpha, Blend::Subtract, Blend::Additive };
    for (std::size_t pass = 0; pass < kPass.size(); ++pass)
    {
        ranges[pass].first = count;
        for (const auto& b : batches)
        {
            if (!b.screen || b.blend != kPass[pass])
            {
                continue;
            }
            for (const auto& v : b.vertices)
            {
                if (count >= kMaxVertices)
                {
                    break;
                }
                out[count++] = { glm::vec4(ZoneScene::gameToRender(v.pos), -2.0f), v.color, glm::vec4(v.uv, 0, 0), v.stage };
            }
        }
        ranges[pass].second = count - ranges[pass].first;
    }
    // Layers: two triangles over the target in clip space (identity matrix, z at the near plane), UV from the rect.
    const uint32_t layerFirst = count;
    std::vector<std::pair<bool, uint32_t>> layerDraws; // (additive, first vertex)
    for (const auto& l : layers)
    {
        if ((l.frozen && !frozenValid_) || count + 6 > kMaxVertices)
        {
            continue;
        }
        const float tex = !l.image ? -1.0f : l.frozen ? -3.0f : -4.0f;
        auto        v   = [&](float x, float y, float u, float w) { return Vertex{ glm::vec4(x, y, 1.0f, tex), l.colour, glm::vec4(u, w, 0, 0), glm::vec4(1.0f) }; };
        const auto& r   = l.uvRect;
        layerDraws.emplace_back(l.additive, count);
        for (const auto& vx : { v(-1, -1, r.x, r.y), v(1, -1, r.z, r.y), v(-1, 1, r.x, r.w), v(-1, 1, r.x, r.w), v(1, -1, r.z, r.y), v(1, 1, r.z, r.w) })
        {
            out[count++] = vx;
        }
    }
    (void)layerFirst;
    used_[frameSlot % used_.size()] = count;
    const std::array<VkDescriptorSet, 2> sets{ textures_.set(), screenSet_ };
    const Push                           p{ viewProj, ctx_.address(buf) };
    for (std::size_t pass = 0; pass < kPass.size(); ++pass)
    {
        const auto [first, n] = ranges[pass];
        if (n == 0)
        {
            continue;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? alpha_ : pass == 1 ? subtract_ : additive_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(p), &p);
        vkCmdDraw(cmd, n, 1, first, 0);
    }
    const Push ident{ glm::mat4(1.0f), ctx_.address(buf) };
    for (const auto& [additive, first] : layerDraws)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, additive ? layerAdd_ : layerAlpha_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ident), &ident);
        vkCmdDraw(cmd, 6, 1, first, 0);
    }
}

} // namespace xi::gfx
