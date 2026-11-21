// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/pathtrace.h"

#include "gfx/pipeline.h"
#include "gfx/raytracing.h"
#include "gfx/texture_set.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace xi::gfx
{

namespace
{

// pt_common.glsl's PtParams (std430).
struct ParamsGpu
{
    glm::mat4  invViewProj;
    glm::mat4  viewProj;
    glm::mat4  prevViewProj;
    glm::vec4  eye; // w = frame
    glm::vec4  light0Dir;
    glm::vec4  light0Color;
    glm::vec4  light1Dir;
    glm::vec4  light1Color;
    glm::vec4  ambient;     // w = the baked shading kept
    glm::vec4  fogColorEnd; // w = fog end
    glm::vec4  fogStart;    // x fog start, y exposure multiplier, z blend, w the directional lights' shadow weight
    glm::uvec4 opts;        // x bounces, y point lights, z frame, w bit0 reset, bit1 Ray Reconstruction, bit2 --post tone maps
    glm::vec4  size;        // xy pixels, zw 1 / pixels
    uint64_t   records;
    uint64_t   exposure; // the automatic exposure buffer
    glm::vec4  sky[8];      // the sky gradient's colours
    glm::vec4  skyPos[2];   // and their elevations
    glm::vec4  skyNorm;     // x = 1 / the gradient's mean luma over the upper hemisphere
    glm::vec4  points[2 * SceneParams::kMaxPointLights];
};

constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

VkPipeline computePipeline(VkDevice d, VkPipelineLayout layout, const char* name)
{
    VkShaderModule              m = loadShader(d, shaderDir() / name);
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName  = "main";
    ci.layout       = layout;
    VkPipeline p    = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    vkDestroyShaderModule(d, m, nullptr);
    return p;
}

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA)
{
    VkMemoryBarrier2 b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    b.srcStageMask  = src;
    b.srcAccessMask = srcA;
    b.dstStageMask  = dst;
    b.dstAccessMask = dstA;
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

// The sky gradient's colour at elevation e (0 horizon .. 1 zenith), as sky.frag.
glm::vec3 skyAt(const SkyRenderer::Gradient& g, float e)
{
    glm::vec3 c = g.colors[0];
    for (std::size_t i = 0; i + 1 < g.colors.size(); ++i)
    {
        if (e >= g.positions[i] && e <= g.positions[i + 1])
        {
            const float span = std::max(g.positions[i + 1] - g.positions[i], 1e-4f);
            c                = glm::mix(g.colors[i], g.colors[i + 1], (e - g.positions[i]) / span);
        }
    }
    if (e > g.positions[7])
    {
        c = g.colors[7];
    }
    return c;
}

// XI_PT_SKIP=trace,denoise,composite: leave parts out (profiling).
bool skip(const char* part)
{
    static const std::string s = std::getenv("XI_PT_SKIP") ? std::getenv("XI_PT_SKIP") : "";
    return s.find(part) != std::string::npos;
}

} // namespace

std::optional<PtOptions> parsePtOptions(std::string_view s)
{
    PtOptions o;
    if (s.empty())
    {
        return o;
    }
    if (s.size() == 1 && s[0] >= '1' && s[0] <= '3')
    {
        o.bounces = s[0] - '0';
        return o;
    }
    return std::nullopt;
}

PathTracer::PathTracer(Context& ctx, RayTracing& rt, PtOptions opts)
: ctx_(ctx)
, rt_(rt)
, opts_(opts)
{
    for (auto& p : params_)
    {
        p = ctx_.createBuffer(sizeof(ParamsGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    }
    exposure_ = ctx_.createBuffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    const float start[4] = { 0.0f, 0.0f, -1.0f, 0.0f }; // exposure (0 = take the first target), its target, sky share (unknown)
    std::memcpy(exposure_.mapped, start, sizeof(start));
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx_.device(), &si, nullptr, &nearest_));
    spdlog::info("pathtrace: on ({} bounces, blend {:.2f}, exposure {:+.1f} EV, baked shading kept {:.2f})", opts_.bounces, opts_.blend, opts_.exposure, opts_.baked);
}

PathTracer::~PathTracer()
{
    ctx_.waitIdle();
    const VkDevice d = ctx_.device();
    destroyImages();
    for (auto& p : params_)
    {
        ctx_.destroyBuffer(p);
    }
    ctx_.destroyBuffer(exposure_);
    vkDestroySampler(d, nearest_, nullptr);
    for (auto p : { tracePipe_, denoisePipe_, compPipe_, exposePipe_ })
    {
        vkDestroyPipeline(d, p, nullptr);
    }
    for (auto l : { traceLayout_, denoiseLayout_, compLayout_ })
    {
        vkDestroyPipelineLayout(d, l, nullptr);
    }
    for (auto l : { setLayout_, compSetLayout_, textureLayout_ })
    {
        vkDestroyDescriptorSetLayout(d, l, nullptr);
    }
    vkDestroyDescriptorPool(d, pool_, nullptr);
}

void PathTracer::destroyImages()
{
    for (Image* i : { &rawE_, &albedo_, &aux_, &histE_[0], &histE_[1], &histAux_[0], &histAux_[1], &filtA_, &filtB_, &sceneCopy_, &guideAlbedo_, &guideSpec_,
                      &guideNormal_ })
    {
        if (i->image)
        {
            ctx_.destroyImage(*i);
            *i = {};
        }
    }
    extent_ = {};
}

void PathTracer::ensureImages(VkExtent2D ext)
{
    if (ext.width == extent_.width && ext.height == extent_.height)
    {
        return;
    }
    ctx_.waitIdle();
    destroyImages();
    constexpr VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (Image* i : { &rawE_, &albedo_, &aux_, &histE_[0], &histE_[1], &histAux_[0], &histAux_[1], &filtA_, &filtB_, &guideNormal_ })
    {
        *i = ctx_.createImage(ext, kFormat, u, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    guideAlbedo_ = ctx_.createImage(ext, VK_FORMAT_R8G8B8A8_UNORM, u, VK_IMAGE_ASPECT_COLOR_BIT);
    guideSpec_   = ctx_.createImage(ext, VK_FORMAT_R8G8B8A8_UNORM, u, VK_IMAGE_ASPECT_COLOR_BIT);
    sceneCopy_   = ctx_.createImage(ext, ctx_.colorFormat(), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    ctx_.immediateSubmit([&](VkCommandBuffer cmd) {
        for (Image* i : { &rawE_, &albedo_, &aux_, &histE_[0], &histE_[1], &histAux_[0], &histAux_[1], &filtA_, &filtB_, &guideNormal_, &guideAlbedo_, &guideSpec_ })
        {
            Context::transition(cmd, i->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        }
        Context::transition(cmd, sceneCopy_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    extent_       = ext;
    historyValid_ = false;
}

void PathTracer::makePipelines()
{
    if (tracePipe_ != VK_NULL_HANDLE)
    {
        return;
    }
    const VkDevice d = ctx_.device();
    constexpr auto C = VK_SHADER_STAGE_COMPUTE_BIT;
    {
        std::vector<VkDescriptorSetLayoutBinding> b;
        b.push_back({ 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, C, nullptr });
        b.push_back({ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, nullptr });
        for (uint32_t i = 2; i <= 13; ++i)
        {
            b.push_back({ i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr });
        }
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = static_cast<uint32_t>(b.size());
        ci.pBindings    = b.data();
        VK_CHECK(vkCreateDescriptorSetLayout(d, &ci, nullptr, &setLayout_));
        std::vector<VkDescriptorSetLayoutBinding> cb;
        for (uint32_t i = 0; i < 5; ++i)
        {
            cb.push_back({ i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
        }
        ci.bindingCount = static_cast<uint32_t>(cb.size());
        ci.pBindings    = cb.data();
        VK_CHECK(vkCreateDescriptorSetLayout(d, &ci, nullptr, &compSetLayout_));
    }
    {
        // The texture sets' layout, as TextureSet defines it (any zone's sets are compatible with it).
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = TextureSet::kMaxTextures;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                               VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bfi.bindingCount  = 1;
        bfi.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo tci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        tci.pNext        = &bfi;
        tci.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        tci.bindingCount = 1;
        tci.pBindings    = &binding;
        VK_CHECK(vkCreateDescriptorSetLayout(d, &tci, nullptr, &textureLayout_));
    }
    const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4 },
                                           { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32 },
                                           { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 } };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets       = 8;
    pci.poolSizeCount = 3;
    pci.pPoolSizes    = sizes;
    VK_CHECK(vkCreateDescriptorPool(d, &pci, nullptr, &pool_));
    auto alloc = [&](VkDescriptorSetLayout l) {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool     = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &l;
        VkDescriptorSet s     = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(d, &ai, &s));
        return s;
    };
    for (auto& s : sets_)
    {
        s = alloc(setLayout_);
    }
    for (auto& s : compSets_)
    {
        s = alloc(compSetLayout_);
    }
    VkPushConstantRange        pr{ C, 0, 16 };
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    const VkDescriptorSetLayout traceSets[] = { setLayout_, textureLayout_, textureLayout_ };
    lci.setLayoutCount         = 3;
    lci.pSetLayouts            = traceSets;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pr;
    VK_CHECK(vkCreatePipelineLayout(d, &lci, nullptr, &traceLayout_));
    tracePipe_         = computePipeline(d, traceLayout_, "pt_trace.comp.spv");
    lci.setLayoutCount = 1;
    lci.pSetLayouts    = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(d, &lci, nullptr, &denoiseLayout_));
    denoisePipe_ = computePipeline(d, denoiseLayout_, "pt_denoise.comp.spv");
    exposePipe_  = computePipeline(d, denoiseLayout_, "pt_expose.comp.spv");
    VkPushConstantRange fr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(VkDeviceAddress) }; // the params address (all of it is set: #139)
    lci.pSetLayouts         = &compSetLayout_;
    lci.pPushConstantRanges = &fr;
    VK_CHECK(vkCreatePipelineLayout(d, &lci, nullptr, &compLayout_));
    PipelineDesc pd;
    pd.vert        = loadShader(d, shaderDir() / "rt_composite.vert.spv");
    pd.frag        = loadShader(d, shaderDir() / "pt_composite.frag.spv");
    pd.layout      = compLayout_;
    pd.colorFormat = ctx_.colorFormat();
    pd.depthFormat = VK_FORMAT_UNDEFINED;
    compPipe_      = createPipeline(d, pd, false);
    vkDestroyShaderModule(d, pd.vert, nullptr);
    vkDestroyShaderModule(d, pd.frag, nullptr);
}

void PathTracer::render(VkCommandBuffer cmd, uint32_t slot, const Image& color, VkImageView colorMip0, const Image& depth, VkExtent2D ext, const View& view,
                        const SceneParams& sp, const SkyRenderer::Gradient& sky, VkDescriptorSet sceneTextures, VkDescriptorSet subTextures)
{
    const VkAccelerationStructureKHR tlas = rt_.tlas();
    if (tlas == VK_NULL_HANDLE || rt_.recordsAddress() == 0)
    {
        return;
    }
    makePipelines();
    ensureImages(ext);
    slot %= Context::kFramesInFlight;
    const VkDevice d   = ctx_.device();
    const uint32_t cur = static_cast<uint32_t>(frame_ & 1u), prev = cur ^ 1u;
    const bool     reset = view.reset || !historyValid_;

    auto* p         = static_cast<ParamsGpu*>(params_[slot].mapped);
    p->invViewProj  = glm::inverse(view.viewProj);
    p->viewProj     = view.viewProj;
    p->prevViewProj = view.prevViewProj;
    p->eye          = glm::vec4(view.eye, static_cast<float>(frame_ & 0xFFFF));
    p->light0Dir    = glm::vec4(glm::normalize(sp.light0Dir), 0.0f);
    p->light0Color  = glm::vec4(sp.light0Color, 0.0f);
    p->light1Dir    = glm::vec4(glm::normalize(sp.light1Dir), 0.0f);
    p->light1Color  = glm::vec4(sp.light1Color, 0.0f);
    p->ambient      = glm::vec4(sp.ambient, opts_.baked);
    p->fogColorEnd  = glm::vec4(sp.fogColor, sp.fogEnd);
    p->fogStart     = glm::vec4(sp.fogStart, std::exp2(opts_.exposure), opts_.blend, view.sunShadow);
    p->opts         = glm::uvec4(static_cast<uint32_t>(std::clamp(opts_.bounces, 1, 3)), static_cast<uint32_t>(std::min(sp.pointLights.size(), SceneParams::kMaxPointLights)),
                                 static_cast<uint32_t>(frame_), (reset ? 1u : 0u) | (rr_ ? 2u : 0u) | (toneMapped_ ? 4u : 0u));
    p->size         = glm::vec4(ext.width, ext.height, 1.0f / static_cast<float>(ext.width), 1.0f / static_cast<float>(ext.height));
    p->records      = rt_.recordsAddress();
    p->exposure     = ctx_.address(exposure_);
    // The sky: the gradient (sky.frag's colours) scaled so that, open to the whole upper sky, a surface gets the
    // game's ambient: its mean luma over the hemisphere (cosine-weighted, as a diffuse surface sees it) is divided out.
    double sum = 0.0, wsum = 0.0;
    for (int i = 0; i < 32; ++i)
    {
        const float e = (static_cast<float>(i) + 0.5f) / 32.0f; // elevation 0..1 (0..90 degrees)
        const float a = e * 1.5707963f;
        const float w = std::sin(a) * std::cos(a);              // cosine-weighted solid angle of the band
        const glm::vec3 c = skyAt(sky, e);
        sum += w * (0.299 * c.r + 0.587 * c.g + 0.114 * c.b);
        wsum += w;
    }
    const float meanLuma = static_cast<float>(sum / std::max(wsum, 1e-6));
    for (std::size_t i = 0; i < 8; ++i)
    {
        p->sky[i] = glm::vec4(sky.colors[i], 0.0f);
    }
    p->skyPos[0] = glm::vec4(sky.positions[0], sky.positions[1], sky.positions[2], sky.positions[3]);
    p->skyPos[1] = glm::vec4(sky.positions[4], sky.positions[5], sky.positions[6], sky.positions[7]);
    p->skyNorm   = glm::vec4(meanLuma > 0.01f ? 1.0f / meanLuma : 0.0f, 0.0f, 0.0f, 0.0f);
    for (std::size_t i = 0; i < SceneParams::kMaxPointLights; ++i)
    {
        const auto l         = i < sp.pointLights.size() ? sp.pointLights[i] : SceneParams::PointLight{};
        p->points[2 * i]     = glm::vec4(l.pos, l.range);
        p->points[2 * i + 1] = glm::vec4(l.colour, 0.0f);
    }
    const VkDeviceAddress params = ctx_.address(params_[slot]);

    // Descriptors (this slot's set: its previous use has finished).
    {
        VkWriteDescriptorSetAccelerationStructureKHR asw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
        asw.accelerationStructureCount = 1;
        asw.pAccelerationStructures    = &tlas;
        VkDescriptorImageInfo depthInfo{ nearest_, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const Image*          st[12] = { &rawE_, &albedo_, &aux_, &histE_[cur], &histE_[prev], &histAux_[cur], &histAux_[prev], &filtA_, &filtB_, &guideAlbedo_,
                                         &guideNormal_, &guideSpec_ };
        VkDescriptorImageInfo infos[12]{};
        VkWriteDescriptorSet  w[14]{};
        for (auto& x : w)
        {
            x.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            x.dstSet          = sets_[slot];
            x.descriptorCount = 1;
        }
        w[0].pNext          = &asw;
        w[0].dstBinding     = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        w[1].dstBinding     = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo     = &depthInfo;
        for (uint32_t i = 0; i < 12; ++i)
        {
            infos[i]                = { VK_NULL_HANDLE, st[i]->view, VK_IMAGE_LAYOUT_GENERAL };
            w[2 + i].dstBinding     = 2 + i;
            w[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[2 + i].pImageInfo     = &infos[i];
        }
        vkUpdateDescriptorSets(d, 14, w, 0, nullptr);
    }

    struct Push
    {
        VkDeviceAddress params;
        uint32_t        pass, pad;
    };
    const uint32_t gx = (ext.width + 7) / 8, gy = (ext.height + 7) / 8;
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    // Trace.
    const VkDescriptorSet tsets[] = { sets_[slot], sceneTextures, subTextures ? subTextures : sceneTextures };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, traceLayout_, 0, 3, tsets, 0, nullptr);
    Push push{ params, 0, 0 };
    vkCmdPushConstants(cmd, traceLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    if (!skip("trace"))
    {
        vkCmdDispatch(cmd, gx, gy, 1);
    }
    // Denoise: temporal into the history, then three a-trous passes (steps 1, 2, 4) ending in filtA.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoisePipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoiseLayout_, 0, 1, &sets_[slot], 0, nullptr);
    for (uint32_t pass = 0; pass < 4 && !rr_ && !skip("denoise"); ++pass)
    {
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        Push dp{ params, pass, 0 };
        vkCmdPushConstants(cmd, denoiseLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dp), &dp);
        vkCmdDispatch(cmd, gx, gy, 1);
    }
    // The automatic exposure (one workgroup): game / traced light over a grid of the frame.
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exposePipe_);
    {
        Push ep{ params, 0, 0 };
        vkCmdPushConstants(cmd, denoiseLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
    }
    if (!skip("expose"))
    {
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT);
    historyValid_ = true;
    ++frame_;
    if (skip("composite"))
    {
        return;
    }

    // Composite: the scene as drawn (characters, the ratio fallback) is read while the result goes over it.
    Context::transition(cmd, color.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    Context::transition(cmd, sceneCopy_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{};
    c.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.extent         = { ext.width, ext.height, 1 };
    vkCmdCopyImage(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sceneCopy_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Context::transition(cmd, sceneCopy_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Context::transition(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    {
        VkDescriptorImageInfo in[5]{ { nearest_, sceneCopy_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                     { nearest_, (rr_ ? rawE_ : filtA_).view, VK_IMAGE_LAYOUT_GENERAL },
                                     { nearest_, albedo_.view, VK_IMAGE_LAYOUT_GENERAL },
                                     { nearest_, aux_.view, VK_IMAGE_LAYOUT_GENERAL },
                                     { nearest_, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkWriteDescriptorSet w[5]{};
        for (uint32_t i = 0; i < 5; ++i)
        {
            w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet          = compSets_[slot];
            w[i].dstBinding      = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo      = &in[i];
        }
        vkUpdateDescriptorSets(d, 5, w, 0, nullptr);
    }
    VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView   = colorMip0;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea           = { { 0, 0 }, ext };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &ca;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
    const VkRect2D   sc{ { 0, 0 }, ext };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compPipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compLayout_, 0, 1, &compSets_[slot], 0, nullptr);
    vkCmdPushConstants(cmd, compLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

} // namespace xi::gfx
