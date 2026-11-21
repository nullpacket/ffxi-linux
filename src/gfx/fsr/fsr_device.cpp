// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/fsr/fsr_device.h"

#include <string_view>
#include <utility>

namespace xi::gfx::fsrdev
{

namespace
{

// The SDK's table, v1.1.4 (ffx_vk.cpp's vkDeviceProcAddr calls). Optional ones are called only after a NULL check.
constexpr const char* kRequired[] = {
    "vkFlushMappedMemoryRanges",
    "vkCreateDescriptorPool",
    "vkCreateSampler",
    "vkCreateDescriptorSetLayout",
    "vkCreateBuffer",
    "vkCreateBufferView",
    "vkCreateImage",
    "vkCreateImageView",
    "vkCreateShaderModule",
    "vkCreatePipelineLayout",
    "vkCreateComputePipelines",
    "vkDestroyPipelineLayout",
    "vkDestroyPipeline",
    "vkDestroyImage",
    "vkDestroyImageView",
    "vkDestroyBuffer",
    "vkDestroyBufferView",
    "vkDestroyDescriptorSetLayout",
    "vkDestroyDescriptorPool",
    "vkDestroySampler",
    "vkDestroyShaderModule",
    "vkGetBufferMemoryRequirements",
    "vkGetBufferMemoryRequirements2KHR",
    "vkGetImageMemoryRequirements",
    "vkAllocateDescriptorSets",
    "vkFreeDescriptorSets",
    "vkAllocateMemory",
    "vkFreeMemory",
    "vkMapMemory",
    "vkUnmapMemory",
    "vkBindBufferMemory",
    "vkBindImageMemory",
    "vkUpdateDescriptorSets",
    "vkCmdPipelineBarrier",
    "vkCmdBindPipeline",
    "vkCmdBindDescriptorSets",
    "vkCmdDispatch",
    "vkCmdDispatchIndirect",
    "vkCmdCopyBuffer",
    "vkCmdCopyImage",
    "vkCmdCopyBufferToImage",
    "vkCmdClearColorImage",
    "vkCmdFillBuffer",
    "vkCmdWriteBufferMarkerAMD",
    "vkCmdWriteBufferMarker2AMD",
};

// The SDK's KHR names whose functions became core (Vulkan 1.1), with the core name.
constexpr std::pair<const char*, const char*> kPromoted[] = {
    { "vkGetBufferMemoryRequirements2KHR", "vkGetBufferMemoryRequirements2" },
};

// VKAPI_ATTR / VKAPI_CALL are empty on Linux (and confuse cppcheck); these have the PFN types' signatures.
void noMarker(VkCommandBuffer, VkPipelineStageFlagBits, VkBuffer, VkDeviceSize, uint32_t) {}
void noMarker2(VkCommandBuffer, VkPipelineStageFlags2, VkBuffer, VkDeviceSize, uint32_t) {}

} // namespace

PFN_vkVoidFunction resolve(Lookup lookup, VkDevice device, const char* name)
{
    if (PFN_vkVoidFunction f = lookup(device, name))
    {
        return f;
    }
    const std::string_view n(name);
    // Only names promoted to core, each to its own core function; a suffix isn't stripped by guesswork (the debug-utils
    // EXT names have no core version, and the SDK checks those for NULL itself).
    for (const auto& [alias, core] : kPromoted)
    {
        if (n == alias)
        {
            if (PFN_vkVoidFunction f = lookup(device, core))
            {
                return f;
            }
        }
    }
    if (n == "vkCmdWriteBufferMarkerAMD")
    {
        return reinterpret_cast<PFN_vkVoidFunction>(&noMarker);
    }
    if (n == "vkCmdWriteBufferMarker2AMD")
    {
        return reinterpret_cast<PFN_vkVoidFunction>(&noMarker2);
    }
    return nullptr;
}

std::vector<std::string> missing(Lookup lookup, VkDevice device)
{
    std::vector<std::string> out;
    for (const char* name : kRequired)
    {
        if (!resolve(lookup, device, name))
        {
            out.emplace_back(name);
        }
    }
    return out;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getDeviceProcAddr(VkDevice device, const char* name)
{
    return resolve(vkGetDeviceProcAddr, device, name);
}

} // namespace xi::gfx::fsrdev
