// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/raytracing.h"

#include "gfx/pipeline.h"
#include "gfx/skinned_renderer.h"
#include "gfx/texture_set.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace xi::gfx
{

namespace
{

// The trace / denoise / composite parameters, one block per frame in flight (std430; rt_common.glsl).
struct ParamsGpu
{
    glm::mat4  invViewProj;
    glm::mat4  viewProj;
    glm::mat4  prevViewProj;
    glm::vec4  eye;          // w = frame number
    glm::vec4  light0Dir;
    glm::vec4  light0Color;
    glm::vec4  light1Dir;
    glm::vec4  light1Color;
    glm::vec4  ambient;      // w = strength
    glm::vec4  fogColorEnd;
    glm::vec4  fogStart;     // x
    glm::uvec4 opts;         // x effect bits (1 ao, 2 shadows, 4 points, 8 gi, 16 reflections), y point lights, z frame, w reset
    glm::vec4  size;         // xy pixels, zw 1 / pixels
    glm::vec4  tune;         // x light 0 shadow cap, y reflection weight scale, z bounce weight, w AO radius (goal 41)
    uint64_t   records;
    uint64_t   pad;
    glm::vec4  points[2 * SceneParams::kMaxPointLights];
    glm::vec4  water[2 * RayTracing::kMaxWaterPlanes]; // box (x0, z0, x1, z1), then (y, 0, 0, 0)
};

constexpr VkFormat kTraceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

VkTransformMatrixKHR toVk(const glm::mat4& m)
{
    VkTransformMatrixKHR t{};
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            t.matrix[r][c] = m[c][r];
        }
    }
    return t;
}

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a)
{
    return (v + a - 1) / a * a;
}

VkShaderModule shader(VkDevice d, const char* name)
{
    return loadShader(d, shaderDir() / name);
}

VkPipeline computePipeline(VkDevice d, VkPipelineLayout layout, const char* name)
{
    VkShaderModule              m = shader(d, name);
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

VkDescriptorSetLayout setLayout(VkDevice d, const std::vector<std::pair<VkDescriptorType, VkShaderStageFlags>>& bindings)
{
    std::vector<VkDescriptorSetLayoutBinding> b;
    for (uint32_t i = 0; i < bindings.size(); ++i)
    {
        b.push_back({ i, bindings[i].first, 1, bindings[i].second, nullptr });
    }
    VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = static_cast<uint32_t>(b.size());
    ci.pBindings    = b.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(d, &ci, nullptr, &l));
    return l;
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

// XI_RT_SKIP=actors,trace,denoise,composite: leave parts out (profiling).
bool skip(const char* part)
{
    static const std::string s = std::getenv("XI_RT_SKIP") ? std::getenv("XI_RT_SKIP") : "";
    return s.find(part) != std::string::npos;
}

std::function<void(const void*)>& forgetHook()
{
    static std::function<void(const void*)> f;
    return f;
}

} // namespace

void rtForget(const void* owner)
{
    if (auto& f = forgetHook())
    {
        f(owner);
    }
}

std::optional<RtOptions> parseRtOptions(std::string_view s)
{
    RtOptions o;
    if (s.empty())
    {
        o.ao = o.shadows = o.points = true;
        return o;
    }
    while (!s.empty())
    {
        const auto        comma = s.find(',');
        const std::string w(s.substr(0, comma));
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
        if (w == "ao")
        {
            o.ao = true;
        }
        else if (w == "shadows")
        {
            o.shadows = o.points = true;
        }
        else if (w == "gi")
        {
            o.gi = true;
        }
        else if (w == "reflections")
        {
            o.reflections = true;
        }
        else if (w == "all")
        {
            o.ao = o.shadows = o.points = o.gi = o.reflections = true;
        }
        else
        {
            return std::nullopt;
        }
    }
    return o;
}

bool applyRtBoost(std::string_view level, RtOptions& o)
{
    struct Preset
    {
        std::string_view name;
        float            strength, cap, reflect, bounce, aoRadius;
    };
    // low: full strength and a deeper shadow; high: past full, near-full shadows, a strong bounce; max: everything up.
    static constexpr Preset kPresets[] = { { "low", 1.0f, 0.6f, 1.3f, 0.8f, 2.0f },
                                           { "high", 1.3f, 0.85f, 1.7f, 1.1f, 2.6f },
                                           { "max", 1.6f, 1.0f, 2.2f, 1.5f, 3.2f } };
    const std::string_view want = level.empty() ? "high" : level;
    for (const auto& p : kPresets)
    {
        if (p.name == want)
        {
            o.strength = p.strength, o.shadowCap = p.cap, o.reflect = p.reflect, o.bounce = p.bounce, o.aoRadius = p.aoRadius;
            return true;
        }
    }
    return false;
}

VkBufferUsageFlags& rtGeometryUsage()
{
    static VkBufferUsageFlags u = 0;
    return u;
}

RayTracing::RayTracing(Context& ctx, RtOptions opts)
: ctx_(ctx)
, opts_(opts)
{
    if (!ctx_.rayQuery())
    {
        return;
    }
    load();
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2                        p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &asp;
    vkGetPhysicalDeviceProperties2(ctx_.physicalDevice(), &p2);
    scratchAlign_ = std::max<uint32_t>(asp.minAccelerationStructureScratchOffsetAlignment, 1u);
    rtGeometryUsage() = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    for (auto& f : frames_)
    {
        f.params  = make(sizeof(ParamsGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
        f.records = make(sizeof(Record) * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    }
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx_.device(), &si, nullptr, &sampler_));
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(ctx_.device(), &si, nullptr, &nearest_));
    forgetHook() = [this](const void* owner) { forget(owner); };
    ok_ = true;
    spdlog::info("rt: ray queries on ({}{}{}{}{})", opts_.ao ? "ao " : "", opts_.shadows ? "shadows " : "", opts_.points ? "point-lights " : "",
                 opts_.gi ? "gi " : "", opts_.reflections ? "reflections" : "");
}

RayTracing::~RayTracing()
{
    if (!ok_)
    {
        return;
    }
    ctx_.waitIdle();
    forgetHook() = nullptr;
    rtGeometryUsage() = 0;
    const VkDevice d = ctx_.device();
    for (auto& [k, b] : blas_)
    {
        destroyAs_(d, b->as, nullptr);
        ctx_.destroyBuffer(b->buffer);
    }
    for (auto& [as, buf] : retired_)
    {
        destroyAs_(d, as, nullptr);
        ctx_.destroyBuffer(buf);
    }
    for (auto& [n, buf] : scratchFree_)
    {
        ctx_.destroyBuffer(buf);
    }
    for (auto& f : frames_)
    {
        for (auto as : f.actorBlas)
        {
            destroyAs_(d, as, nullptr);
        }
        if (f.tlas)
        {
            destroyAs_(d, f.tlas, nullptr);
        }
        for (Buffer* b : { &f.instances, &f.tlasBuffer, &f.scratch, &f.records, &f.params, &f.skinned, &f.actorAs, &f.actorScratch })
        {
            if (b->buffer)
            {
                ctx_.destroyBuffer(*b);
            }
        }
    }
    destroyImages();
    vkDestroySampler(d, sampler_, nullptr);
    vkDestroySampler(d, nearest_, nullptr);
    for (auto p : { tracePipe_, denoisePipe_, compPipe_, skinPipe_, guidesPipe_, reflPipe_ })
    {
        vkDestroyPipeline(d, p, nullptr);
    }
    for (auto l : { traceLayout_, denoiseLayout_, compLayout_, skinLayout_, guidesLayout_ })
    {
        vkDestroyPipelineLayout(d, l, nullptr);
    }
    for (auto l : { traceSetLayout_, denoiseSetLayout_, compSetLayout_, textureLayout_, guidesSetLayout_ })
    {
        vkDestroyDescriptorSetLayout(d, l, nullptr);
    }
    vkDestroyDescriptorPool(d, pool_, nullptr);
}

void RayTracing::load()
{
    const VkDevice d = ctx_.device();
    createAs_  = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(d, "vkCreateAccelerationStructureKHR"));
    destroyAs_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(d, "vkDestroyAccelerationStructureKHR"));
    sizes_     = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(d, "vkGetAccelerationStructureBuildSizesKHR"));
    buildAs_   = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(d, "vkCmdBuildAccelerationStructuresKHR"));
    asAddress_ = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(d, "vkGetAccelerationStructureDeviceAddressKHR"));
}

Buffer RayTracing::make(VkDeviceSize size, VkBufferUsageFlags usage, bool host)
{
    return ctx_.createBuffer(std::max<VkDeviceSize>(size, 256), usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host);
}

void RayTracing::forget(const void* owner)
{
    bool any = false;
    for (auto it = blas_.begin(); it != blas_.end();)
    {
        if (it->first.first == owner)
        {
            retired_.emplace_back(it->second->as, it->second->buffer);
            it = blas_.erase(it);
            any = true;
        }
        else
        {
            ++it;
        }
    }
    if (!any)
    {
        return;
    }
    // In-flight frames may still trace them: the zone change waits anyway.
    ctx_.waitIdle();
    for (auto& [as, buf] : retired_)
    {
        destroyAs_(ctx_.device(), as, nullptr);
        ctx_.destroyBuffer(buf);
    }
    retired_.clear();
    std::erase_if(pending_, [&](const PendingBlas& p) { return std::ranges::none_of(blas_, [&](const auto& kv) { return kv.second.get() == p.blas; }); });
    // Re-number the records of what is left.
    std::vector<Record> kept;
    for (auto& [k, b] : blas_)
    {
        const uint32_t first = static_cast<uint32_t>(kept.size());
        kept.insert(kept.end(), records_.begin() + b->records, records_.begin() + b->records + b->count);
        b->records = first;
    }
    records_ = std::move(kept);
    historyValid_ = false;
}

void RayTracing::beginFrame(uint32_t slot)
{
    slot_ = slot % Context::kFramesInFlight;
    instances_.clear();
    actors_.clear();
}

RayTracing::Blas* RayTracing::blasFor(const MeshRenderer& meshes, uint32_t mesh, uint32_t textureSlot)
{
    const auto key = std::make_pair(static_cast<const void*>(&meshes), mesh);
    if (const auto it = blas_.find(key); it != blas_.end())
    {
        return it->second.get();
    }
    const auto parts = meshes.rtParts(mesh);
    auto       b     = std::make_unique<Blas>();
    Blas*      raw   = b.get();
    blas_.emplace(key, std::move(b));
    if (parts.empty())
    {
        return raw;
    }
    PendingBlas pb;
    pb.blas         = raw;
    raw->records    = static_cast<uint32_t>(records_.size());
    raw->count      = static_cast<uint32_t>(parts.size());
    const auto vb   = meshes.rtVertices();
    const auto ib   = meshes.rtIndices();
    std::vector<uint32_t> prims;
    for (const auto& p : parts)
    {
        VkAccelerationStructureGeometryKHR g{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        static const bool allAlpha = std::getenv("XI_RT_ALL_ALPHA") != nullptr; // experiment: every textured piece by its alpha
        g.flags        = p.alphaTest || p.blended || allAlpha ? VkGeometryFlagsKHR{ 0 } : VkGeometryFlagsKHR{ VK_GEOMETRY_OPAQUE_BIT_KHR };
        auto& t        = g.geometry.triangles;
        t.sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t.vertexFormat             = VK_FORMAT_R32G32B32_SFLOAT;
        t.vertexData.deviceAddress = vb + static_cast<VkDeviceAddress>(static_cast<int64_t>(p.vertexOffset) * static_cast<int64_t>(sizeof(Vertex)));
        t.vertexStride             = sizeof(Vertex);
        t.maxVertex                = p.maxVertex;
        t.indexType                = VK_INDEX_TYPE_UINT32;
        t.indexData.deviceAddress  = ib + VkDeviceAddress{ p.firstIndex } * 4u;
        pb.geoms.push_back(g);
        pb.ranges.push_back({ p.indexCount / 3, 0, 0, 0 });
        prims.push_back(p.indexCount / 3);
        Record r;
        r.vertices = t.vertexData.deviceAddress;
        r.indices  = t.indexData.deviceAddress;
        r.texture  = p.texture;
        r.flags    = (p.alphaTest ? 1u : 0u) | (std::min(textureSlot, 2u) << 1) | (p.blended || allAlpha ? 16u : 0u);
        r.stride   = sizeof(Vertex);
        records_.push_back(r);
    }
    VkAccelerationStructureBuildGeometryInfoKHR bi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bi.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.geometryCount = static_cast<uint32_t>(pb.geoms.size());
    bi.pGeometries   = pb.geoms.data();
    VkAccelerationStructureBuildSizesInfoKHR sz{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    sizes_(ctx_.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, prims.data(), &sz);
    raw->buffer = make(sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
    VkAccelerationStructureCreateInfoKHR ci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    ci.buffer = raw->buffer.buffer;
    ci.size   = sz.accelerationStructureSize;
    ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(createAs_(ctx_.device(), &ci, nullptr, &raw->as));
    VkAccelerationStructureDeviceAddressInfoKHR ai{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    ai.accelerationStructure = raw->as;
    raw->address             = asAddress_(ctx_.device(), &ai);
    pb.scratch               = sz.buildScratchSize;
    pending_.push_back(std::move(pb));
    return raw;
}

void RayTracing::pushInstance(const glm::mat4& m, VkDeviceAddress blas, uint32_t record, uint8_t mask, bool opaque)
{
    VkAccelerationStructureInstanceKHR inst{};
    inst.transform                              = toVk(m);
    inst.instanceCustomIndex                    = record & 0xFFFFFFu;
    inst.mask                                   = mask;
    inst.instanceShaderBindingTableRecordOffset = 0;
    // Zone meshes are drawn without culling rules the rays would know: hit both faces.
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | (opaque ? static_cast<uint32_t>(VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR) : 0u);
    inst.accelerationStructureReference = blas;
    instances_.push_back(inst);
}

void RayTracing::addMeshes(const MeshRenderer& meshes, uint32_t textureSlot)
{
    if (!ok_)
    {
        return;
    }
    meshes.rtInstances([&](uint32_t mesh, const glm::mat4& m) {
        if (m[0][0] == 0.0f && m[1][1] == 0.0f && m[2][2] == 0.0f && m[0][1] == 0.0f) // a hidden prop slot
        {
            return;
        }
        const Blas* b = blasFor(meshes, mesh, textureSlot);
        if (b->count > 0)
        {
            pushInstance(m, b->address, b->records, 0x01, false);
        }
    });
}

void RayTracing::addActors(const SkinnedRenderer& skinned, const glm::vec3& eye, float range)
{
    if (!ok_ || skip("actors"))
    {
        return;
    }
    for (const auto& a : skinned.rtActors())
    {
        if (glm::distance(a.origin, eye) > range || a.parts.empty())
        {
            continue;
        }
        ActorJob j;
        j.vertices    = a.vertices;
        j.indices     = a.indices;
        j.bones       = a.bones;
        j.vertexCount = a.vertexCount;
        j.boneBase    = a.boneBase;
        for (const auto& [first, count, offset] : a.parts)
        {
            j.ranges.emplace_back(first, count);
            j.offsets.push_back(offset);
        }
        actors_.push_back(std::move(j));
    }
}

void RayTracing::build(VkCommandBuffer cmd)
{
    if (!ok_)
    {
        return;
    }
    const VkDevice d = ctx_.device();
    auto&          f = frames_[slot_];
    // 1. New static BLASes (the first frames of a zone), in one call.
    if (!pending_.empty())
    {
        VkDeviceSize total = 0;
        for (const auto& p : pending_)
        {
            total += alignUp(p.scratch, scratchAlign_);
        }
        Buffer scratch = make(total + scratchAlign_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        const VkDeviceAddress base = alignUp(address(scratch), scratchAlign_);
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR>     infos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
        VkDeviceSize                                                 off = 0;
        for (auto& p : pending_)
        {
            VkAccelerationStructureBuildGeometryInfoKHR bi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            bi.type                      = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            bi.flags                     = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            bi.mode                      = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            bi.dstAccelerationStructure  = p.blas->as;
            bi.geometryCount             = static_cast<uint32_t>(p.geoms.size());
            bi.pGeometries               = p.geoms.data();
            bi.scratchData.deviceAddress = base + off;
            off += alignUp(p.scratch, scratchAlign_);
            infos.push_back(bi);
            ranges.push_back(p.ranges.data());
            p.blas->built = true;
        }
        buildAs_(cmd, static_cast<uint32_t>(infos.size()), infos.data(), ranges.data());
        const auto alphaParts = std::ranges::count_if(records_, [](const Record& r) { return (r.flags & 17u) != 0u; });
        spdlog::info("rt: built {} BLASes ({:.1f} MB scratch; {} of {} sub-meshes alpha-tested or blended)", infos.size(), static_cast<double>(total) / 1048576.0, alphaParts,
                     records_.size());
        pending_.clear();
        scratchFree_.emplace_back(frame_, scratch); // freed once the GPU is past this frame
    }
    std::erase_if(scratchFree_, [&](auto& s) {
        if (s.first + Context::kFramesInFlight + 1 > frame_)
        {
            return false;
        }
        ctx_.destroyBuffer(s.second);
        return true;
    });

    // 2. The characters: skinned into this slot's buffer, one BLAS each.
    for (auto as : f.actorBlas)
    {
        destroyAs_(d, as, nullptr);
    }
    f.actorBlas.clear();
    frameRecords_ = records_;
    if (!actors_.empty() && skinPipe_ == VK_NULL_HANDLE)
    {
        makePipelines();
    }
    if (!actors_.empty())
    {
        VkDeviceSize verts = 0;
        for (const auto& a : actors_)
        {
            verts += a.vertexCount;
        }
        if (f.skinned.size < verts * 12)
        {
            if (f.skinned.buffer)
            {
                ctx_.destroyBuffer(f.skinned);
            }
            f.skinned = make(verts * 12 * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skinPipe_);
        struct SkinPush
        {
            VkDeviceAddress verts, bones, out;
            uint32_t        boneBase, count;
        };
        VkDeviceSize                                                  at = 0;
        std::vector<std::vector<VkAccelerationStructureGeometryKHR>>  geoms;
        std::vector<std::vector<VkAccelerationStructureBuildRangeInfoKHR>> rng;
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR>      infos;
        std::vector<VkDeviceSize>                                     asSize, scratchSize;
        for (const auto& a : actors_)
        {
            const VkDeviceAddress out = address(f.skinned) + at * 12;
            SkinPush              p{ a.vertices, a.bones, out, a.boneBase, a.vertexCount };
            vkCmdPushConstants(cmd, skinLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
            vkCmdDispatch(cmd, (a.vertexCount + 63) / 64, 1, 1);
            auto& g = geoms.emplace_back();
            auto& r = rng.emplace_back();
            std::vector<uint32_t> prims;
            const uint32_t        recordBase = static_cast<uint32_t>(frameRecords_.size());
            for (std::size_t k = 0; k < a.ranges.size(); ++k)
            {
                VkAccelerationStructureGeometryKHR geo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
                geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                geo.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;
                auto& t          = geo.geometry.triangles;
                t.sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                t.vertexFormat             = VK_FORMAT_R32G32B32_SFLOAT;
                t.vertexData.deviceAddress = out + static_cast<VkDeviceAddress>(static_cast<int64_t>(a.offsets[k]) * 12);
                t.vertexStride             = 12;
                t.maxVertex                = a.vertexCount - 1 - static_cast<uint32_t>(std::max(a.offsets[k], 0));
                t.indexType                = VK_INDEX_TYPE_UINT32;
                t.indexData.deviceAddress  = a.indices + VkDeviceAddress{ a.ranges[k].first } * 4u;
                g.push_back(geo);
                r.push_back({ a.ranges[k].second / 3, 0, 0, 0 });
                prims.push_back(a.ranges[k].second / 3);
                Record rec;
                rec.vertices = t.vertexData.deviceAddress;
                rec.indices  = t.indexData.deviceAddress;
                rec.flags    = (2u << 1) | 8u; // no texture set, positions only
                rec.stride   = 12;
                frameRecords_.push_back(rec);
            }
            VkAccelerationStructureBuildGeometryInfoKHR bi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            bi.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            bi.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
            bi.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            bi.geometryCount = static_cast<uint32_t>(g.size());
            bi.pGeometries   = g.data();
            VkAccelerationStructureBuildSizesInfoKHR sz{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
            sizes_(d, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, prims.data(), &sz);
            infos.push_back(bi);
            asSize.push_back(alignUp(sz.accelerationStructureSize, 256));
            scratchSize.push_back(alignUp(sz.buildScratchSize, scratchAlign_));
            instances_.push_back({}); // placeholder: filled once the BLAS address is known
            instances_.back().instanceCustomIndex = recordBase & 0xFFFFFFu;
            at += a.vertexCount;
        }
        VkDeviceSize asTotal = 0, scTotal = 0;
        for (std::size_t i = 0; i < infos.size(); ++i)
        {
            asTotal += asSize[i];
            scTotal += scratchSize[i];
        }
        if (f.actorAs.size < asTotal)
        {
            if (f.actorAs.buffer)
            {
                ctx_.destroyBuffer(f.actorAs);
            }
            f.actorAs = make(asTotal * 2, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        }
        if (f.actorScratch.size < scTotal + scratchAlign_)
        {
            if (f.actorScratch.buffer)
            {
                ctx_.destroyBuffer(f.actorScratch);
            }
            f.actorScratch = make((scTotal + scratchAlign_) * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT);
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rp;
        VkDeviceSize                                                 aoff = 0;
        VkDeviceAddress                                              soff = alignUp(address(f.actorScratch), scratchAlign_);
        const std::size_t                                            firstActor = instances_.size() - infos.size();
        for (std::size_t i = 0; i < infos.size(); ++i)
        {
            VkAccelerationStructureCreateInfoKHR ci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
            ci.buffer = f.actorAs.buffer;
            ci.offset = aoff;
            ci.size   = asSize[i];
            ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            VkAccelerationStructureKHR as = VK_NULL_HANDLE;
            VK_CHECK(createAs_(d, &ci, nullptr, &as));
            f.actorBlas.push_back(as);
            infos[i].dstAccelerationStructure  = as;
            infos[i].scratchData.deviceAddress = soff;
            infos[i].pGeometries               = geoms[i].data();
            rp.push_back(rng[i].data());
            aoff += asSize[i];
            soff += scratchSize[i];
            VkAccelerationStructureDeviceAddressInfoKHR ai{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
            ai.accelerationStructure = as;
            auto& inst               = instances_[firstActor + i];
            const uint32_t rec       = inst.instanceCustomIndex;
            inst                     = {};
            inst.transform           = toVk(glm::mat4(1.0f));
            inst.instanceCustomIndex = rec & 0xFFFFFFu;
            inst.mask                = 0x02;
            inst.flags               = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
            inst.accelerationStructureReference = asAddress_(d, &ai);
        }
        buildAs_(cmd, static_cast<uint32_t>(infos.size()), infos.data(), rp.data());
    }

    // 3. The geometry records (static + this frame's characters).
    if (f.records.size < frameRecords_.size() * sizeof(Record))
    {
        ctx_.destroyBuffer(f.records);
        f.records = make(frameRecords_.size() * sizeof(Record) * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }
    if (!frameRecords_.empty())
    {
        std::memcpy(f.records.mapped, frameRecords_.data(), frameRecords_.size() * sizeof(Record));
    }

    // 4. The TLAS over everything.
    const uint32_t count = static_cast<uint32_t>(instances_.size());
    if (f.instances.size < std::max<VkDeviceSize>(count, 1) * sizeof(VkAccelerationStructureInstanceKHR))
    {
        if (f.instances.buffer)
        {
            ctx_.destroyBuffer(f.instances);
        }
        f.instances = make(std::max<VkDeviceSize>(count, 64) * 2 * sizeof(VkAccelerationStructureInstanceKHR),
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
    }
    if (count)
    {
        std::memcpy(f.instances.mapped, instances_.data(), count * sizeof(VkAccelerationStructureInstanceKHR));
    }
    VkAccelerationStructureGeometryKHR tg{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tg.geometryType                          = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tg.geometry.instances.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tg.geometry.instances.data.deviceAddress = address(f.instances);
    VkAccelerationStructureBuildGeometryInfoKHR tb{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    tb.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tb.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tb.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tb.geometryCount = 1;
    tb.pGeometries   = &tg;
    if (f.tlas == VK_NULL_HANDLE || f.tlasCapacity < count)
    {
        const uint32_t cap = std::max<uint32_t>(count * 2, 256);
        VkAccelerationStructureBuildSizesInfoKHR sz{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        sizes_(d, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tb, &cap, &sz);
        if (f.tlas)
        {
            ctx_.waitIdle();
            destroyAs_(d, f.tlas, nullptr);
            ctx_.destroyBuffer(f.tlasBuffer);
            ctx_.destroyBuffer(f.scratch);
        }
        f.tlasBuffer = make(sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        f.scratch    = make(sz.buildScratchSize + scratchAlign_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        VkAccelerationStructureCreateInfoKHR ci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        ci.buffer = f.tlasBuffer.buffer;
        ci.size   = sz.accelerationStructureSize;
        ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VK_CHECK(createAs_(d, &ci, nullptr, &f.tlas));
        f.tlasCapacity = cap;
    }
    tb.dstAccelerationStructure  = f.tlas;
    tb.scratchData.deviceAddress = alignUp(address(f.scratch), scratchAlign_);
    barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_HOST_WRITE_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    VkAccelerationStructureBuildRangeInfoKHR        tr{ count, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
    buildAs_(cmd, 1, &tb, &trp);
    barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    ++frame_;
}

void RayTracing::destroyImages()
{
    for (Image* i : { &rawA_, &rawB_, &rawC_, &rawD_, &histA_[0], &histA_[1], &histB_[0], &histB_[1], &histC_[0], &histC_[1], &filtA_, &filtB_, &filtC_,
                      &sceneCopy_, &guideAlbedo_, &guideSpec_, &guideNormal_ })
    {
        if (i->image)
        {
            ctx_.destroyImage(*i);
            *i = {};
        }
    }
    extent_ = {};
}

void RayTracing::ensureImages(VkExtent2D ext)
{
    if (ext.width == extent_.width && ext.height == extent_.height)
    {
        return;
    }
    ctx_.waitIdle();
    destroyImages();
    constexpr VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (Image* i : { &rawA_, &rawB_, &rawC_, &rawD_, &histA_[0], &histA_[1], &histB_[0], &histB_[1], &histC_[0], &histC_[1], &filtA_, &filtB_, &filtC_ })
    {
        *i = ctx_.createImage(ext, kTraceFormat, u, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    sceneCopy_ = ctx_.createImage(ext, ctx_.colorFormat(), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (rr_)
    {
        guideAlbedo_ = ctx_.createImage(ext, VK_FORMAT_R8G8B8A8_UNORM, u, VK_IMAGE_ASPECT_COLOR_BIT);
        guideSpec_   = ctx_.createImage(ext, VK_FORMAT_R8G8B8A8_UNORM, u, VK_IMAGE_ASPECT_COLOR_BIT);
        guideNormal_ = ctx_.createImage(ext, kTraceFormat, u, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    ctx_.immediateSubmit([&](VkCommandBuffer cmd) {
        for (Image* i : { &rawA_, &rawB_, &rawC_, &rawD_, &histA_[0], &histA_[1], &histB_[0], &histB_[1], &histC_[0], &histC_[1], &filtA_, &filtB_, &filtC_ })
        {
            Context::transition(cmd, i->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        }
        Context::transition(cmd, sceneCopy_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (Image* i : { &guideAlbedo_, &guideSpec_, &guideNormal_ })
        {
            if (i->image)
            {
                Context::transition(cmd, i->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            }
        }
    });
    extent_       = ext;
    historyValid_ = false;
}

void RayTracing::makePipelines()
{
    const VkDevice d = ctx_.device();
    if (pool_ == VK_NULL_HANDLE)
    {
        constexpr auto C = VK_SHADER_STAGE_COMPUTE_BIT, F = VK_SHADER_STAGE_FRAGMENT_BIT;
        traceSetLayout_   = setLayout(d, { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, C },
                                           { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, C },
                                           { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, C },
                                           { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, C },
                                           { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, C },
                                           { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, C } });
        std::vector<std::pair<VkDescriptorType, VkShaderStageFlags>> dn;
        for (int i = 0; i < 6; ++i)
        {
            dn.emplace_back(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, C); // 0-2 input, 3-5 history
        }
        for (int i = 0; i < 3; ++i)
        {
            dn.emplace_back(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, C); // 6-8 out
        }
        dn.emplace_back(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, C); // 9 depth
        denoiseSetLayout_ = setLayout(d, dn);
        compSetLayout_    = setLayout(d, { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F },
                                           { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F },
                                           { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F },
                                           { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F },
                                           { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F },
                                           { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F } });
        const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 8 },
                                               { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 96 },
                                               { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 48 } };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets       = 16;
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
        for (auto& s : traceSets_)
        {
            s = alloc(traceSetLayout_);
        }
        for (auto& s : compSets_)
        {
            s = alloc(compSetLayout_);
        }
        for (auto& s : denoiseSets_)
        {
            s = alloc(denoiseSetLayout_);
        }
        VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        lci.setLayoutCount         = 1;
        lci.pSetLayouts            = &denoiseSetLayout_;
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges    = &pr;
        VK_CHECK(vkCreatePipelineLayout(d, &lci, nullptr, &denoiseLayout_));
        denoisePipe_ = computePipeline(d, denoiseLayout_, "rt_denoise.comp.spv");
        VkPushConstantRange sr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
        lci.setLayoutCount      = 0;
        lci.pSetLayouts         = nullptr;
        lci.pPushConstantRanges = &sr;
        VK_CHECK(vkCreatePipelineLayout(d, &lci, nullptr, &skinLayout_));
        skinPipe_ = computePipeline(d, skinLayout_, "rt_skin.comp.spv");
        VkPushConstantRange cr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(VkDeviceAddress) }; // the params address (all of it is set: #139)
        lci.setLayoutCount      = 1;
        lci.pSetLayouts         = &compSetLayout_;
        lci.pPushConstantRanges = &cr;
        VK_CHECK(vkCreatePipelineLayout(d, &lci, nullptr, &compLayout_));
        {
            constexpr auto GC = VK_SHADER_STAGE_COMPUTE_BIT;
            guidesSetLayout_ = setLayout(d, { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GC },
                                              { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GC },
                                              { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GC },
                                              { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, GC },
                                              { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, GC },
                                              { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, GC } });
            for (auto& gs : guidesSets_)
            {
                gs = alloc(guidesSetLayout_);
            }
            VkPushConstantRange        gr{ GC, 0, 16 };
            VkPipelineLayoutCreateInfo gl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            gl.setLayoutCount         = 1;
            gl.pSetLayouts            = &guidesSetLayout_;
            gl.pushConstantRangeCount = 1;
            gl.pPushConstantRanges    = &gr;
            VK_CHECK(vkCreatePipelineLayout(d, &gl, nullptr, &guidesLayout_));
            guidesPipe_ = computePipeline(d, guidesLayout_, "rt_guides.comp.spv");
        }
        PipelineDesc pd;
        pd.vert        = shader(d, "rt_composite.vert.spv");
        pd.frag        = shader(d, "rt_composite.frag.spv");
        pd.layout      = compLayout_;
        pd.colorFormat = ctx_.colorFormat();
        pd.depthFormat = VK_FORMAT_UNDEFINED;
        compPipe_      = createPipeline(d, pd, false);
        vkDestroyShaderModule(d, pd.frag, nullptr);
        pd.frag       = shader(d, "rt_reflect.frag.spv"); // the water reflection, alpha-blended over the finished scene
        pd.alphaBlend = true;
        reflPipe_     = createPipeline(d, pd, false);
        vkDestroyShaderModule(d, pd.vert, nullptr);
        vkDestroyShaderModule(d, pd.frag, nullptr);
    }
    if (tracePipe_ == VK_NULL_HANDLE)
    {
        // The texture sets' layout, defined as TextureSet defines it (sets from any zone's are compatible with it; the
        // zones' own layouts go away with them).
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
        const VkDescriptorSetLayout sets[] = { traceSetLayout_, textureLayout_, textureLayout_ };
        VkPushConstantRange         pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo  lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        lci.setLayoutCount         = 3;
        lci.pSetLayouts            = sets;
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges    = &pr;
        VK_CHECK(vkCreatePipelineLayout(d, &lci, nullptr, &traceLayout_));
        tracePipe_ = computePipeline(d, traceLayout_, "rt_trace.comp.spv");
    }
}

void RayTracing::trace(VkCommandBuffer cmd, const Image& depth, VkExtent2D ext, const View& view, const SceneParams& sp, VkDescriptorSet sceneTextures,
                       VkDescriptorSet subTextures, VkDescriptorSetLayout textureLayout)
{
    if (!ok_ || frames_[slot_].tlas == VK_NULL_HANDLE)
    {
        return;
    }
    (void)textureLayout;
    makePipelines();
    ensureImages(ext);
    const VkDevice d = ctx_.device();
    auto&          f = frames_[slot_];
    const uint32_t cur = static_cast<uint32_t>(frame_ & 1u), prev = cur ^ 1u;

    // Parameters.
    auto* p          = static_cast<ParamsGpu*>(f.params.mapped);
    p->invViewProj   = glm::inverse(view.viewProj);
    p->viewProj      = view.viewProj;
    p->prevViewProj  = view.prevViewProj;
    p->eye           = glm::vec4(view.eye, static_cast<float>(frame_ & 0xFFFF));
    p->light0Dir     = glm::vec4(glm::normalize(sp.light0Dir), 0.0f);
    p->light0Color   = glm::vec4(sp.light0Color, 0.0f);
    p->light1Dir     = glm::vec4(glm::normalize(sp.light1Dir), 0.0f);
    p->light1Color   = glm::vec4(sp.light1Color, 0.0f);
    p->ambient       = glm::vec4(sp.ambient, opts_.strength);
    p->fogColorEnd   = glm::vec4(sp.fogColor, sp.fogEnd);
    const std::size_t nWater = view.water ? std::min(view.water->size() / 2, kMaxWaterPlanes) : 0;
    p->fogStart      = glm::vec4(sp.fogStart, view.sunShadow, static_cast<float>(nWater), 0);
    for (std::size_t i = 0; i < 2 * nWater; ++i)
    {
        p->water[i] = (*view.water)[i];
    }
    const bool reset = view.reset || !historyValid_;
    // Debug XI_RT_DEBUG=reflect: the composite tints where the water reflection lands (red = its weight).
    // XI_RT_DEBUG=reflectcolor: the reflected colour alone, at full weight.
    static const std::string_view dbg          = std::getenv("XI_RT_DEBUG") ? std::getenv("XI_RT_DEBUG") : "";
    static const bool             debugReflect = dbg == "reflect", debugColour = dbg == "reflectcolor";
    p->opts = glm::uvec4((opts_.ao ? 1u : 0u) | (opts_.shadows ? 2u : 0u) | (opts_.points ? 4u : 0u) | (opts_.gi ? 8u : 0u) | (opts_.reflections ? 16u : 0u) |
                             (debugReflect ? 32u : 0u) | (debugColour ? 64u : 0u),
                         static_cast<uint32_t>(std::min(sp.pointLights.size(), SceneParams::kMaxPointLights)), static_cast<uint32_t>(frame_), reset ? 1u : 0u);
    p->size    = glm::vec4(ext.width, ext.height, 1.0f / static_cast<float>(ext.width), 1.0f / static_cast<float>(ext.height));
    p->tune    = glm::vec4(opts_.shadowCap, opts_.reflect, opts_.bounce, opts_.aoRadius);
    p->records = address(f.records);
    for (std::size_t i = 0; i < SceneParams::kMaxPointLights; ++i)
    {
        const auto l       = i < sp.pointLights.size() ? sp.pointLights[i] : SceneParams::PointLight{};
        p->points[2 * i]     = glm::vec4(l.pos, l.range);
        p->points[2 * i + 1] = glm::vec4(l.colour, 0.0f);
    }
    const VkDeviceAddress params = address(f.params);

    // Descriptors.
    {
        VkWriteDescriptorSetAccelerationStructureKHR asw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
        asw.accelerationStructureCount = 1;
        asw.pAccelerationStructures    = &f.tlas;
        VkDescriptorImageInfo depthInfo{ nearest_, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo outs[4]{ { VK_NULL_HANDLE, rawA_.view, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, rawB_.view, VK_IMAGE_LAYOUT_GENERAL },
                                       { VK_NULL_HANDLE, rawC_.view, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, rawD_.view, VK_IMAGE_LAYOUT_GENERAL } };
        VkWriteDescriptorSet w[6]{};
        for (auto& x : w)
        {
            x.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            x.dstSet          = traceSets_[slot_];
            x.descriptorCount = 1;
        }
        w[0].pNext          = &asw;
        w[0].dstBinding     = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        w[1].dstBinding     = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo     = &depthInfo;
        for (int i = 0; i < 4; ++i)
        {
            w[2 + i].dstBinding     = static_cast<uint32_t>(2 + i);
            w[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[2 + i].pImageInfo     = &outs[i];
        }
        vkUpdateDescriptorSets(d, 6, w, 0, nullptr);
    }
    const Image* hist[3][2] = { { &histA_[0], &histA_[1] }, { &histB_[0], &histB_[1] }, { &histC_[0], &histC_[1] } };
    const Image* raws[3]    = { &rawA_, &rawB_, &rawC_ };
    const Image* filts[3]   = { &filtA_, &filtB_, &filtC_ };
    for (int pass = 0; pass < 2; ++pass)
    {
        VkDescriptorImageInfo in[10]{};
        for (int i = 0; i < 3; ++i)
        {
            in[i]     = { sampler_, pass == 0 ? raws[i]->view : hist[i][cur]->view, VK_IMAGE_LAYOUT_GENERAL };
            in[3 + i] = { sampler_, hist[i][prev]->view, VK_IMAGE_LAYOUT_GENERAL };
            in[6 + i] = { VK_NULL_HANDLE, pass == 0 ? hist[i][cur]->view : filts[i]->view, VK_IMAGE_LAYOUT_GENERAL };
        }
        in[9] = { nearest_, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[10]{};
        for (uint32_t i = 0; i < 10; ++i)
        {
            w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet          = denoiseSets_[slot_ * 2 + static_cast<uint32_t>(pass)];
            w[i].dstBinding      = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType  = i >= 6 && i < 9 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo      = &in[i];
        }
        vkUpdateDescriptorSets(d, 10, w, 0, nullptr);
    }

    // Trace.
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    const VkDescriptorSet sets[] = { traceSets_[slot_], sceneTextures, subTextures ? subTextures : sceneTextures };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, traceLayout_, 0, 3, sets, 0, nullptr);
    struct Push
    {
        VkDeviceAddress params;
        uint32_t        pass, pad;
    };
    Push push{ params, 0, 0 };
    vkCmdPushConstants(cmd, traceLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    if (!skip("trace"))
    {
        vkCmdDispatch(cmd, (ext.width + 7) / 8, (ext.height + 7) / 8, 1);
    }
    // Denoise: temporal into the history, then a spatial filter into the result.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoisePipe_);
    for (uint32_t pass = 0; pass < 2; ++pass)
    {
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoiseLayout_, 0, 1, &denoiseSets_[slot_ * 2 + pass], 0, nullptr);
        Push dp{ params, pass, 0 };
        vkCmdPushConstants(cmd, denoiseLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dp), &dp);
        if (!skip("denoise") && !rr_) // Ray Reconstruction denoises the raw terms itself
        {
            vkCmdDispatch(cmd, (ext.width + 7) / 8, (ext.height + 7) / 8, 1);
        }
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);
    historyValid_ = true;
}

void RayTracing::reflect(VkCommandBuffer cmd, VkImageView colorMip0, VkExtent2D ext)
{
    if (!ok_ || !opts_.reflections || reflPipe_ == VK_NULL_HANDLE || extent_.width != ext.width || extent_.height != ext.height || skip("reflect"))
    {
        return;
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, reflPipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compLayout_, 0, 1, &compSets_[slot_], 0, nullptr); // set by composite
    const VkDeviceAddress pa = address(frames_[slot_].params);
    vkCmdPushConstants(cmd, compLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pa), &pa);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void RayTracing::composite(VkCommandBuffer cmd, const Image& color, VkImageView colorMip0, const Image& depth, VkExtent2D ext, const View& view,
                           const SceneParams& sp)
{
    (void)view;
    (void)sp;
    if (!ok_ || compPipe_ == VK_NULL_HANDLE || extent_.width != ext.width || extent_.height != ext.height || skip("composite"))
    {
        return;
    }
    auto& f = frames_[slot_];
    // The scene as drawn, to read while writing the result over it.
    Context::transition(cmd, color.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    Context::transition(cmd, sceneCopy_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{};
    c.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.extent         = { ext.width, ext.height, 1 };
    vkCmdCopyImage(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sceneCopy_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Context::transition(cmd, sceneCopy_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Context::transition(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);

    VkDescriptorImageInfo in[6]{ { nearest_, sceneCopy_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                 { nearest_, (rr_ ? rawA_ : filtA_).view, VK_IMAGE_LAYOUT_GENERAL },
                                 { nearest_, (rr_ ? rawB_ : filtB_).view, VK_IMAGE_LAYOUT_GENERAL },
                                 { nearest_, (rr_ ? rawC_ : filtC_).view, VK_IMAGE_LAYOUT_GENERAL },
                                 { nearest_, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                 { nearest_, rawD_.view, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet w[6]{};
    for (uint32_t i = 0; i < 6; ++i)
    {
        w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet          = compSets_[slot_];
        w[i].dstBinding      = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo      = &in[i];
    }
    vkUpdateDescriptorSets(ctx_.device(), 6, w, 0, nullptr);

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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compLayout_, 0, 1, &compSets_[slot_], 0, nullptr);
    const VkDeviceAddress pa = address(f.params);
    vkCmdPushConstants(cmd, compLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pa), &pa);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (!rr_ || !guideAlbedo_.image)
    {
        return;
    }
    // The Ray Reconstruction guides.
    VkDescriptorImageInfo gi[6]{ { nearest_, sceneCopy_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                 { nearest_, rawA_.view, VK_IMAGE_LAYOUT_GENERAL },
                                 { nearest_, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                 { VK_NULL_HANDLE, guideAlbedo_.view, VK_IMAGE_LAYOUT_GENERAL },
                                 { VK_NULL_HANDLE, guideNormal_.view, VK_IMAGE_LAYOUT_GENERAL },
                                 { VK_NULL_HANDLE, guideSpec_.view, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet gw[6]{};
    for (uint32_t i = 0; i < 6; ++i)
    {
        gw[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        gw[i].dstSet          = guidesSets_[slot_];
        gw[i].dstBinding      = i;
        gw[i].descriptorCount = 1;
        gw[i].descriptorType  = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        gw[i].pImageInfo      = &gi[i];
    }
    vkUpdateDescriptorSets(ctx_.device(), 6, gw, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, guidesPipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, guidesLayout_, 0, 1, &guidesSets_[slot_], 0, nullptr);
    struct Push
    {
        VkDeviceAddress params;
        uint32_t        pass, pad;
    } gp{ pa, 0, 0 };
    vkCmdPushConstants(cmd, guidesLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gp), &gp);
    vkCmdDispatch(cmd, (ext.width + 7) / 8, (ext.height + 7) / 8, 1);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
}

} // namespace xi::gfx
