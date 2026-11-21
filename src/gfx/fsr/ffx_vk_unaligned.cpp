// SPDX-License-Identifier: GPL-3.0-or-later
// The FidelityFX SDK's Vulkan back end (ffx_vk.cpp, unchanged), compiled without its one over-aligned type (#139).
//
// ffx_vk.cpp declares `struct alignas(32) EffectContext` but places the array of them inside the caller's scratch
// buffer at whatever offset the arrays before it add up to (8 mod 32 here), so GCC's aligned stores to it (movaps)
// fault: every first FSR dispatch crashed in CreateBackendContextVK. The type is private to that file (its layout
// isn't shared with any other translation unit), and the alignment is only a cache hint, so this file includes
// everything ffx_vk.cpp includes first and then compiles it with `alignas` defined away; no other type in the SDK
// uses alignas.
#include <FidelityFX/host/ffx_assert.h>
#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/ffx_util.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <ffx_breadcrumbs_list.h>
#include <ffx_shader_blobs.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <codecvt>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#define alignas(n)
#include XI_FFX_VK_CPP // NOLINT(bugprone-suspicious-include): the SDK file itself, on purpose
#undef alignas
