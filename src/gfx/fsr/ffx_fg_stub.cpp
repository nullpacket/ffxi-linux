// SPDX-License-Identifier: GPL-3.0-or-later
// The FidelityFX SDK's Vulkan back end (ffx_vk.cpp, ffxGetInterfaceVK) takes the address of the frame-generation
// swapchain's configure function, which lives in the SDK's FrameInterpolationSwapchain sources. The client uses the
// FSR 3.1 upscaler only, so those sources aren't built (issue #139; cmake/fsr.cmake) and this stands in for the one
// symbol: frame generation is never configured.
#include "gfx/fsr/ffx_linux_compat.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>

FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*)
{
    return FFX_ERROR_INVALID_ARGUMENT;
}
