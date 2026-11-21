// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

// The device functions the FidelityFX SDK's Vulkan back end looks up (issue #139). It resolves its whole table through
// the vkDeviceProcAddr we hand it and calls entries without checking them; a NULL one is a crash on the first use (a
// KHR alias of a core function, vkGetBufferMemoryRequirements2KHR, came back NULL when its extension wasn't enabled).
// resolve() fills the gaps: a KHR name the SDK uses falls back to the core function it was promoted to, and the
// AMD buffer markers (the SDK's crash breadcrumbs, used when the GPU lists VK_AMD_buffer_marker) to a no-op.
namespace xi::gfx::fsrdev
{

using Lookup = PFN_vkVoidFunction (*)(VkDevice, const char*);

// The name the SDK asks for, else its core name (promoted KHR names only), else a no-op for the AMD markers; NULL
// otherwise (the debug-utils EXT names: the SDK checks those before calling).
[[nodiscard]] PFN_vkVoidFunction resolve(Lookup lookup, VkDevice device, const char* name);
// Every function the SDK's table (v1.1.4, ffx_vk.cpp CreateBackendContextVK) needs and `resolve` leaves NULL.
// Debug-utils names are optional (the SDK checks them before calling).
[[nodiscard]] std::vector<std::string> missing(Lookup lookup, VkDevice device);
// A PFN_vkGetDeviceProcAddr for the SDK: resolve() over the loader's vkGetDeviceProcAddr.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getDeviceProcAddr(VkDevice device, const char* name);

} // namespace xi::gfx::fsrdev
