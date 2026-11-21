// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/skinned_renderer.h"

#include "gfx/raytracing.h"

#include "gfx/pipeline.h"

#include <SDL3/SDL.h>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace xi::gfx
{

namespace
{

// skinned.vert / skinned.frag: viewProj @0, bones @64, unused @72, frame @80.
struct SkinPush
{
    glm::mat4       viewProj;
    VkDeviceAddress bones;
    VkDeviceAddress unused;
    VkDeviceAddress frame;
    uint32_t        boneBase;
    uint32_t        texture;
    uint32_t        flags;
    uint32_t        tint; // RGBA8, 0x80 = 1.0
    uint32_t        envTexture;
    uint32_t        specTexture;
    glm::vec4       origin{ 0.0f }; // the actor's position (render space), for the specular look-at
};
static_assert(sizeof(SkinPush) <= kClientPushConstantBytes); // Vulkan's guaranteed push-constant size (issue #139)

// skinned.frag `Frame`: mesh.frag's prefix, then the view basis (env / specular reflections).
struct FrameParamsGpu
{
    glm::vec4 eyeFogStart, fogColorEnd, ambient, light0Dir, light0Color, light1Dir, light1Color, camRight, camUp, camForward;
    glm::vec4 specMode; // x = 1: the game's specular mapping (dat-format §10.3), 0: XI_SPEC_OLD's centred blob
};

// The skinned vertex layout and winding (FFXI data is clockwise-front; game->render is a rotation).
void setSkinnedVertexInput(PipelineDesc& pd)
{
    pd.frontFace  = VK_FRONT_FACE_CLOCKWISE;
    pd.bindings   = { { 0, sizeof(SkinnedVertex), VK_VERTEX_INPUT_RATE_VERTEX } };
    pd.attributes = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, p0) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, p1) },
        { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, n0) },
        { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, n1) },
        { 4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SkinnedVertex, uv) },
        { 5, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(SkinnedVertex, bones) },
        { 6, 0, VK_FORMAT_R32_SFLOAT, offsetof(SkinnedVertex, weight1) },
    };
}

} // namespace

SkinnedRenderer::SkinnedRenderer(Context& ctx, TextureSet& textures)
: ctx_(ctx)
, textures_(textures)
{
    for (auto& b : bonePalettes_)
    {
        b = ctx_.createBuffer(sizeof(glm::mat4) * kMaxBones, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    }
    for (auto& b : frameParams_)
    {
        b = ctx_.createBuffer(sizeof(FrameParamsGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    }
    VkPushConstantRange        pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkinPush) };
    auto                       setLayout = textures_.layout();
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 1;
    lci.pSetLayouts            = &setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcr;
    VK_CHECK(vkCreatePipelineLayout(ctx_.device(), &lci, nullptr, &layout_));

    const auto  dir  = shaderDir();
    PipelineDesc pd;
    pd.vert        = loadShader(ctx_.device(), dir / "skinned.vert.spv");
    pd.frag        = loadShader(ctx_.device(), dir / "skinned.frag.spv");
    pd.layout      = layout_;
    pd.colorFormat = ctx_.colorFormat();
    pd.depthFormat = VK_FORMAT_D32_SFLOAT;
    setSkinnedVertexInput(pd);
    const VkCullModeFlags culls[3] = { VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_NONE };
    for (int b = 0; b < 2; ++b)
    {
        for (int c = 0; c < 3; ++c)
        {
            pd.alphaBlend    = b == 1;
            pd.cullMode      = culls[c];
            pipelines_[b][c] = createPipeline(ctx_.device(), pd);
        }
    }
    // The overlay pass (effect ops 0x40 / 0x41 / 0x54 / 0x55): over the drawn model, no depth writes, SRCALPHA / ONE
    // (blend 0x48) or SRCALPHA / INVSRCALPHA.
    for (int b = 0; b < 2; ++b)
    {
        VkPipelineColorBlendAttachmentState bs{};
        bs.blendEnable         = VK_TRUE;
        bs.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        bs.dstColorBlendFactor = b == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        bs.colorBlendOp        = VK_BLEND_OP_ADD;
        bs.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        bs.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        bs.alphaBlendOp        = VK_BLEND_OP_ADD;
        bs.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        pd.alphaBlend          = true;
        pd.blend               = bs;
        pd.cullMode            = VK_CULL_MODE_BACK_BIT;
        overlayPipelines_[b]   = createPipeline(ctx_.device(), pd);
    }
    vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
}

SkinnedRenderer::~SkinnedRenderer()
{
    ctx_.waitIdle();
    for (auto& m : models_)
    {
        ctx_.destroyBuffer(m.vertices);
        ctx_.destroyBuffer(m.indices);
    }
    for (auto& b : bonePalettes_)
    {
        ctx_.destroyBuffer(b);
    }
    for (auto& b : frameParams_)
    {
        ctx_.destroyBuffer(b);
    }
    for (auto& row : pipelines_)
    {
        for (auto p : row)
        {
            vkDestroyPipeline(ctx_.device(), p, nullptr);
        }
    }
    for (auto p : overlayPipelines_)
    {
        vkDestroyPipeline(ctx_.device(), p, nullptr);
    }
    for (auto p : motionPipelines_)
    {
        if (p)
        {
            vkDestroyPipeline(ctx_.device(), p, nullptr);
        }
    }
    for (auto& b : prevPalettes_)
    {
        if (b.buffer)
        {
            ctx_.destroyBuffer(b);
        }
    }
    if (silhouettePipeline_)
    {
        vkDestroyPipeline(ctx_.device(), silhouettePipeline_, nullptr);
    }
    vkDestroyPipelineLayout(ctx_.device(), layout_, nullptr);
    if (refractPipeline_)
    {
        vkDestroyPipeline(ctx_.device(), refractPipeline_, nullptr);
        vkDestroyPipelineLayout(ctx_.device(), refractLayout_, nullptr);
    }
}

uint32_t SkinnedRenderer::addModel(const std::vector<SkinnedVertex>& vertices, const std::vector<uint32_t>& indices, std::vector<SubMesh> subMeshes)
{
    return addModel(ctx_.uploadBuffer(vertices.data(), vertices.size() * sizeof(SkinnedVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rtGeometryUsage()),
                    ctx_.uploadBuffer(indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rtGeometryUsage()), std::move(subMeshes));
}

uint32_t SkinnedRenderer::addModel(Buffer vertices, Buffer indices, std::vector<SubMesh> subMeshes)
{
    Model m;
    m.vertices  = vertices;
    m.indices   = indices;
    m.subMeshes = std::move(subMeshes);
    m.alive     = true;
    for (uint32_t i = 0; i < models_.size(); ++i)
    {
        if (!models_[i].alive)
        {
            models_[i] = std::move(m);
            return i;
        }
    }
    models_.push_back(std::move(m));
    return static_cast<uint32_t>(models_.size() - 1);
}

void SkinnedRenderer::removeModel(uint32_t model)
{
    if (model < models_.size() && models_[model].alive)
    {
        ctx_.waitIdle(); // rare (entity leaves range); keeps buffer lifetime simple
        ctx_.destroyBuffer(models_[model].vertices);
        ctx_.destroyBuffer(models_[model].indices);
        models_[model].alive = false;
    }
}

void SkinnedRenderer::prepare(const std::vector<Instance>& instances)
{
    bool added = false;
    for (const auto& inst : instances)
    {
        if (inst.overlay && !overlayIndex_.contains(inst.overlay) && textures_.size() < TextureSet::kMaxTextures)
        {
            overlayIndex_[inst.overlay] = textures_.add(*inst.overlay);
            added                       = true;
        }
    }
    if (added)
    {
        textures_.commit();
    }
}

void SkinnedRenderer::draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const SceneParams& scene, const std::vector<Instance>& instances)
{
    if (instances.empty())
    {
        return;
    }
    auto& fp = frameParams_[frameSlot % frameParams_.size()];
    *static_cast<FrameParamsGpu*>(fp.mapped) = {
        glm::vec4(scene.eye, scene.fogStart),          glm::vec4(scene.fogColor, scene.fogEnd), glm::vec4(scene.ambient, 0.0f),
        glm::vec4(glm::normalize(scene.light0Dir), 0), glm::vec4(scene.light0Color, 0.0f),     glm::vec4(glm::normalize(scene.light1Dir), 0),
        glm::vec4(scene.light1Color, 0.0f),           glm::vec4(scene.camRight, 0.0f),        glm::vec4(scene.camUp, 0.0f),
        glm::vec4(scene.camForward, 0.0f),
        glm::vec4(std::getenv("XI_SPEC_OLD") ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f),
    };
    auto&       palette = bonePalettes_[frameSlot % bonePalettes_.size()];
    auto*       bones   = static_cast<glm::mat4*>(palette.mapped);
    std::size_t used    = 0;
    const auto  boneAddr  = ctx_.address(palette);
    const auto  frameAddr = ctx_.address(fp);

    auto set = textures_.set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    // Opaque first, then blended.
    for (int pass = 0; pass < 2; ++pass)
    {
        std::size_t paletteCursor = 0;
        for (const auto& inst : instances)
        {
            if (inst.model >= models_.size() || !models_[inst.model].alive || inst.bones.empty())
            {
                continue;
            }
            if (pass == 0)
            {
                if (used + inst.bones.size() > kMaxBones)
                {
                    break;
                }
                std::memcpy(bones + used, inst.bones.data(), inst.bones.size() * sizeof(glm::mat4));
                used += inst.bones.size();
            }
            const auto  boneBase = static_cast<uint32_t>(paletteCursor);
            paletteCursor += inst.bones.size();
            const auto& m        = models_[inst.model];
            VkDeviceSize zero    = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            for (const auto& s : m.subMeshes)
            {
                if (s.blended != (pass == 1) || (s.partTag >= 0 && ((inst.hiddenTags >> s.partTag) & 1)))
                {
                    continue;
                }
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[pass][s.doubleSided ? 2 : 0]);
                // Fading corpse or an effect's alpha below 0x80: mesh.frag dither.
                const float    gone = std::max(inst.fade, 1.0f - inst.tint.a);
                const uint32_t fade = static_cast<uint32_t>(std::clamp(gone, 0.0f, 1.0f) * 255.0f) << 8;
                auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v * 128.0f, 0.0f, 255.0f)); };
                const uint32_t tint = byte(inst.tint.r) | byte(inst.tint.g) << 8 | byte(inst.tint.b) << 16 | 0x80u << 24;
                const glm::vec3 origin = inst.origin ? *inst.origin : glm::vec3(inst.bones.empty() ? glm::vec4(0.0f) : inst.bones[0][3]);
                SkinPush p{ viewProj, boneAddr, 0, frameAddr, boneBase, s.texture, (s.alphaTest ? 1u : 0u) | fade | s.material, tint,
                            s.envTexture, s.specTexture, glm::vec4(origin, 1.0f) };
                vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
                vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, s.vertexOffset, 0);
            }
        }
    }
    lastBones_ = boneAddr, lastFrame_ = frameAddr, lastUsed_ = used;
    // Overlay pass: the whole model again with the overlay texture (flags bit 1), after everything else.
    std::size_t paletteCursor = 0;
    for (const auto& inst : instances)
    {
        if (inst.model >= models_.size() || !models_[inst.model].alive || inst.bones.empty())
        {
            continue;
        }
        const auto boneBase = static_cast<uint32_t>(paletteCursor);
        paletteCursor += inst.bones.size();
        const auto ov = inst.overlay ? overlayIndex_.find(inst.overlay) : overlayIndex_.end();
        if (ov == overlayIndex_.end() || paletteCursor > used)
        {
            continue;
        }
        const auto&  m    = models_[inst.model];
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipelines_[inst.overlayAdditive ? 1 : 0]);
        const uint32_t scroll = glm::packHalf2x16(inst.overlayScroll);
        auto           byte   = [](float v) { return static_cast<uint32_t>(std::clamp(v * 128.0f, 0.0f, 255.0f)); };
        const uint32_t tint   = byte(inst.overlayTint.r) | byte(inst.overlayTint.g) << 8 | byte(inst.overlayTint.b) << 16 | byte(inst.overlayTint.a) << 24;
        for (const auto& s : m.subMeshes)
        {
            if (s.partTag >= 0 && ((inst.hiddenTags >> s.partTag) & 1))
            {
                continue;
            }
            SkinPush p{ viewProj, boneAddr, 0, frameAddr, boneBase, s.texture, 2u | (s.alphaTest ? 1u : 0u), tint, ov->second, scroll };
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, s.vertexOffset, 0);
        }
    }
}

void SkinnedRenderer::prewarm(VkFormat silhouetteFormat, VkDescriptorSetLayout screenLayout)
{
    // The pipelines made on first use, made now: on the first in-zone frame they stalled it ~10 ms when the driver's
    // shader cache was cold (docs/performance.md goal 25).
    ensureSilhouettePipeline(silhouetteFormat);
    ensureRefractPipeline(screenLayout);
}

void SkinnedRenderer::ensureSilhouettePipeline(VkFormat format)
{
    if (silhouettePipeline_)
    {
        return; // the effect render textures it draws into are all R8G8B8A8_UNORM
    }
    const auto   dir  = shaderDir();
    PipelineDesc pd;
    pd.vert        = loadShader(ctx_.device(), dir / "skinned.vert.spv");
    pd.frag        = loadShader(ctx_.device(), dir / "silhouette.frag.spv");
    pd.layout      = layout_;
    pd.colorFormat = format;
    pd.depthFormat = VK_FORMAT_UNDEFINED;
    pd.cullMode    = VK_CULL_MODE_NONE; // flattened: the winding means nothing
    setSkinnedVertexInput(pd);
    silhouettePipeline_ = createPipeline(ctx_.device(), pd, false);
    vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
}

void SkinnedRenderer::ensureRefractPipeline(VkDescriptorSetLayout screenLayout)
{
    if (refractPipeline_)
    {
        return;
    }
    VkPushConstantRange                        pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkinPush) };
    const std::array<VkDescriptorSetLayout, 2> sets{ textures_.layout(), screenLayout };
    VkPipelineLayoutCreateInfo                 lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 2;
    lci.pSetLayouts            = sets.data();
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcr;
    VK_CHECK(vkCreatePipelineLayout(ctx_.device(), &lci, nullptr, &refractLayout_));
    const auto   dir  = shaderDir();
    PipelineDesc pd;
    pd.vert        = loadShader(ctx_.device(), dir / "skinned.vert.spv");
    pd.frag        = loadShader(ctx_.device(), dir / "skinned_refract.frag.spv");
    pd.layout      = refractLayout_;
    pd.colorFormat = ctx_.colorFormat();
    pd.depthFormat = VK_FORMAT_D32_SFLOAT;
    pd.cullMode    = VK_CULL_MODE_BACK_BIT;
    pd.alphaBlend  = true; // SRCALPHA / INVSRCALPHA, no depth writes
    setSkinnedVertexInput(pd);
    refractPipeline_ = createPipeline(ctx_.device(), pd);
    vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
}

void SkinnedRenderer::drawSilhouettes(VkCommandBuffer cmd, const Image& target, const std::vector<Instance>& instances, const std::vector<SilhouetteJob>& jobs)
{
    if (jobs.empty() || lastBones_ == 0)
    {
        return;
    }
    ensureSilhouettePipeline(target.format);
    Context::transition(cmd, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView   = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue  = {};
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea           = { { 0, 0 }, target.extent };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &ri);
    auto set = textures_.set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, silhouettePipeline_);
    // Palette offsets as draw() laid them out.
    std::vector<std::size_t> base(instances.size(), 0);
    std::size_t              cursor = 0;
    for (std::size_t i = 0; i < instances.size(); ++i)
    {
        const auto& inst = instances[i];
        base[i]          = cursor;
        if (inst.model < models_.size() && models_[inst.model].alive && !inst.bones.empty())
        {
            cursor += inst.bones.size();
        }
    }
    for (const auto& j : jobs)
    {
        if (j.instance >= instances.size())
        {
            continue;
        }
        const auto& inst = instances[j.instance];
        if (inst.model >= models_.size() || !models_[inst.model].alive || inst.bones.empty() || base[j.instance] + inst.bones.size() > lastUsed_)
        {
            continue;
        }
        const VkViewport vp{ static_cast<float>(j.cell.offset.x), static_cast<float>(j.cell.offset.y), static_cast<float>(j.cell.extent.width),
                             static_cast<float>(j.cell.extent.height), 0, 1 };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &j.cell);
        const auto&  m    = models_[inst.model];
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const auto& s : m.subMeshes)
        {
            if (s.partTag >= 0 && ((inst.hiddenTags >> s.partTag) & 1))
            {
                continue;
            }
            SkinPush p{ j.viewProj, lastBones_, 0, lastFrame_, static_cast<uint32_t>(base[j.instance]), s.texture, s.alphaTest ? 1u : 0u, 0x80808080u, 0, 0 };
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, s.vertexOffset, 0);
        }
    }
    vkCmdEndRendering(cmd);
    Context::transition(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void SkinnedRenderer::drawMotion(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& jitteredViewProj, VkDeviceAddress motionBlock,
                                 const std::vector<Instance>& instances)
{
    if (lastBones_ == 0)
    {
        return;
    }
    if (!motionPipelines_[0])
    {
        const auto   dir = shaderDir();
        PipelineDesc pd;
        pd.vert        = loadShader(ctx_.device(), dir / "motion_skinned.vert.spv");
        pd.frag        = loadShader(ctx_.device(), dir / "motion_skinned.frag.spv");
        pd.layout      = layout_;
        pd.colorFormat = VK_FORMAT_R16G16_SFLOAT;
        pd.depthFormat = VK_FORMAT_D32_SFLOAT;
        setSkinnedVertexInput(pd);
        // Tested against the colour pass's depth, not written (alphaBlend = no depth writes; the blend itself is off).
        VkPipelineColorBlendAttachmentState bs{};
        bs.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
        pd.alphaBlend     = true;
        pd.blend          = bs;
        const VkCullModeFlags culls[3] = { VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_NONE };
        for (int c = 0; c < 3; ++c)
        {
            pd.cullMode         = culls[c];
            motionPipelines_[c] = createPipeline(ctx_.device(), pd);
        }
        vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
        vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
        for (auto& b : prevPalettes_)
        {
            b = ctx_.createBuffer(sizeof(glm::mat4) * kMaxBones, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
        }
    }
    // Last frame's bones, laid out as this frame's palette: each instance takes the unclaimed previous instance of the
    // same model whose root is nearest (within 4 yalms), else its own bones (camera motion only).
    auto&                  prevPal = prevPalettes_[frameSlot % prevPalettes_.size()];
    auto*                  prevOut = static_cast<glm::mat4*>(prevPal.mapped);
    std::vector<bool>      claimed(prevInstances_.size(), false);
    std::vector<PrevInstance> next;
    next.reserve(instances.size());
    std::size_t cursor = 0;
    for (const auto& inst : instances)
    {
        if (inst.model >= models_.size() || !models_[inst.model].alive || inst.bones.empty())
        {
            continue;
        }
        if (cursor + inst.bones.size() > lastUsed_)
        {
            break;
        }
        const glm::vec3 root(inst.bones[0][3]);
        int             best = -1;
        float           bestD = 16.0f;
        for (std::size_t k = 0; k < prevInstances_.size(); ++k)
        {
            const auto& p = prevInstances_[k];
            if (claimed[k] || p.model != inst.model || p.bones.size() != inst.bones.size())
            {
                continue;
            }
            const glm::vec3 d = p.root - root;
            if (const float dd = glm::dot(d, d); dd < bestD)
            {
                bestD = dd;
                best  = static_cast<int>(k);
            }
        }
        const auto& src = best >= 0 ? prevInstances_[static_cast<std::size_t>(best)].bones : inst.bones;
        if (best >= 0)
        {
            claimed[static_cast<std::size_t>(best)] = true;
        }
        std::memcpy(prevOut + cursor, src.data(), src.size() * sizeof(glm::mat4));
        cursor += inst.bones.size();
        next.push_back({ inst.model, root, inst.bones });
    }
    const auto prevAddr = ctx_.address(prevPal);
    auto       set      = textures_.set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    std::size_t paletteCursor = 0;
    for (const auto& inst : instances)
    {
        if (inst.model >= models_.size() || !models_[inst.model].alive || inst.bones.empty())
        {
            continue;
        }
        const auto boneBase = static_cast<uint32_t>(paletteCursor);
        paletteCursor += inst.bones.size();
        // Solid parts only: dissolving / see-through actors keep the background's motion, as their pixels mostly show it.
        if (paletteCursor > cursor || inst.fade > 0.5f || inst.tint.a < 0.5f)
        {
            continue;
        }
        const auto&  m    = models_[inst.model];
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const auto& s : m.subMeshes)
        {
            if (s.blended || (s.partTag >= 0 && ((inst.hiddenTags >> s.partTag) & 1)))
            {
                continue;
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motionPipelines_[s.doubleSided ? 2 : 0]);
            SkinPush p{ jitteredViewProj, lastBones_, prevAddr, motionBlock, boneBase, s.texture, s.alphaTest ? 1u : 0u, 0, 0, 0 };
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, s.vertexOffset, 0);
        }
    }
    prevInstances_ = std::move(next);
}

bool SkinnedRenderer::needsScreen(const std::vector<Instance>& instances)
{
    return std::ranges::any_of(instances, [](const Instance& i) { return i.refraction != 0.0f && i.tint.a < 1.0f; });
}

void SkinnedRenderer::drawRefraction(VkCommandBuffer cmd, const glm::mat4& viewProj, const std::vector<Instance>& instances,
                                     VkDescriptorSetLayout screenLayout, VkDescriptorSet screenSet)
{
    if (!needsScreen(instances) || lastBones_ == 0)
    {
        return;
    }
    ensureRefractPipeline(screenLayout);
    const std::array<VkDescriptorSet, 2> sets{ textures_.set(), screenSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, refractLayout_, 0, 2, sets.data(), 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, refractPipeline_);
    std::size_t paletteCursor = 0;
    for (const auto& inst : instances)
    {
        if (inst.model >= models_.size() || !models_[inst.model].alive || inst.bones.empty())
        {
            continue;
        }
        const auto boneBase = static_cast<uint32_t>(paletteCursor);
        paletteCursor += inst.bones.size();
        // Weight (0x80 - the actor's alpha) / 0x80: 1 - tint.a (tint 1 = 0x80).
        const float weight = std::clamp(1.0f - inst.tint.a, 0.0f, 1.0f);
        if (inst.refraction == 0.0f || weight <= 0.0f || paletteCursor > lastUsed_)
        {
            continue;
        }
        const auto&  m    = models_[inst.model];
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        const uint32_t tint = 0x808080u | static_cast<uint32_t>(weight * 255.0f) << 24;
        for (const auto& s : m.subMeshes)
        {
            if (s.partTag >= 0 && ((inst.hiddenTags >> s.partTag) & 1))
            {
                continue;
            }
            SkinPush p{ viewProj, lastBones_, 0, lastFrame_, boneBase, s.texture, s.alphaTest ? 1u : 0u, tint, std::bit_cast<uint32_t>(inst.refraction), 0 };
            vkCmdPushConstants(cmd, refractLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, s.vertexOffset, 0);
        }
    }
}

void SkinnedRenderer::rtRemember(const std::vector<Instance>& instances)
{
    // The palette order of draw(): every live instance with bones, in order, until the palette is full.
    rtActors_.clear();
    if (lastBones_ == 0)
    {
        return;
    }
    std::size_t cursor = 0;
    for (const auto& inst : instances)
    {
        if (inst.model >= models_.size() || !models_[inst.model].alive || inst.bones.empty())
        {
            continue;
        }
        if (cursor + inst.bones.size() > lastUsed_)
        {
            break;
        }
        const auto& m    = models_[inst.model];
        const auto  base = static_cast<uint32_t>(cursor);
        cursor += inst.bones.size();
        if (inst.fade > 0.5f || inst.tint.a < 0.5f) // dissolving or see-through: no shadow
        {
            continue;
        }
        RtActor a;
        a.vertices    = ctx_.address(m.vertices);
        a.indices     = ctx_.address(m.indices);
        a.bones       = lastBones_;
        a.vertexCount = static_cast<uint32_t>(m.vertices.size / sizeof(SkinnedVertex));
        a.boneBase    = base;
        a.origin      = inst.origin ? *inst.origin : glm::vec3(inst.bones[0][3]);
        for (const auto& sm : m.subMeshes)
        {
            if (sm.blended || sm.indexCount < 3 || (sm.partTag >= 0 && ((inst.hiddenTags >> sm.partTag) & 1)))
            {
                continue;
            }
            a.parts.emplace_back(sm.firstIndex, sm.indexCount - sm.indexCount % 3, sm.vertexOffset);
        }
        rtActors_.push_back(std::move(a));
    }
}

} // namespace xi::gfx
