// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/mesh_renderer.h"

#include "gfx/pipeline.h"
#include "gfx/raytracing.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <tuple>

namespace xi::gfx
{

namespace
{

struct MeshPush // within the guaranteed 128 bytes
{
    glm::mat4       viewProj;
    VkDeviceAddress transforms;
    VkDeviceAddress draws;
    VkDeviceAddress frame;
};
static_assert(sizeof(MeshPush) <= kClientPushConstantBytes); // Vulkan's guaranteed push-constant size (issue #139)

// std430 mirror of `Frame` in mesh.frag.
struct FrameParamsGpu
{
    glm::vec4 eyeFogStart;
    glm::vec4 fogColorEnd;
    glm::vec4 ambient;
    glm::vec4 light0Dir;
    glm::vec4 light0Color;
    glm::vec4 light1Dir;
    glm::vec4 light1Color;
    glm::vec4 points[SceneParams::kMaxPointLights * 2]; // pos.xyz + range, colour.rgb
};

std::filesystem::path shaderPath(const char* name)
{
    return shaderDir() / name;
}

} // namespace

MeshRenderer::MeshRenderer(Context& ctx, TextureSet& textures)
: ctx_(ctx)
, textures_(textures)
{
    for (auto& b : frameParams_)
    {
        b = ctx_.createBuffer(sizeof(FrameParamsGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    }
    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPush) };
    auto                setLayout = textures_.layout();
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 1;
    lci.pSetLayouts            = &setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcr;
    VK_CHECK(vkCreatePipelineLayout(ctx_.device(), &lci, nullptr, &layout_));

    PipelineDesc pd;
    pd.vert        = loadShader(ctx_.device(), shaderPath("mesh.vert.spv"));
    pd.frag        = loadShader(ctx_.device(), shaderPath("mesh.frag.spv"));
    pd.layout      = layout_;
    pd.colorFormat = ctx_.colorFormat();
    pd.depthFormat = VK_FORMAT_D32_SFLOAT;
    pd.bindings    = { { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX } };
    pd.frontFace   = VK_FRONT_FACE_CLOCKWISE;
    pd.attributes  = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal) },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, color) },
        { 3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv) },
    };
    // Zone data is clockwise-front (D3D). The game-to-render mapping is a rotation, so that stays true.
    const VkCullModeFlags culls[kCullModes] = { VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_NONE };
    for (int b = 0; b < 2; ++b)
    {
        for (int c = 0; c < kCullModes; ++c)
        {
            pd.alphaBlend       = b == 1;
            pd.depthBias        = b == 1; // client: ZBIAS 8 on blended pieces
            pd.cullMode         = culls[c];
            pipelines_[b][c]    = createPipeline(ctx_.device(), pd);
        }
    }
    vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
}

MeshRenderer::~MeshRenderer()
{
    ctx_.waitIdle();
    rtForget(this); // --rt: its acceleration structures
    for (auto* b : { &vertexBuffer_, &indexBuffer_, &transformBuffer_, &cmdBuffer_, &dataBuffer_ })
    {
        ctx_.destroyBuffer(*b);
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
    vkDestroyPipelineLayout(ctx_.device(), layout_, nullptr);
}

uint32_t MeshRenderer::addMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, std::vector<SubMesh> subMeshes)
{
    const auto baseVertex = static_cast<int32_t>(vertices_.size());
    const auto baseIndex  = static_cast<uint32_t>(indices_.size());
    vertices_.insert(vertices_.end(), vertices.begin(), vertices.end());
    indices_.insert(indices_.end(), indices.begin(), indices.end());
    for (auto& s : subMeshes)
    {
        s.firstIndex += baseIndex;
        s.vertexOffset += baseVertex;
    }
    meshes_.push_back({ std::move(subMeshes) });
    return static_cast<uint32_t>(meshes_.size() - 1);
}

uint32_t MeshRenderer::addInstance(uint32_t mesh, const glm::mat4& transform, bool mirrored, uint32_t lights)
{
    transforms_.push_back(transform);
    instances_.push_back({ mesh, static_cast<uint32_t>(transforms_.size() - 1), mirrored, lights });
    return static_cast<uint32_t>(instances_.size() - 1);
}

uint32_t MeshRenderer::addLodGroup(const glm::vec3& centre, float near2, float mid2)
{
    lodGroups_.push_back({ centre, near2, mid2, 0, {} });
    return static_cast<uint32_t>(lodGroups_.size() - 1);
}

void MeshRenderer::setInstanceLod(uint32_t instance, uint32_t group, uint8_t bands)
{
    if (instance < instances_.size())
    {
        instances_[instance].lodGroup = group;
        instances_[instance].bands    = bands;
    }
}

void MeshRenderer::updateLod(const glm::vec3& eye)
{
    // CODE 0x1017c8d0: d² <= instance+0xC8 draws the high mesh, <= +0xCC the medium one, else the low one.
    for (auto& g : lodGroups_)
    {
        const glm::vec3 d    = g.centre - eye;
        const float     d2   = glm::dot(d, d);
        const int       band = d2 <= g.near2 ? 0 : d2 <= g.mid2 ? 1 : 2;
        if (band == g.band)
        {
            continue;
        }
        for (const auto& [cmd, bands] : g.draws)
        {
            const bool was = (bands >> g.band) & 1, now = (bands >> band) & 1;
            if (was != now)
            {
                pendingCounts_.emplace_back(cmd, now ? 1u : 0u);
            }
        }
        g.band = band;
    }
    if (std::getenv("XI_LOD_TRACE") && !lodGroups_.empty())
    {
        static auto last = std::chrono::steady_clock::time_point{};
        if (const auto t = std::chrono::steady_clock::now(); t - last > std::chrono::seconds(5))
        {
            last = t;
            int   n[3]{};
            float nearest = 1e30f;
            for (const auto& g : lodGroups_)
            {
                ++n[g.band];
                nearest = std::min(nearest, glm::dot(g.centre - eye, g.centre - eye));
            }
            spdlog::info("lod: {} groups: {} near, {} middle, {} far; eye ({:.1f}, {:.1f}, {:.1f}), nearest {:.1f}", lodGroups_.size(), n[0], n[1], n[2], eye.x,
                         eye.y, eye.z, std::sqrt(nearest));
        }
    }
}

void MeshRenderer::setTransform(uint32_t instance, const glm::mat4& transform)
{
    if (instance < instances_.size())
    {
        pending_.emplace_back(instances_[instance].transform, transform);
    }
}

void MeshRenderer::flush(VkCommandBuffer cmd)
{
    if (!pendingCounts_.empty() && cmdBuffer_.buffer != VK_NULL_HANDLE)
    {
        // LOD changes: the draws' instance counts (0 or 1), between this queue's earlier indirect reads and this frame's.
        VkMemoryBarrier2 before{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        before.srcStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        before.srcAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        before.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers    = &before;
        vkCmdPipelineBarrier2(cmd, &dep);
        for (const auto& [index, count] : pendingCounts_)
        {
            vkCmdUpdateBuffer(cmd, cmdBuffer_.buffer, static_cast<VkDeviceSize>(index) * sizeof(VkDrawIndexedIndirectCommand) + offsetof(VkDrawIndexedIndirectCommand, instanceCount),
                              sizeof(uint32_t), &count);
        }
        pendingCounts_.clear();
        VkMemoryBarrier2 after{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        after.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        after.dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        after.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        dep.pMemoryBarriers = &after;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    if (pending_.empty() || transformBuffer_.buffer == VK_NULL_HANDLE)
    {
        return;
    }
    // Earlier frames may still read the buffer (same queue, so a barrier orders them): reads before the writes, and
    // the writes before this frame's vertex shaders.
    VkMemoryBarrier2 before{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    before.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    before.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    before.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers    = &before;
    vkCmdPipelineBarrier2(cmd, &dep);
    for (const auto& [index, m] : pending_)
    {
        transforms_[index] = m;
        vkCmdUpdateBuffer(cmd, transformBuffer_.buffer, static_cast<VkDeviceSize>(index) * sizeof(glm::mat4), sizeof(glm::mat4), &m);
    }
    pending_.clear();
    VkMemoryBarrier2 after{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    after.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    after.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    after.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    dep.pMemoryBarriers = &after;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void MeshRenderer::upload()
{
    std::vector<VkDrawIndexedIndirectCommand> cmds[2][kCullModes];
    std::vector<DrawData>                     data[2][kCullModes];
    // LOD draws: (bucket, index within it) per group, made global once the buckets are laid out.
    std::vector<std::tuple<uint32_t, int, int, uint32_t, uint8_t>> lodDraws; // group, b, c, index, bands
    for (const auto& inst : instances_)
    {
        for (const auto& s : meshes_[inst.mesh].subMeshes)
        {
            const int b = s.blended ? 1 : 0;
            const int c = s.doubleSided ? 2 : (inst.mirrored ? 1 : 0);
            if (inst.lodGroup != UINT32_MAX)
            {
                lodDraws.emplace_back(inst.lodGroup, b, c, static_cast<uint32_t>(cmds[b][c].size()), inst.bands);
            }
            // Groups start at the near band (updateLod corrects them before the first draw).
            const uint32_t count = inst.lodGroup == UINT32_MAX || (inst.bands & 1) ? 1u : 0u;
            cmds[b][c].push_back({ s.indexCount, count, s.firstIndex, s.vertexOffset, 0 });
            data[b][c].push_back({ inst.transform, s.texture, s.alphaTest ? 1u : 0u, inst.lights });
            stats_.triangles += count ? s.indexCount / 3 : 0;
        }
    }
    std::vector<VkDrawIndexedIndirectCommand> allCmds;
    std::vector<DrawData>                     allData;
    for (int b = 0; b < 2; ++b)
    {
        for (int c = 0; c < kCullModes; ++c)
        {
            buckets_[b][c] = { static_cast<uint32_t>(allCmds.size()), static_cast<uint32_t>(cmds[b][c].size()) };
            allCmds.insert(allCmds.end(), cmds[b][c].begin(), cmds[b][c].end());
            allData.insert(allData.end(), data[b][c].begin(), data[b][c].end());
        }
    }
    stats_.draws = static_cast<uint32_t>(allCmds.size());
    for (const auto& [group, b, c, index, bands] : lodDraws)
    {
        lodGroups_[group].draws.emplace_back(buckets_[b][c].first + index, bands);
    }

    auto up = [&](const auto& v, VkBufferUsageFlags usage) {
        using T = typename std::decay_t<decltype(v)>::value_type;
        const VkDeviceSize     size = std::max<VkDeviceSize>(v.size() * sizeof(T), 16);
        std::vector<std::byte> tmp(size);
        if (!v.empty())
        {
            std::memcpy(tmp.data(), v.data(), v.size() * sizeof(T));
        }
        return ctx_.uploadBuffer(tmp.data(), size, usage);
    };
    constexpr auto ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    vertexBuffer_    = up(vertices_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rtGeometryUsage()); // --rt: also acceleration-structure input
    indexBuffer_     = up(indices_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rtGeometryUsage());
    transformBuffer_ = up(transforms_, ssbo);
    cmdBuffer_       = up(allCmds, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    dataBuffer_      = up(allData, ssbo);
}

void MeshRenderer::draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const SceneParams& scene)
{
    if (stats_.draws == 0)
    {
        return;
    }
    auto& fp = frameParams_[frameSlot % frameParams_.size()];
    auto* gpu = static_cast<FrameParamsGpu*>(fp.mapped);
    gpu->eyeFogStart = glm::vec4(scene.eye, scene.fogStart);
    gpu->fogColorEnd = glm::vec4(scene.fogColor, scene.fogEnd);
    gpu->ambient     = glm::vec4(scene.ambient, 0.0f);
    gpu->light0Dir   = glm::vec4(glm::normalize(scene.light0Dir), 0);
    gpu->light0Color = glm::vec4(scene.light0Color, 0.0f);
    gpu->light1Dir   = glm::vec4(glm::normalize(scene.light1Dir), 0);
    gpu->light1Color = glm::vec4(scene.light1Color, 0.0f);
    for (std::size_t i = 0; i < SceneParams::kMaxPointLights; ++i)
    {
        const auto l          = i < scene.pointLights.size() ? scene.pointLights[i] : SceneParams::PointLight{};
        gpu->points[2 * i]     = glm::vec4(l.pos, l.range);
        gpu->points[2 * i + 1] = glm::vec4(l.colour, 0.0f);
    }
    const auto frameAddr = ctx_.address(fp);
    auto set = textures_.set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, indexBuffer_.buffer, 0, VK_INDEX_TYPE_UINT32);

    const auto transforms = ctx_.address(transformBuffer_);
    const auto drawData   = ctx_.address(dataBuffer_);
    // Opaque first, then blended. The client draws blended pieces unsorted, in file order (§6).
    for (int b = 0; b < 2; ++b)
    {
        for (int c = 0; c < kCullModes; ++c)
        {
            const auto& bucket = buckets_[b][c];
            if (bucket.count == 0)
            {
                continue;
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[b][c]);
            MeshPush p{ viewProj, transforms, drawData + bucket.first * sizeof(DrawData), frameAddr };
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            vkCmdDrawIndexedIndirect(cmd, cmdBuffer_.buffer, bucket.first * sizeof(VkDrawIndexedIndirectCommand), bucket.count,
                                     sizeof(VkDrawIndexedIndirectCommand));
        }
    }
}

std::vector<MeshRenderer::RtPart> MeshRenderer::rtParts(uint32_t mesh) const
{
    std::vector<RtPart> out;
    if (mesh >= meshes_.size())
    {
        return out;
    }
    for (const auto& s : meshes_[mesh].subMeshes)
    {
        if (s.indexCount < 3)
        {
            continue;
        }
        uint32_t hi = 0;
        for (uint32_t i = s.firstIndex; i < s.firstIndex + s.indexCount && i < indices_.size(); ++i)
        {
            hi = std::max(hi, indices_[i]);
        }
        out.push_back({ s.firstIndex, s.indexCount - s.indexCount % 3, s.vertexOffset, hi, s.texture, s.alphaTest, s.blended });
    }
    return out;
}

VkDeviceAddress MeshRenderer::rtVertices() const
{
    return vertexBuffer_.buffer ? ctx_.address(vertexBuffer_) : 0;
}

VkDeviceAddress MeshRenderer::rtIndices() const
{
    return indexBuffer_.buffer ? ctx_.address(indexBuffer_) : 0;
}

void MeshRenderer::rtInstances(const std::function<void(uint32_t mesh, const glm::mat4& transform)>& f) const
{
    if (vertexBuffer_.buffer == VK_NULL_HANDLE)
    {
        return;
    }
    for (const auto& inst : instances_)
    {
        if (inst.lodGroup != UINT32_MAX && inst.lodGroup < lodGroups_.size() && !((inst.bands >> lodGroups_[inst.lodGroup].band) & 1))
        {
            continue;
        }
        f(inst.mesh, transforms_[inst.transform]);
    }
}

} // namespace xi::gfx
