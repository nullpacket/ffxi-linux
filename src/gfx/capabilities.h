// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

// What this machine's GPU can do, found without a window or a device: `xi-client --capabilities` prints it for the
// launcher (goal 44), which greys out the options a build or GPU lacks.
namespace xi::gfx
{

struct GpuCapabilities
{
    bool        vulkan = false;    // a Vulkan instance and at least one physical device
    std::string gpu;               // the chosen device's name (discrete first, as the client picks)
    uint32_t    vendorId = 0;      // PCI vendor (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel)
    std::string driver;            // the driver's name and version info, when the driver reports them
    bool        rayQuery =
        false; // VK_KHR_acceleration_structure + VK_KHR_ray_query + shaderInt64 (--rt, --rt-boost, --pathtrace)
    bool        nvidia   = false;  // DLSS needs an NVIDIA RTX GPU (and a DLSS build)
    std::string pretend; // XI_GPU_PRETEND's vendor when set ("amd", "intel"): the probe answers as that vendor
    std::vector<std::string> missing;     // required features the GPU lacks (the client will not start on it)
    bool                     fsr = false; // FSR 3.1 can run (any vendor; gfx/fsr.h)
};

// PCI vendor ids.
inline constexpr uint32_t kVendorNvidia = 0x10DE, kVendorAmd = 0x1002, kVendorIntel = 0x8086;

// XI_GPU_PRETEND=amd|intel (issue #139): there is no AMD or Intel GPU to test on, so the client can treat the GPU it
// has as another vendor's. The vendor id changes (DLSS goes away, as on a real AMD card), the limits the client checks
// against are clamped to that vendor's (gpuLimitsFor), and features AMD's or Intel's drivers lack are reported missing.
struct GpuPretend
{
    std::string vendor;       // "" (off), "amd" or "intel"
    uint32_t    vendorId = 0; // kVendorAmd / kVendorIntel
};
[[nodiscard]] GpuPretend gpuPretendFromEnv();
[[nodiscard]] GpuPretend gpuPretendFrom(const char* value); // "amd", "intel", anything else = off

// The device limits the client depends on. clampLimits keeps the smaller of the device's and a pretended vendor's.
struct GpuLimits
{
    uint32_t maxPushConstantsSize           = 0;
    uint32_t maxPerStageSampledImages       = 0; // update-after-bind sampled images per stage (the bindless arrays)
    uint32_t maxComputeWorkGroupInvocations = 0;
    uint32_t maxImageDimension2D            = 0;
    uint32_t minSubgroupSize = 0, maxSubgroupSize = 0; // VK_EXT_subgroup_size_control / Vulkan 1.3 (FSR's wave64 path)
};
// What an RX 6800 (RDNA2, AMD's own driver; the tighter of AMDVLK's and RADV's published values) and an Arc A770
// report: the limits the masquerade clamps to.
[[nodiscard]] GpuLimits gpuLimitsFor(uint32_t vendorId);
[[nodiscard]] GpuLimits clampLimits(const GpuLimits& device, const GpuLimits& pretend);
[[nodiscard]] GpuLimits readLimits(VkPhysicalDevice pd);

// What the client needs and what it can use, for one physical device: gfx::Context picks with it, probeGpu reports it.
struct DeviceSupport
{
    std::vector<std::string> missing; // required features / limits not there: the device can't run the client
    bool                     rayQuery        = false; // acceleration structures + ray queries + shaderInt64
    bool                     subgroupControl = false; // computeFullSubgroups + subgroupSizeControl (FSR's wave64)
    bool                     float16         = false; // shaderFloat16 + shaderInt16 (FSR's 16-bit passes)
    bool                     fsr = false; // what FSR 3.1's shaders need (storage-image formats, quad subgroups)
    std::string              fsrMissing;  // why not, when not
    GpuLimits                limits;      // clamped by the pretend, when set
};
[[nodiscard]] DeviceSupport checkDevice(VkPhysicalDevice pd, const GpuPretend& pretend);

// The push-constant bytes the client's biggest pipeline layout uses (post.cpp's PostPush); checked against the limit.
inline constexpr uint32_t kClientPushConstantBytes = 128;
// The bindless texture arrays a pass binds per stage (TextureSet::kMaxTextures, two in the ray-traced passes).
inline constexpr uint32_t kClientSampledImagesPerStage = 2 * 4096 + 16;

// Probes the first suitable physical device with a throw-away Vulkan instance. Never throws.
[[nodiscard]] GpuCapabilities probeGpu();

} // namespace xi::gfx
