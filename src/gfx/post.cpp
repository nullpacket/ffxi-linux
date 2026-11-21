// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/post.h"

#include "gfx/pipeline.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace xi::gfx
{

// The push constants of every post_*.comp pass (post_common.glsl). At most 128 bytes, Vulkan's guaranteed
// maxPushConstantsSize (AMD's own driver reports exactly that): the inverse view-projection is read from view_ instead.
struct PostPush
{
    glm::vec4 eye{ 0.0f };
    glm::vec4 lightDir{ 0.0f };
    glm::vec4 lightColor{ 0.0f };
    glm::vec4 fogColor{ 0.0f };
    glm::vec4 ambient{ 0.0f };
    glm::vec4 sun{ 0.0f };
    glm::vec4 size{ 0.0f };
    VkDeviceAddress view = 0; // PostView: the frame's inverse view-projection
    uint64_t        pad  = 0;
};
static_assert(sizeof(PostPush) == kClientPushConstantBytes);

namespace
{

constexpr VkFormat kWork = VK_FORMAT_R16G16B16A16_SFLOAT;
enum : uint32_t
{
    kHdr = 1, kBloom = 2, kGodrays = 4, kFog = 8, kRq = 16
};

VkPipeline compute(VkDevice d, VkPipelineLayout layout, const char* name)
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

// Binding 0 a storage image, 1 and 2 sampled images; 3 the TLAS (ray-query layout only).
VkDescriptorSetLayout makeSetLayout(VkDevice d, bool rq)
{
    std::vector<VkDescriptorSetLayoutBinding> b = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                                                    { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                                                    { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
    if (rq)
    {
        b.push_back({ 3, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr });
    }
    VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = static_cast<uint32_t>(b.size());
    ci.pBindings    = b.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(d, &ci, nullptr, &l));
    return l;
}

VkPipelineLayout makeLayout(VkDevice d, VkDescriptorSetLayout set)
{
    VkPushConstantRange        pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PostPush) };
    VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    ci.setLayoutCount         = 1;
    ci.pSetLayouts            = &set;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges    = &pr;
    VkPipelineLayout l        = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(d, &ci, nullptr, &l));
    return l;
}

VkExtent2D half(VkExtent2D e)
{
    return { std::max(1u, e.width / 2), std::max(1u, e.height / 2) };
}

// A full memory barrier between the compute passes (all images stay in GENERAL).
void computeBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier2 b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    b.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

} // namespace

std::optional<PostOptions> parsePostOptions(std::string_view s)
{
    PostOptions o;
    if (s.empty() || s == "all")
    {
        o.hdr = o.bloom = o.godrays = o.fog = true;
        return o;
    }
    while (!s.empty())
    {
        const auto        comma = s.find(',');
        const std::string w(s.substr(0, comma));
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
        if (w == "hdr")
        {
            o.hdr = true;
        }
        else if (w == "bloom")
        {
            o.bloom = true;
        }
        else if (w == "godrays")
        {
            o.godrays = true;
        }
        else if (w == "fog")
        {
            o.fog = true;
        }
        else if (w == "all")
        {
            o.hdr = o.bloom = o.godrays = o.fog = true;
        }
        else
        {
            return std::nullopt;
        }
    }
    o.hdr = true; // the others work in the HDR buffer
    return o;
}

PostProcess::PostProcess(Context& ctx, PostOptions opts, bool rayQuery)
: ctx_(ctx)
, opts_(opts)
, rayQuery_(rayQuery)
{
    const VkDevice d = ctx_.device();
    for (Buffer& b : view_)
    {
        b = ctx_.createBuffer(sizeof(glm::mat4),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    }
    setLayout_       = makeSetLayout(d, false);
    layout_          = makeLayout(d, setLayout_);
    volume_          = compute(d, layout_, "post_volume.comp.spv");
    rays_            = compute(d, layout_, "post_rays.comp.spv");
    prep_            = compute(d, layout_, "post_prep.comp.spv");
    down_            = compute(d, layout_, "post_bloom_down.comp.spv");
    up_              = compute(d, layout_, "post_bloom_up.comp.spv");
    final_           = compute(d, layout_, "post_final.comp.spv");
    if (rayQuery_)
    {
        rqSetLayout_ = makeSetLayout(d, true);
        rqLayout_    = makeLayout(d, rqSetLayout_);
        volumeRq_    = compute(d, rqLayout_, "post_volume_rq.comp.spv");
    }
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(d, &si, nullptr, &linear_));
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(d, &si, nullptr, &nearest_));
    const uint32_t                    sets = 4 + 2 * kBloomLevels + Context::kFramesInFlight;
    std::vector<VkDescriptorPoolSize> sizes{ { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * sets } };
    if (rayQuery_)
    {
        sizes.push_back({ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, Context::kFramesInFlight });
    }
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets       = sets;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes    = sizes.data();
    VK_CHECK(vkCreateDescriptorPool(d, &pi, nullptr, &pool_));
    spdlog::info("post: {}{}{}{}(light shafts {})", opts_.hdr ? "hdr " : "", opts_.bloom ? "bloom " : "", opts_.godrays ? "godrays " : "",
                 opts_.fog ? "fog " : "", rayQuery_ ? "ray-queried" : "screen-space");
}

PostProcess::~PostProcess()
{
    ctx_.waitIdle();
    destroyImages();
    for (Buffer& b : view_)
    {
        ctx_.destroyBuffer(b);
    }
    const VkDevice d = ctx_.device();
    vkDestroyDescriptorPool(d, pool_, nullptr);
    for (VkPipeline p : { volume_, volumeRq_, rays_, prep_, down_, up_, final_ })
    {
        if (p)
        {
            vkDestroyPipeline(d, p, nullptr);
        }
    }
    for (VkPipelineLayout l : { layout_, rqLayout_ })
    {
        if (l)
        {
            vkDestroyPipelineLayout(d, l, nullptr);
        }
    }
    for (VkDescriptorSetLayout l : { setLayout_, rqSetLayout_ })
    {
        if (l)
        {
            vkDestroyDescriptorSetLayout(d, l, nullptr);
        }
    }
    vkDestroySampler(d, linear_, nullptr);
    vkDestroySampler(d, nearest_, nullptr);
}

void PostProcess::destroyImages()
{
    for (Image* i : { &work_, &vol_ })
    {
        if (i->image)
        {
            ctx_.destroyImage(*i);
        }
    }
    for (auto& b : bloom_)
    {
        if (b.image)
        {
            ctx_.destroyImage(b);
        }
    }
    if (pool_)
    {
        vkResetDescriptorPool(ctx_.device(), pool_, 0);
    }
    extent_    = {};
    depthView_ = VK_NULL_HANDLE;
}

void PostProcess::ensure(VkExtent2D ext, const Image& depth)
{
    if (ext.width == extent_.width && ext.height == extent_.height && depth.view == depthView_)
    {
        return;
    }
    ctx_.waitIdle();
    destroyImages();
    extent_    = ext;
    depthView_ = depth.view;
    constexpr VkImageUsageFlags kUse = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    work_      = ctx_.createImage(ext, kWork, kUse, VK_IMAGE_ASPECT_COLOR_BIT);
    vol_       = ctx_.createImage(half(ext), kWork, kUse, VK_IMAGE_ASPECT_COLOR_BIT);
    VkExtent2D e = ext;
    for (auto& b : bloom_)
    {
        e = half(e);
        b = ctx_.createImage(e, kWork, kUse, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    const VkDevice d     = ctx_.device();
    auto           alloc = [&](VkDescriptorSetLayout l) {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool     = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &l;
        VkDescriptorSet s     = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(d, &ai, &s));
        return s;
    };
    // (storage, sampled A, sampled B); a sampled image of GENERAL layout unless it is the depth.
    auto write = [&](VkDescriptorSet s, const Image& dst, VkImageView a, VkSampler sa, VkImageLayout la, VkImageView b, VkSampler sb, VkImageLayout lb) {
        VkDescriptorImageInfo st{ VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo ia{ sa, a, la };
        VkDescriptorImageInfo ib{ sb, b, lb };
        VkWriteDescriptorSet  w[3]{};
        for (int i = 0; i < 3; ++i)
        {
            w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet          = s;
            w[i].dstBinding      = static_cast<uint32_t>(i);
            w[i].descriptorCount = 1;
            w[i].descriptorType  = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        w[0].pImageInfo = &st;
        w[1].pImageInfo = &ia;
        w[2].pImageInfo = &ib;
        vkUpdateDescriptorSets(d, 3, w, 0, nullptr);
    };
    constexpr auto G = VK_IMAGE_LAYOUT_GENERAL;
    constexpr auto R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    volumeSet_       = alloc(setLayout_);
    write(volumeSet_, vol_, depth.view, nearest_, R, depth.view, nearest_, R);
    raysSet_ = alloc(setLayout_);
    write(raysSet_, vol_, work_.view, linear_, G, depth.view, nearest_, R);
    prepSet_ = alloc(setLayout_);
    write(prepSet_, work_, vol_.view, linear_, G, depth.view, nearest_, R);
    finalSet_ = alloc(setLayout_);
    write(finalSet_, work_, bloom_[0].view, linear_, G, depth.view, nearest_, R);
    for (int i = 0; i < kBloomLevels; ++i)
    {
        downSets_[i] = alloc(setLayout_);
        write(downSets_[i], bloom_[i], i == 0 ? work_.view : bloom_[i - 1].view, linear_, G, depth.view, nearest_, R);
        upSets_[i] = alloc(setLayout_);
        write(upSets_[i], bloom_[i], i + 1 < kBloomLevels ? bloom_[i + 1].view : bloom_[i].view, linear_, G, depth.view, nearest_, R);
    }
    if (rayQuery_)
    {
        for (auto& s : volumeRqSets_)
        {
            s = alloc(rqSetLayout_);
            write(s, vol_, depth.view, nearest_, R, depth.view, nearest_, R);
        }
    }
    // The images start (and stay) in GENERAL.
    ctx_.immediateSubmit([&](VkCommandBuffer cmd) {
        for (Image* i : { &work_, &vol_ })
        {
            Context::transition(cmd, i->image, VK_IMAGE_LAYOUT_UNDEFINED, G);
        }
        for (auto& b : bloom_)
        {
            Context::transition(cmd, b.image, VK_IMAGE_LAYOUT_UNDEFINED, G);
        }
    });
}

void PostProcess::dispatch(VkCommandBuffer cmd, VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set, VkExtent2D size, const PostPush& push)
{
    PostPush p = push;
    p.size     = glm::vec4(size.width, size.height, 1.0f / static_cast<float>(size.width), 1.0f / static_cast<float>(size.height));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PostPush), &p);
    vkCmdDispatch(cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
    computeBarrier(cmd);
}

void PostProcess::apply(VkCommandBuffer cmd, uint32_t slot, VkImage image, VkImageLayout layout, VkExtent2D ext, const Image& depth, VkImageLayout depthLayout,
                        const View& view)
{
    ensure(ext, depth);
    constexpr auto G = VK_IMAGE_LAYOUT_GENERAL;
    const bool     volume = opts_.fog || opts_.godrays;
    const bool     rq     = rayQuery_ && view.tlas != VK_NULL_HANDLE;

    // The scene into the working buffer (an 8-bit -> half-float blit).
    Context::transition(cmd, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    Context::transition(cmd, work_.image, G, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.srcOffsets[1]  = { static_cast<int32_t>(ext.width), static_cast<int32_t>(ext.height), 1 };
    b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.dstOffsets[1]  = b.srcOffsets[1];
    vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, work_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    Context::transition(cmd, work_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, G);
    if (depthLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        Context::transition(cmd, depth.image, depthLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    PostPush        p;
    Buffer&         vb  = view_[slot % Context::kFramesInFlight];
    const glm::mat4 inv = glm::inverse(view.viewProj);
    std::memcpy(vb.mapped, &inv, sizeof(inv));
    p.view        = ctx_.address(vb);
    p.eye         = glm::vec4(view.eye, static_cast<float>(view.frame & 0xFFFF));
    p.lightDir    = glm::vec4(glm::normalize(view.lightDir), view.fogEnd);
    p.lightColor  = glm::vec4(view.lightColor, opts_.bloomStrength);
    p.fogColor    = glm::vec4(view.fogColor, opts_.fogDensity);
    p.ambient     = glm::vec4(view.ambient, opts_.shaftStrength);
    // Light 0 on screen: a point far along its direction, projected; z fades it out as it leaves the screen / goes behind.
    const glm::vec4 s = view.viewProj * glm::vec4(view.eye + glm::normalize(view.lightDir) * 1000.0f, 1.0f);
    glm::vec2       uv{ 0.5f };
    float           on = 0.0f;
    if (s.w > 0.0f)
    {
        uv            = glm::vec2(s.x / s.w, s.y / s.w) * 0.5f + 0.5f;
        const float o = std::max(std::max(-uv.x, uv.x - 1.0f), std::max(-uv.y, uv.y - 1.0f)); // how far outside
        on            = std::clamp(1.0f - o * 2.0f, 0.0f, 1.0f);
    }
    const uint32_t flags = (opts_.hdr ? kHdr : 0u) | (opts_.bloom ? kBloom : 0u) | (opts_.godrays ? kGodrays : 0u) | (opts_.fog ? kFog : 0u) | (rq ? kRq : 0u);
    p.sun                = glm::vec4(uv, on, static_cast<float>(flags));

    if (volume)
    {
        if (rq)
        {
            VkWriteDescriptorSetAccelerationStructureKHR as{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
            as.accelerationStructureCount = 1;
            as.pAccelerationStructures    = &view.tlas;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.pNext           = &as;
            w.dstSet          = volumeRqSets_[slot % Context::kFramesInFlight];
            w.dstBinding      = 3;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(ctx_.device(), 1, &w, 0, nullptr);
            dispatch(cmd, volumeRq_, rqLayout_, w.dstSet, vol_.extent, p);
        }
        else
        {
            dispatch(cmd, volume_, layout_, volumeSet_, vol_.extent, p);
            if (opts_.godrays)
            {
                dispatch(cmd, rays_, layout_, raysSet_, vol_.extent, p);
            }
        }
    }
    dispatch(cmd, prep_, layout_, prepSet_, ext, p);
    if (opts_.bloom)
    {
        for (int i = 0; i < kBloomLevels; ++i)
        {
            PostPush q = p;
            q.eye.w    = i == 0 ? -1.0f : 0.0f;
            dispatch(cmd, down_, layout_, downSets_[i], bloom_[i].extent, q);
        }
        for (int i = kBloomLevels - 2; i >= 0; --i)
        {
            dispatch(cmd, up_, layout_, upSets_[i], bloom_[i].extent, p);
        }
    }
    dispatch(cmd, final_, layout_, finalSet_, ext, p);

    // Back into the scene image.
    Context::transition(cmd, work_.image, G, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Context::transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    vkCmdBlitImage(cmd, work_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    Context::transition(cmd, work_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G);
    Context::transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    if (depthLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        Context::transition(cmd, depth.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depthLayout, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
}

} // namespace xi::gfx
