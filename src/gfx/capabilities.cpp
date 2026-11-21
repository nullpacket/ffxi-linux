// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/capabilities.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xi::gfx
{

GpuPretend gpuPretendFrom(const char* value)
{
    const std::string_view v = value ? value : "";
    if (v == "amd")
    {
        return { "amd", kVendorAmd };
    }
    if (v == "intel")
    {
        return { "intel", kVendorIntel };
    }
    return {};
}

GpuPretend gpuPretendFromEnv()
{
    return gpuPretendFrom(std::getenv("XI_GPU_PRETEND"));
}

GpuLimits gpuLimitsFor(uint32_t vendorId)
{
    GpuLimits l;
    if (vendorId == kVendorAmd)
    {
        // RX 6800: AMDVLK / AMD's proprietary driver report 128 push-constant bytes (RADV 256), wave32 and wave64.
        l.maxPushConstantsSize           = 128;
        l.maxPerStageSampledImages       = 8388606; // RADV (AMDVLK reports 2^32 - 1)
        l.maxComputeWorkGroupInvocations = 1024;
        l.maxImageDimension2D            = 16384;
        l.minSubgroupSize                = 32;
        l.maxSubgroupSize                = 64;
    }
    else if (vendorId == kVendorIntel)
    {
        // Arc A770 (ANV): SIMD8 to SIMD32.
        l.maxPushConstantsSize           = 256;
        l.maxPerStageSampledImages       = 1048576;
        l.maxComputeWorkGroupInvocations = 1024;
        l.maxImageDimension2D            = 16384;
        l.minSubgroupSize                = 8;
        l.maxSubgroupSize                = 32;
    }
    return l;
}

GpuLimits clampLimits(const GpuLimits& device, const GpuLimits& pretend)
{
    const auto lo = [](uint32_t a, uint32_t b) { return b == 0 ? a : std::min(a, b); };
    GpuLimits  l;
    l.maxPushConstantsSize     = lo(device.maxPushConstantsSize, pretend.maxPushConstantsSize);
    l.maxPerStageSampledImages = lo(device.maxPerStageSampledImages, pretend.maxPerStageSampledImages);
    l.maxComputeWorkGroupInvocations =
        lo(device.maxComputeWorkGroupInvocations, pretend.maxComputeWorkGroupInvocations);
    l.maxImageDimension2D = lo(device.maxImageDimension2D, pretend.maxImageDimension2D);
    // The subgroup range is the pretended vendor's when the device can be asked for it (a 4090 runs 32 only, so a
    // wave64 request is clamped back by the driver; the client only uses it through subgroup size control).
    l.minSubgroupSize = pretend.minSubgroupSize ? pretend.minSubgroupSize : device.minSubgroupSize;
    l.maxSubgroupSize = pretend.maxSubgroupSize ? pretend.maxSubgroupSize : device.maxSubgroupSize;
    return l;
}

GpuLimits readLimits(VkPhysicalDevice pd)
{
    VkPhysicalDeviceVulkan13Properties p13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization): an output struct, the driver fills driverID
    VkPhysicalDeviceVulkan12Properties p12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2        p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p12.pNext = &p13;
    p2.pNext  = &p12;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    GpuLimits l;
    l.maxPushConstantsSize           = p2.properties.limits.maxPushConstantsSize;
    l.maxPerStageSampledImages       = p12.maxPerStageDescriptorUpdateAfterBindSampledImages;
    l.maxComputeWorkGroupInvocations = p2.properties.limits.maxComputeWorkGroupInvocations;
    l.maxImageDimension2D            = p2.properties.limits.maxImageDimension2D;
    l.minSubgroupSize                = p13.minSubgroupSize;
    l.maxSubgroupSize                = p13.maxSubgroupSize;
    return l;
}

DeviceSupport checkDevice(VkPhysicalDevice pd, const GpuPretend& pretend)
{
    DeviceSupport              s;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    if (props.apiVersion < VK_API_VERSION_1_3)
    {
        s.missing.emplace_back("Vulkan 1.3");
        return s;
    }
    VkPhysicalDeviceVulkan13Features                 f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features                 f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features                 f11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fas{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
    };
    VkPhysicalDeviceRayQueryFeaturesKHR frq{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceFeatures2           f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    uint32_t                            n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
    const auto has = [&](std::string_view e) {
        return std::ranges::any_of(exts, [&](const VkExtensionProperties& p) { return e == p.extensionName; });
    };
    const bool rtExts = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                        has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    f13.pNext         = &f12;
    f12.pNext         = &f11;
    f11.pNext         = rtExts ? static_cast<void*>(&fas) : nullptr;
    fas.pNext         = &frq;
    f2.pNext          = &f13;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    // gfx::Context enables every one of these.
    const std::pair<const char*, VkBool32> need[] = {
        { "dynamicRendering", f13.dynamicRendering },
        { "synchronization2", f13.synchronization2 },
        { "shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation },
        { "shaderDrawParameters", f11.shaderDrawParameters },
        { "descriptorIndexing", f12.descriptorIndexing },
        { "runtimeDescriptorArray", f12.runtimeDescriptorArray },
        { "descriptorBindingPartiallyBound", f12.descriptorBindingPartiallyBound },
        { "descriptorBindingVariableDescriptorCount", f12.descriptorBindingVariableDescriptorCount },
        { "shaderSampledImageArrayNonUniformIndexing", f12.shaderSampledImageArrayNonUniformIndexing },
        { "descriptorBindingSampledImageUpdateAfterBind", f12.descriptorBindingSampledImageUpdateAfterBind },
        { "bufferDeviceAddress", f12.bufferDeviceAddress },
        { "textureCompressionBC", f2.features.textureCompressionBC },
        { "samplerAnisotropy", f2.features.samplerAnisotropy },
        { "multiDrawIndirect", f2.features.multiDrawIndirect },
    };
    for (const auto& [name, ok] : need)
    {
        if (!ok)
        {
            s.missing.emplace_back(name);
        }
    }
    s.limits = readLimits(pd);
    if (!pretend.vendor.empty())
    {
        s.limits = clampLimits(s.limits, gpuLimitsFor(pretend.vendorId));
    }
    if (s.limits.maxPushConstantsSize < kClientPushConstantBytes)
    {
        s.missing.emplace_back("maxPushConstantsSize >= " + std::to_string(kClientPushConstantBytes));
    }
    s.rayQuery        = rtExts && fas.accelerationStructure && frq.rayQuery && f2.features.shaderInt64 &&
                        s.limits.maxPerStageSampledImages >= kClientSampledImagesPerStage;
    s.subgroupControl = f13.subgroupSizeControl && f13.computeFullSubgroups;
    s.float16         = f12.shaderFloat16 && f2.features.shaderInt16;
    // FSR 3.1's passes (tools/fsr_shaders.py's modules): StorageImageExtendedFormats, StorageImageWriteWithoutFormat,
    // and quad subgroup operations in compute shaders for its downsampling.
    VkPhysicalDeviceVulkan11Properties p11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
    VkPhysicalDeviceProperties2        pp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    pp.pNext = &p11;
    vkGetPhysicalDeviceProperties2(pd, &pp);
    const bool quad = (p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                      (p11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) &&
                      (p11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT);
    if (!f2.features.shaderStorageImageExtendedFormats)
    {
        s.fsrMissing = "shaderStorageImageExtendedFormats";
    }
    else if (!f2.features.shaderStorageImageWriteWithoutFormat)
    {
        s.fsrMissing = "shaderStorageImageWriteWithoutFormat";
    }
    else if (!quad)
    {
        s.fsrMissing = "quad subgroup operations in compute shaders";
    }
    else if (f12.shaderFloat16 && !f2.features.shaderInt16)
    {
        s.fsrMissing = "shaderInt16 (the SDK picks its 16-bit passes from shaderFloat16 alone)";
    }
    s.fsr = s.fsrMissing.empty();
    return s;
}

GpuCapabilities probeGpu()
{
    GpuCapabilities caps;
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "xi-client capabilities";
    app.apiVersion       = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkInstance instance  = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
    {
        return caps;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    // The same choice as the client (gfx::Context): Vulkan 1.3, a discrete GPU before an integrated one.
    VkPhysicalDevice best      = VK_NULL_HANDLE;
    int              bestScore = 0;
    for (VkPhysicalDevice pd : devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3)
        {
            continue;
        }
        const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > bestScore)
        {
            bestScore = score;
            best      = pd;
        }
    }
    if (best != VK_NULL_HANDLE)
    {
        caps.vulkan = true;
        VkPhysicalDeviceDriverProperties driver{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2      props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props2.pNext = &driver;
        vkGetPhysicalDeviceProperties2(best, &props2);
        caps.gpu      = props2.properties.deviceName;
        const GpuPretend pretend = gpuPretendFromEnv();
        caps.vendorId            = pretend.vendor.empty() ? props2.properties.vendorID : pretend.vendorId;
        caps.nvidia              = caps.vendorId == kVendorNvidia;
        caps.pretend             = pretend.vendor;
        caps.driver   = std::string(driver.driverName) + (driver.driverInfo[0] ? std::string(" ") + driver.driverInfo : "");
        const DeviceSupport ds = checkDevice(best, pretend);
        caps.rayQuery          = ds.rayQuery;
        caps.missing           = ds.missing;
        caps.fsr               = ds.fsr;
    }
    vkDestroyInstance(instance, nullptr);
    return caps;
}

} // namespace xi::gfx
