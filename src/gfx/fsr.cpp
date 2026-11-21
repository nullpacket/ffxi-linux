// SPDX-License-Identifier: GPL-3.0-or-later
#if XI_HAVE_FSR
#include "gfx/fsr/ffx_linux_compat.h" // first: the SDK's context size on Linux (see there)
#endif
#include "gfx/fsr.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#if XI_HAVE_FSR
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include "gfx/fsr/fsr_device.h"

#include <cfloat> // FLT_MAX
#include <string>
#include <vector>
#endif

namespace xi::gfx
{

std::optional<FsrMode> parseFsrMode(std::string_view s)
{
    if (s.empty() || s == "quality")
    {
        return FsrMode::Quality;
    }
    if (s == "balanced")
    {
        return FsrMode::Balanced;
    }
    if (s == "performance")
    {
        return FsrMode::Performance;
    }
    if (s == "ultra-performance" || s == "ultraperformance")
    {
        return FsrMode::UltraPerformance;
    }
    if (s == "native" || s == "nativeaa" || s == "dlaa")
    {
        return FsrMode::NativeAa;
    }
    return std::nullopt;
}

const char* fsrModeName(FsrMode m)
{
    switch (m)
    {
        case FsrMode::NativeAa: return "native";
        case FsrMode::Quality: return "quality";
        case FsrMode::Balanced: return "balanced";
        case FsrMode::Performance: return "performance";
        case FsrMode::UltraPerformance: return "ultra-performance";
    }
    return "?";
}

float fsrUpscale(FsrMode m)
{
    switch (m)
    {
        case FsrMode::NativeAa: return 1.0f;
        case FsrMode::Quality: return 1.5f;
        case FsrMode::Balanced: return 1.7f;
        case FsrMode::Performance: return 2.0f;
        case FsrMode::UltraPerformance: return 3.0f;
    }
    return 1.0f;
}

float fsrLodBias(FsrMode m)
{
    if (const char* e = std::getenv("XI_FSR_LOD_BIAS"); e && *e)
    {
        return std::strtof(e, nullptr);
    }
    return std::log2(1.0f / fsrUpscale(m)) - 1.0f;
}

VkExtent2D fsrRenderExtent(VkExtent2D output, FsrMode m)
{
    // ffxFsr3UpscalerGetRenderResolutionFromQualityMode: the output divided by the ratio, truncated.
    const float r = fsrUpscale(m);
    return { std::max(1u, static_cast<uint32_t>(static_cast<float>(output.width) / r)),
             std::max(1u, static_cast<uint32_t>(static_cast<float>(output.height) / r)) };
}

#if XI_HAVE_FSR

bool fsrBuilt()
{
    return true;
}

namespace
{

void onMessage(FfxMsgType type, const wchar_t* message)
{
    std::string text;
    for (const wchar_t* p = message; p && *p; ++p)
    {
        text += *p < 0x80 ? static_cast<char>(*p) : '?';
    }
    if (type == FFX_MESSAGE_TYPE_ERROR)
    {
        spdlog::error("fsr: {}", text);
    }
    else
    {
        spdlog::warn("fsr: {}", text);
    }
}

// An image of ours as the SDK sees it.
FfxResource resource(const Image& img, VkImageUsageFlags usage, const wchar_t* name, FfxResourceStates state)
{
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization): every field the SDK reads is set below
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format    = img.format;
    ci.extent    = { img.extent.width, img.extent.height, 1 };
    // Mip 0 only: FSR reads and writes no other mip, and the scene colour has a mip chain (SceneTarget) whose other
    // mips aren't in the layout it is handed in, so a view over them is invalid (validation, #139).
    ci.mipLevels   = 1;
    ci.arrayLayers = 1;
    ci.samples     = VK_SAMPLE_COUNT_1_BIT;
    ci.usage       = usage;
    const auto uav = (usage & VK_IMAGE_USAGE_STORAGE_BIT) ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
    return ffxGetResourceVK(reinterpret_cast<void*>(img.image), ffxGetImageResourceDescriptionVK(img.image, ci, uav),
                            name, state);
}

} // namespace

struct Fsr::Impl
{
    std::vector<unsigned char> scratch;
    FfxInterface               backend{};
    FfxFsr3UpscalerContext     context{};
    bool                       created = false;
    VkExtent2D                 output{}, maxRender{};
    // FSR 3.1's shared resources (ffxFsr3UpscalerGetSharedResourceDescriptions): the application allocates them, at
    // the maximum render size; without them its first clear hits a null resource. Made GENERAL once (`fresh`).
    Image dilatedDepth, dilatedMotion, prevNearestDepth;
    bool  fresh = false;

    void destroyShared(Context& ctx)
    {
        for (Image* i : { &dilatedDepth, &dilatedMotion, &prevNearestDepth })
        {
            if (i->image)
            {
                ctx.destroyImage(*i);
            }
            *i = Image{};
        }
    }
};

Fsr::Fsr(Context& ctx, FsrMode mode, float sharpness)
: ctx_(ctx)
, mode_(mode)
, sharpness_(std::clamp(sharpness, 0.0f, 1.0f))
, impl_(std::make_unique<Impl>())
{
    if (!ctx_.fsr())
    {
        spdlog::warn("fsr: this GPU / driver lacks {}; rendering without FSR", ctx_.fsrMissing());
        return;
    }
    // The SDK calls every entry of its function table unchecked; fsrdev fills the KHR aliases and AMD markers a
    // driver leaves NULL, and anything still missing turns FSR off here instead of crashing in its first dispatch.
    if (const auto gaps = fsrdev::missing(vkGetDeviceProcAddr, ctx_.device()); !gaps.empty())
    {
        std::string names;
        for (const auto& g : gaps)
        {
            names += (names.empty() ? "" : ", ") + g;
        }
        spdlog::warn("fsr: the Vulkan driver has no {}; rendering without FSR", names);
        return;
    }
    VkDeviceContext dc{ ctx_.device(), ctx_.physicalDevice(), &fsrdev::getDeviceProcAddr };
    const FfxDevice device = ffxGetDeviceVK(&dc);
    const size_t    size   = ffxGetScratchMemorySizeVK(ctx_.physicalDevice(), FFX_FSR3UPSCALER_CONTEXT_COUNT);
    impl_->scratch.assign(size, 0);
    const FfxErrorCode e =
        ffxGetInterfaceVK(&impl_->backend, device, impl_->scratch.data(), size, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    if (e != FFX_OK)
    {
        spdlog::warn("fsr: the FidelityFX Vulkan back end did not start (error {:#x}); rendering without FSR",
                     static_cast<uint32_t>(e));
        return;
    }
    ok_ = true;
    spdlog::info("fsr: FSR 3.1 upscaler, {} ({:.1f}x per axis), sharpness {:.2f}", fsrModeName(mode_),
                 fsrUpscale(mode_), sharpness_);
}

Fsr::~Fsr()
{
    if (impl_ && impl_->created)
    {
        ctx_.waitIdle();
        ffxFsr3UpscalerContextDestroy(&impl_->context);
        impl_->destroyShared(ctx_);
    }
}

bool Fsr::evaluate(VkCommandBuffer cmd, const UpscaleInputs& in)
{
    if (!ok_ || !in.color || !in.depth || !in.motion || !in.output)
    {
        return false;
    }
    const VkExtent2D out = in.output->extent;
    auto&            im  = *impl_;
    // The context is made for one output size and a render size up to the mode's for it: a new window size makes it
    // again (the history goes with it).
    if (!im.created || im.output.width != out.width || im.output.height != out.height ||
        in.render.width > im.maxRender.width || in.render.height > im.maxRender.height)
    {
        if (im.created)
        {
            ctx_.waitIdle();
            ffxFsr3UpscalerContextDestroy(&im.context);
            im.destroyShared(ctx_);
            im.created = false;
        }
        const VkExtent2D                  render = renderExtent(out);
        FfxFsr3UpscalerContextDescription d{};
        // The client's depth is reverse-Z with an infinite far plane; its colour is the 8-bit scene (no HDR), its motion
        // vectors are at render size without the jitter. tools/fsr_shaders.py compiles exactly these permutations.
        d.flags            = FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE |
                             FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
        d.maxRenderSize    = { std::max(render.width, in.render.width), std::max(render.height, in.render.height) };
        d.maxUpscaleSize   = { out.width, out.height };
        d.fpMessage        = onMessage;
        d.backendInterface = im.backend;
        if (std::getenv("XI_FSR_DEBUG"))
        {
            d.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING; // the SDK checks the dispatch values and says what's off
        }
        const FfxErrorCode e = ffxFsr3UpscalerContextCreate(&im.context, &d);
        if (e != FFX_OK)
        {
            spdlog::warn("fsr: could not create the upscaler for {}x{} (error {:#x}); rendering without FSR", out.width,
                         out.height, static_cast<uint32_t>(e));
            ok_ = false;
            return false;
        }
        im.created   = true;
        im.output    = out;
        im.maxRender = { d.maxRenderSize.width, d.maxRenderSize.height };
        // The shared resources, in the formats the SDK describes (R32_FLOAT, R16G16_FLOAT, R32_UINT), read and written
        // by its passes as storage images.
        constexpr VkImageUsageFlags u =
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        im.dilatedDepth     = ctx_.createImage(im.maxRender, VK_FORMAT_R32_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
        im.dilatedMotion    = ctx_.createImage(im.maxRender, VK_FORMAT_R16G16_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
        im.prevNearestDepth = ctx_.createImage(im.maxRender, VK_FORMAT_R32_UINT, u, VK_IMAGE_ASPECT_COLOR_BIT);
        im.fresh            = true;
        spdlog::info("fsr: upscaler {}x{} -> {}x{}", d.maxRenderSize.width, d.maxRenderSize.height, out.width,
                     out.height);
    }

    constexpr VkImageUsageFlags shared =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (im.fresh)
    {
        for (const Image* i : { &im.dilatedDepth, &im.dilatedMotion, &im.prevNearestDepth })
        {
            Context::transition(cmd, i->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        }
        im.fresh = false;
    }
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization): every field is set below
    FfxFsr3UpscalerDispatchDescription p{};
    p.commandList  = ffxGetCommandListVK(cmd);
    p.dilatedDepth = resource(im.dilatedDepth, shared, L"FSR3_DilatedDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    p.dilatedMotionVectors =
        resource(im.dilatedMotion, shared, L"FSR3_DilatedMotionVectors", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    p.reconstructedPrevNearestDepth = resource(im.prevNearestDepth, shared, L"FSR3_ReconstructedPrevNearestDepth",
                                               FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    p.color = resource(*in.color, VK_IMAGE_USAGE_SAMPLED_BIT, L"FSR3_InputColor", FFX_RESOURCE_STATE_COMPUTE_READ);
    p.depth = resource(*in.depth, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       L"FSR3_InputDepth", FFX_RESOURCE_STATE_COMPUTE_READ);
    p.motionVectors =
        resource(*in.motion, VK_IMAGE_USAGE_SAMPLED_BIT, L"FSR3_InputMotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);
    p.output =
        resource(*in.output, VK_IMAGE_USAGE_STORAGE_BIT, L"FSR3_UpscaledOutput", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    // Jitter in render pixels as added to the projection, the motion vectors in render pixels (previous - current):
    // both the conventions DLSS takes, and FSR's with a scale of 1 (it divides by the render size itself).
    p.jitterOffset      = { in.jitter.x, in.jitter.y };
    p.motionVectorScale = { 1.0f, 1.0f };
    p.renderSize        = { in.render.width, in.render.height };
    p.upscaleSize       = { out.width, out.height };
    p.enableSharpening  = sharpness_ > 0.0f;
    p.sharpness         = sharpness_;
    p.frameTimeDelta    = std::clamp(in.frameMs, 1.0f, 100.0f);
    p.preExposure       = 1.0f;
    p.reset             = in.reset;
    // Reverse-Z with an infinite far plane: FSR wants the near plane in cameraFar and FLT_MAX in cameraNear then.
    p.cameraNear              = FLT_MAX;
    p.cameraFar               = in.nearZ;
    p.cameraFovAngleVertical  = in.fovY > 0.0f ? in.fovY : 1.0f;
    p.viewSpaceToMetersFactor = 1.0f; // a game unit (a yalm) is about a metre
    const FfxErrorCode e      = ffxFsr3UpscalerContextDispatch(&im.context, &p);
    if (e != FFX_OK)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            spdlog::warn("fsr: dispatch failed (error {:#x})", static_cast<uint32_t>(e));
        }
        return false;
    }
    return true;
}

#else // built without the SDK: --fsr logs and the client renders as always.

bool fsrBuilt()
{
    return false;
}

struct Fsr::Impl
{
};

Fsr::Fsr(Context& ctx, FsrMode mode, float sharpness)
: ctx_(ctx)
, mode_(mode)
, sharpness_(sharpness)
{
    spdlog::warn("fsr: this build has no FSR (XI_FSR, and the FidelityFX SDK at configure time); rendering without it");
}

Fsr::~Fsr() = default;

bool Fsr::evaluate(VkCommandBuffer, const UpscaleInputs&)
{
    return false;
}

#endif

} // namespace xi::gfx
