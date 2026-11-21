// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #139: the upscaler choice (DLSS / FSR, with the fallback on GPUs without DLSS), FSR's modes and render sizes,
// and the XI_GPU_PRETEND limits. No GPU needed.
#include "gfx/capabilities.h"
#include "gfx/fsr.h"
#include "gfx/fsr/fsr_device.h"
#include "gfx/upscaler.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

using Catch::Approx;
using xi::gfx::FsrMode;
using xi::gfx::UpscalerKind;

TEST_CASE("fsr: mode names, ratios and render sizes", "[fsr]")
{
    CHECK(xi::gfx::parseFsrMode("") == FsrMode::Quality);
    CHECK(xi::gfx::parseFsrMode("balanced") == FsrMode::Balanced);
    CHECK(xi::gfx::parseFsrMode("performance") == FsrMode::Performance);
    CHECK(xi::gfx::parseFsrMode("ultra-performance") == FsrMode::UltraPerformance);
    CHECK(xi::gfx::parseFsrMode("native") == FsrMode::NativeAa);
    CHECK(xi::gfx::parseFsrMode("dlaa") == FsrMode::NativeAa); // a --dlss=dlaa profile on an AMD GPU
    CHECK_FALSE(xi::gfx::parseFsrMode("ultra"));
    for (auto m :
         { FsrMode::NativeAa, FsrMode::Quality, FsrMode::Balanced, FsrMode::Performance, FsrMode::UltraPerformance })
    {
        CHECK(xi::gfx::parseFsrMode(xi::gfx::fsrModeName(m)) == m);
    }
    // AMD's fixed ratios, and ffxFsr3UpscalerGetRenderResolutionFromQualityMode's truncation.
    CHECK(xi::gfx::fsrUpscale(FsrMode::Quality) == Approx(1.5f));
    CHECK(xi::gfx::fsrUpscale(FsrMode::Balanced) == Approx(1.7f));
    const auto q = xi::gfx::fsrRenderExtent({ 2560, 1440 }, FsrMode::Quality);
    CHECK(q.width == 1706);
    CHECK(q.height == 960);
    const auto b = xi::gfx::fsrRenderExtent({ 1920, 1080 }, FsrMode::Balanced);
    CHECK(b.width == 1129);
    CHECK(b.height == 635);
    const auto n = xi::gfx::fsrRenderExtent({ 1280, 720 }, FsrMode::NativeAa);
    CHECK(n.width == 1280);
    CHECK(n.height == 720);
    // log2(render / output) - 1: Performance = -2, native = -1.
    CHECK(xi::gfx::fsrLodBias(FsrMode::Performance) == Approx(-2.0f));
    CHECK(xi::gfx::fsrLodBias(FsrMode::NativeAa) == Approx(-1.0f));
}

TEST_CASE("upscaler: which one starts", "[fsr]")
{
    using xi::gfx::chooseUpscaler;
    // NVIDIA with a DLSS build: --dlss is DLSS.
    auto c = chooseUpscaler("", "balanced", "", true, true);
    CHECK(c.kind == UpscalerKind::Dlss);
    CHECK(c.mode == "balanced");
    CHECK(c.note.empty());
    // AMD / Intel (or no DLSS build): --dlss falls back to FSR with the same mode, and says so.
    c = chooseUpscaler("", "performance", "", false, true);
    CHECK(c.kind == UpscalerKind::Fsr);
    CHECK(c.mode == "performance");
    CHECK_FALSE(c.note.empty());
    // --fsr is FSR everywhere, NVIDIA too.
    c = chooseUpscaler("", "", "quality", true, true);
    CHECK(c.kind == UpscalerKind::Fsr);
    // --upscaler=auto: DLSS where it can run, else FSR, quietly.
    c = chooseUpscaler("auto", "", "", true, true);
    CHECK(c.kind == UpscalerKind::Dlss);
    CHECK(c.mode == "quality");
    c = chooseUpscaler("auto", "", "", false, true);
    CHECK(c.kind == UpscalerKind::Fsr);
    CHECK(c.note.empty());
    // --upscaler=fsr wins over --dlss=; off is off.
    c = chooseUpscaler("fsr", "dlaa", "", true, true);
    CHECK(c.kind == UpscalerKind::Fsr);
    CHECK(c.mode == "dlaa"); // parseFsrMode reads it as native
    CHECK(chooseUpscaler("off", "quality", "quality", true, true).kind == UpscalerKind::None);
    CHECK(chooseUpscaler("", "", "", true, true).kind == UpscalerKind::None);
    // Neither can run: nothing, with the reason.
    c = chooseUpscaler("", "quality", "", false, false);
    CHECK(c.kind == UpscalerKind::None);
    CHECK_FALSE(c.note.empty());
}

TEST_CASE("gpu pretend: vendors and clamped limits", "[fsr]")
{
    using namespace xi::gfx;
    CHECK(gpuPretendFrom("amd").vendorId == kVendorAmd);
    CHECK(gpuPretendFrom("intel").vendorId == kVendorIntel);
    CHECK(gpuPretendFrom("nvidia").vendor.empty()); // only the vendors there is no hardware for
    CHECK(gpuPretendFrom(nullptr).vendor.empty());
    // A 4090's limits clamped to an RX 6800's: 128 push-constant bytes, wave32 to wave64.
    GpuLimits rtx;
    rtx.maxPushConstantsSize           = 256;
    rtx.maxPerStageSampledImages       = 1048576;
    rtx.maxComputeWorkGroupInvocations = 1024;
    rtx.maxImageDimension2D            = 32768;
    rtx.minSubgroupSize = rtx.maxSubgroupSize = 32;
    const auto amd                            = clampLimits(rtx, gpuLimitsFor(kVendorAmd));
    CHECK(amd.maxPushConstantsSize == 128);
    CHECK(amd.maxImageDimension2D == 16384);
    CHECK(amd.minSubgroupSize == 32);
    CHECK(amd.maxSubgroupSize == 64);
    CHECK(amd.maxPerStageSampledImages == 1048576); // the smaller of the two
    // The client's biggest push block fits every vendor's guaranteed size.
    CHECK(kClientPushConstantBytes <= amd.maxPushConstantsSize);
    const auto intel = clampLimits(rtx, gpuLimitsFor(kVendorIntel));
    CHECK(intel.minSubgroupSize == 8);
    CHECK(kClientSampledImagesPerStage <= intel.maxPerStageSampledImages);
    // No pretend: the device's own.
    CHECK(clampLimits(rtx, GpuLimits{}).maxPushConstantsSize == 256);
}

namespace
{

void fakeFunction() {}

// A driver that resolves core names only: every KHR / EXT / AMD name comes back NULL (an extension not enabled).
PFN_vkVoidFunction coreOnly(VkDevice, const char* name)
{
    const std::string n(name);
    for (const char* suffix : { "KHR", "EXT", "AMD" })
    {
        if (n.size() > 3 && n.compare(n.size() - 3, 3, suffix) == 0)
        {
            return nullptr;
        }
    }
    return &fakeFunction;
}

// A driver missing one core function too.
PFN_vkVoidFunction noDispatch(VkDevice d, const char* name)
{
    return std::strcmp(name, "vkCmdDispatch") == 0 ? nullptr : coreOnly(d, name);
}

PFN_vkVoidFunction nothing(VkDevice, const char*)
{
    return nullptr;
}

} // namespace

TEST_CASE("fsr: the SDK's device functions, with core and no-op fallbacks", "[fsr]")
{
    namespace fd = xi::gfx::fsrdev;
    // The crash of the first live run: vkGetBufferMemoryRequirements2KHR NULL without its extension; the core
    // function (Vulkan 1.1) stands in.
    CHECK(fd::resolve(coreOnly, VK_NULL_HANDLE, "vkGetBufferMemoryRequirements2KHR") == &fakeFunction);
    CHECK(fd::resolve(coreOnly, VK_NULL_HANDLE, "vkCmdPipelineBarrier") == &fakeFunction);
    // The AMD breadcrumb markers become no-ops (callable, never NULL).
    const auto marker = fd::resolve(coreOnly, VK_NULL_HANDLE, "vkCmdWriteBufferMarkerAMD");
    REQUIRE(marker != nullptr);
    CHECK(marker != &fakeFunction);
    CHECK(fd::resolve(coreOnly, VK_NULL_HANDLE, "vkCmdWriteBufferMarker2AMD") != nullptr);
    // A name with no core version stays NULL.
    CHECK(fd::resolve(coreOnly, VK_NULL_HANDLE, "vkSetDebugUtilsObjectNameEXT") == nullptr);
    // With the fallbacks the whole table resolves; a missing core function is named, so FSR turns off cleanly.
    CHECK(fd::missing(coreOnly, VK_NULL_HANDLE).empty());
    const auto gaps = fd::missing(noDispatch, VK_NULL_HANDLE);
    REQUIRE(gaps.size() == 1);
    CHECK(gaps[0] == "vkCmdDispatch");
    CHECK(fd::missing(nothing, VK_NULL_HANDLE).size() > 40);
}
