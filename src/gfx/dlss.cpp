// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/dlss.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if XI_HAVE_DLSS
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>
#include <nvsdk_ngx_vk.h>

#include <SDL3/SDL.h>
#endif

namespace xi::gfx
{

std::optional<DlssMode> parseDlssMode(std::string_view s)
{
    if (s.empty() || s == "quality")
    {
        return DlssMode::Quality;
    }
    if (s == "balanced")
    {
        return DlssMode::Balanced;
    }
    if (s == "performance")
    {
        return DlssMode::Performance;
    }
    if (s == "ultra-performance" || s == "ultraperformance")
    {
        return DlssMode::UltraPerformance;
    }
    if (s == "dlaa")
    {
        return DlssMode::Dlaa;
    }
    return std::nullopt;
}

const char* dlssModeName(DlssMode m)
{
    switch (m)
    {
        case DlssMode::Dlaa: return "dlaa";
        case DlssMode::Quality: return "quality";
        case DlssMode::Balanced: return "balanced";
        case DlssMode::Performance: return "performance";
        case DlssMode::UltraPerformance: return "ultra-performance";
    }
    return "?";
}

float dlssRatio(DlssMode m)
{
    switch (m)
    {
        case DlssMode::Dlaa: return 1.0f;
        case DlssMode::Quality: return 2.0f / 3.0f;
        case DlssMode::Balanced: return 0.58f;
        case DlssMode::Performance: return 0.5f;
        case DlssMode::UltraPerformance: return 1.0f / 3.0f;
    }
    return 1.0f;
}

float dlssLodBias(DlssMode m)
{
    if (const char* e = std::getenv("XI_DLSS_LOD_BIAS"); e && *e)
    {
        return std::strtof(e, nullptr);
    }
    return std::log2(dlssRatio(m)) - 1.0f;
}

namespace
{

float halton(uint64_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    while (index > 0)
    {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

} // namespace

glm::vec2 dlssJitter(uint64_t frame, float upscale)
{
    // The DLSS guide's phase count: 8 x the area ratio (8 at DLAA, 18 at Quality's 1.5x, 72 at Ultra Performance).
    const auto phases = static_cast<uint64_t>(std::max(8.0f, std::ceil(8.0f * upscale * upscale)));
    const auto i      = frame % phases + 1;
    return { halton(i, 2) - 0.5f, halton(i, 3) - 0.5f };
}

glm::mat4 jitterViewProj(const glm::mat4& viewProj, glm::vec2 ndcOffset)
{
    // clip.xy += clip.w * offset: row 0 / 1 gain row 3 times the offset (column-major: m[col][row]).
    glm::mat4 m = viewProj;
    for (int c = 0; c < 4; ++c)
    {
        m[c][0] += m[c][3] * ndcOffset.x;
        m[c][1] += m[c][3] * ndcOffset.y;
    }
    return m;
}

#if XI_HAVE_DLSS

bool dlssBuilt()
{
    return true;
}

namespace
{

// A fixed project id for NGX's custom-engine path (NVSDK_NGX_ENGINE_TYPE_CUSTOM: no NVIDIA-issued application id).
constexpr const char* kProjectId     = "6f3c1b8e-ff11-4a5e-9c1d-0f0f11e1a001";
constexpr const char* kEngineVersion = "1.0";

std::wstring widen(const std::string& s)
{
    return { s.begin(), s.end() };
}

std::filesystem::path dataPath()
{
    const char* xdg  = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path p = xdg && *xdg ? std::filesystem::path(xdg) : std::filesystem::path(home ? home : "/tmp") / ".cache";
    p /= "ffxi-linux/ngx";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

// Where libnvidia-ngx-dlss.so.<ver> may be: XI_DLSS_LIB_DIR, the executable's folder (NGX's own default), the SDK
// the build was configured with.
std::vector<std::wstring> searchPaths()
{
    std::vector<std::wstring> out;
    if (const char* e = std::getenv("XI_DLSS_LIB_DIR"); e && *e)
    {
        out.push_back(widen(e));
    }
    if (const char* base = SDL_GetBasePath())
    {
        out.push_back(widen(base));
    }
#ifdef XI_DLSS_DEFAULT_LIB_DIR
    out.push_back(widen(XI_DLSS_DEFAULT_LIB_DIR));
#endif
    return out;
}

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::string m(message);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r'))
    {
        m.pop_back();
    }
    spdlog::debug("ngx: {}", m);
}

struct Discovery
{
    std::wstring                     data = dataPath().wstring();
    std::vector<std::wstring>        paths = searchPaths();
    std::vector<const wchar_t*>      pathPtrs;
    NVSDK_NGX_FeatureCommonInfo      common{};
    NVSDK_NGX_FeatureDiscoveryInfo   info{};

    explicit Discovery(NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_SuperSampling)
    {
        for (const auto& p : paths)
        {
            pathPtrs.push_back(p.c_str());
        }
        common.PathListInfo.Path                    = pathPtrs.data();
        common.PathListInfo.Length                  = static_cast<unsigned int>(pathPtrs.size());
        common.LoggingInfo.LoggingCallback          = ngxLog;
        common.LoggingInfo.MinimumLoggingLevel      = std::getenv("XI_DLSS_LOG") ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
        common.LoggingInfo.DisableOtherLoggingSinks = false;
        info.SDKVersion                             = NVSDK_NGX_Version_API;
        info.FeatureID                              = feature;
        info.Identifier.IdentifierType              = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        info.Identifier.v.ProjectDesc.ProjectId     = kProjectId;
        info.Identifier.v.ProjectDesc.EngineType    = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
        info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
        info.ApplicationDataPath                    = data.c_str();
        info.FeatureInfo                            = &common;
    }
};

NVSDK_NGX_PerfQuality_Value perfQuality(DlssMode m)
{
    switch (m)
    {
        case DlssMode::Dlaa: return NVSDK_NGX_PerfQuality_Value_DLAA;
        case DlssMode::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case DlssMode::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case DlssMode::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case DlssMode::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

NVSDK_NGX_Resource_VK resource(const Image& img, VkImageView view, VkImageAspectFlags aspect, bool readWrite)
{
    return NVSDK_NGX_Create_ImageView_Resource_VK(view, img.image, { aspect, 0, 1, 0, 1 }, img.format, img.extent.width, img.extent.height, readWrite);
}

} // namespace

struct Dlss::Impl
{
    Discovery              discovery;
    NVSDK_NGX_Parameter*   params  = nullptr;
    NVSDK_NGX_Handle*      feature = nullptr;
    VkExtent2D             featureRender{}, featureOutput{};
    bool                   featureRR   = false; // the feature made is Ray Reconstruction
    bool                   initialised = false;
};

ContextExtensions Dlss::extensions()
{
    ContextExtensions out;
    // Super Resolution's and Ray Reconstruction's (--rt, goal 38) requirements, merged.
    for (const auto feature : { NVSDK_NGX_Feature_SuperSampling, NVSDK_NGX_Feature_RayReconstruction })
    {
        Discovery              d(feature);
        uint32_t               n     = 0;
        VkExtensionProperties* props = nullptr;
        if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&d.info, &n, &props)))
        {
            spdlog::warn("dlss: NGX gave no instance extension list");
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            if (std::ranges::find(out.instance, std::string(props[i].extensionName)) == out.instance.end())
            {
                out.instance.emplace_back(props[i].extensionName);
            }
        }
    }
    out.device = [](VkInstance inst, VkPhysicalDevice pd) {
        std::vector<std::string> exts;
        for (const auto feature : { NVSDK_NGX_Feature_SuperSampling, NVSDK_NGX_Feature_RayReconstruction })
        {
        Discovery                dd(feature);
        uint32_t                 count = 0;
        VkExtensionProperties*   p     = nullptr;
        if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(inst, pd, &dd.info, &count, &p)))
        {
            spdlog::warn("dlss: NGX gave no device extension list");
            continue;
        }
        // Only those the GPU offers: a missing one makes vkCreateDevice fail for everything, DLSS or not.
        uint32_t have = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &have, nullptr);
        std::vector<VkExtensionProperties> avail(have);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &have, avail.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::string name = p[i].extensionName;
            if (std::ranges::any_of(avail, [&](const VkExtensionProperties& a) { return name == a.extensionName; }))
            {
                if (std::ranges::find(exts, name) == exts.end())
                {
                    exts.push_back(name);
                }
            }
            else
            {
                spdlog::warn("dlss: device extension {} not offered by the GPU", name);
            }
        }
        }
        return exts;
    };
    return out;
}

Dlss::Dlss(Context& ctx, DlssMode mode)
: ctx_(ctx)
, mode_(mode)
, impl_(std::make_unique<Impl>())
{
    auto&      d   = impl_->discovery;
    const auto res = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, d.data.c_str(), ctx_.instance(),
                                                          ctx_.physicalDevice(), ctx_.device(), vkGetInstanceProcAddr, vkGetDeviceProcAddr, &d.common);
    if (NVSDK_NGX_FAILED(res))
    {
        spdlog::warn("dlss: NGX init failed ({:#x}); rendering without DLSS", static_cast<uint32_t>(res));
        return;
    }
    impl_->initialised = true;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&impl_->params)))
    {
        spdlog::warn("dlss: no NGX capability parameters; rendering without DLSS");
        return;
    }
    int available = 0;
    NVSDK_NGX_Parameter_GetI(impl_->params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available)
    {
        int initResult = 0;
        NVSDK_NGX_Parameter_GetI(impl_->params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        spdlog::warn("dlss: Super Resolution not available (init result {:#x}; is libnvidia-ngx-dlss.so on XI_DLSS_LIB_DIR?); rendering without DLSS",
                     static_cast<uint32_t>(initResult));
        return;
    }
    ok_ = true;
    int rr = 0;
    NVSDK_NGX_Parameter_GetI(impl_->params, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rr);
    rrAvailable_ = rr != 0;
    spdlog::info("dlss: NGX ready on {} ({} mode; Ray Reconstruction {})", ctx_.deviceName(), dlssModeName(mode_), rrAvailable_ ? "available" : "not available");
}

Dlss::~Dlss()
{
    if (!impl_)
    {
        return;
    }
    ctx_.waitIdle();
    if (impl_->feature)
    {
        NVSDK_NGX_VULKAN_ReleaseFeature(impl_->feature);
    }
    if (impl_->params)
    {
        NVSDK_NGX_VULKAN_DestroyParameters(impl_->params);
    }
    if (impl_->initialised)
    {
        NVSDK_NGX_VULKAN_Shutdown1(ctx_.device());
    }
}

VkExtent2D Dlss::renderExtent(VkExtent2D output)
{
    if (!ok_ || mode_ == DlssMode::Dlaa)
    {
        return output;
    }
    unsigned int w = 0, h = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float        sharp = 0.0f;
    if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(impl_->params, output.width, output.height, perfQuality(mode_), &w, &h, &maxW, &maxH, &minW, &minH, &sharp)) ||
        w == 0 || h == 0)
    {
        return output;
    }
    return { w, h };
}

bool Dlss::evaluate(VkCommandBuffer cmd, const Inputs& in)
{
    if (!ok_)
    {
        return false;
    }
    auto&            im  = *impl_;
    const VkExtent2D out = in.output->extent;
    if (rrAvailable_ && in.diffuseAlbedo && in.specularAlbedo && in.normals)
    {
        return evaluateRR(cmd, in);
    }
    if (!im.feature || im.featureRR || im.featureRender.width != in.render.width || im.featureRender.height != in.render.height ||
        im.featureOutput.width != out.width || im.featureOutput.height != out.height)
    {
        if (im.feature)
        {
            ctx_.waitIdle();
            NVSDK_NGX_VULKAN_ReleaseFeature(im.feature);
            im.feature = nullptr;
        }
        // DLSS 4's transformer model (preset K) for every mode.
        for (const char* p : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance })
        {
            NVSDK_NGX_Parameter_SetUI(im.params, p, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
        }
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth            = in.render.width;
        cp.Feature.InHeight           = in.render.height;
        cp.Feature.InTargetWidth      = out.width;
        cp.Feature.InTargetHeight     = out.height;
        cp.Feature.InPerfQualityValue = perfQuality(mode_);
        // LDR (the game's colours are display-ready), reverse-Z depth, render-size unjittered motion, auto exposure.
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        const auto res = NGX_VULKAN_CREATE_DLSS_EXT1(ctx_.device(), cmd, 1, 1, &im.feature, im.params, &cp);
        if (NVSDK_NGX_FAILED(res))
        {
            spdlog::warn("dlss: feature creation failed ({:#x}) for {}x{} -> {}x{}; rendering without DLSS", static_cast<uint32_t>(res), in.render.width,
                         in.render.height, out.width, out.height);
            ok_ = false;
            return false;
        }
        im.featureRender = in.render;
        im.featureOutput = out;
        im.featureRR     = false;
        spdlog::info("dlss: {}x{} -> {}x{} ({})", in.render.width, in.render.height, out.width, out.height, dlssModeName(mode_));
    }
    NVSDK_NGX_Resource_VK color  = resource(*in.color, in.colorView, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK depth  = resource(*in.depth, in.depth->view, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    NVSDK_NGX_Resource_VK motion = resource(*in.motion, in.motion->view, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK output = resource(*in.output, in.output->view, VK_IMAGE_ASPECT_COLOR_BIT, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor                   = &color;
    ep.Feature.pInOutput                  = &output;
    ep.pInDepth                           = &depth;
    ep.pInMotionVectors                   = &motion;
    ep.InJitterOffsetX                    = in.jitter.x;
    ep.InJitterOffsetY                    = in.jitter.y;
    ep.InRenderSubrectDimensions          = { in.render.width, in.render.height };
    ep.InReset                            = in.reset ? 1 : 0;
    ep.InMVScaleX                         = 1.0f;
    ep.InMVScaleY                         = 1.0f;
    ep.InPreExposure                      = 1.0f;
    ep.InExposureScale                    = 1.0f;
    const auto res = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, im.feature, im.params, &ep);
    if (NVSDK_NGX_FAILED(res))
    {
        spdlog::warn("dlss: evaluate failed ({:#x})", static_cast<uint32_t>(res));
        return false;
    }
    return true;
}

bool Dlss::evaluateRR(VkCommandBuffer cmd, const Inputs& in)
{
    auto&            im  = *impl_;
    const VkExtent2D out = in.output->extent;
    if (!im.feature || !im.featureRR || im.featureRender.width != in.render.width || im.featureRender.height != in.render.height ||
        im.featureOutput.width != out.width || im.featureOutput.height != out.height)
    {
        if (im.feature)
        {
            ctx_.waitIdle();
            NVSDK_NGX_VULKAN_ReleaseFeature(im.feature);
            im.feature = nullptr;
        }
        // The transformer model (preset D) for every mode.
        for (const char* p : { NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                               NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
                               NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                               NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance })
        {
            NVSDK_NGX_Parameter_SetUI(im.params, p, NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D);
        }
        NVSDK_NGX_DLSSD_Create_Params cp{};
        cp.InDenoiseMode        = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        cp.InRoughnessMode      = NVSDK_NGX_DLSS_Roughness_Mode_Packed; // normals.w
        cp.InUseHWDepth         = NVSDK_NGX_DLSS_Depth_Type_HW;
        cp.InWidth              = in.render.width;
        cp.InHeight             = in.render.height;
        cp.InTargetWidth        = out.width;
        cp.InTargetHeight       = out.height;
        cp.InPerfQualityValue   = perfQuality(mode_);
        // Ray Reconstruction takes HDR colour only ("HDR Color required"): the game's 0..1 linear colour is a valid HDR input.
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        const auto res = NGX_VULKAN_CREATE_DLSSD_EXT1(ctx_.device(), cmd, 1, 1, &im.feature, im.params, &cp);
        if (NVSDK_NGX_FAILED(res))
        {
            spdlog::warn("dlss: Ray Reconstruction feature creation failed ({:#x}); Super Resolution with the plain denoiser instead",
                         static_cast<uint32_t>(res));
            rrAvailable_ = false;
            return false;
        }
        im.featureRender = in.render;
        im.featureOutput = out;
        im.featureRR     = true;
        spdlog::info("dlss: Ray Reconstruction {}x{} -> {}x{} ({})", in.render.width, in.render.height, out.width, out.height, dlssModeName(mode_));
    }
    NVSDK_NGX_Resource_VK color   = resource(*in.color, in.colorView, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK depth   = resource(*in.depth, in.depth->view, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    NVSDK_NGX_Resource_VK motion  = resource(*in.motion, in.motion->view, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK output  = resource(*in.output, in.output->view, VK_IMAGE_ASPECT_COLOR_BIT, true);
    NVSDK_NGX_Resource_VK diffuse = resource(*in.diffuseAlbedo, in.diffuseAlbedo->view, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK spec    = resource(*in.specularAlbedo, in.specularAlbedo->view, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK normals = resource(*in.normals, in.normals->view, VK_IMAGE_ASPECT_COLOR_BIT, false);
    // The matrices as 16 floats in glm's column-major order (INFERRED: the SDK states no convention for Vulkan).
    glm::mat4 w2v = in.worldToView, v2c = in.viewToClip;
    NVSDK_NGX_VK_DLSSD_Eval_Params ep{};
    ep.pInColor                  = &color;
    ep.pInOutput                 = &output;
    ep.pInDepth                  = &depth;
    ep.pInMotionVectors          = &motion;
    ep.pInDiffuseAlbedo          = &diffuse;
    ep.pInSpecularAlbedo         = &spec;
    ep.pInNormals                = &normals;
    ep.pInRoughness              = &normals;
    ep.InJitterOffsetX           = in.jitter.x;
    ep.InJitterOffsetY           = in.jitter.y;
    ep.InRenderSubrectDimensions = { in.render.width, in.render.height };
    ep.InReset                   = in.reset ? 1 : 0;
    ep.InMVScaleX                = 1.0f;
    ep.InMVScaleY                = 1.0f;
    ep.InPreExposure             = 1.0f;
    ep.InExposureScale           = 1.0f;
    ep.pInWorldToViewMatrix      = &w2v[0][0];
    ep.pInViewToClipMatrix       = &v2c[0][0];
    const auto res = NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd, im.feature, im.params, &ep);
    if (NVSDK_NGX_FAILED(res))
    {
        spdlog::warn("dlss: Ray Reconstruction evaluate failed ({:#x})", static_cast<uint32_t>(res));
        return false;
    }
    return true;
}

#else // built without the SDK: --dlss logs and the client renders as always.

bool dlssBuilt()
{
    return false;
}

struct Dlss::Impl
{
};

ContextExtensions Dlss::extensions()
{
    return {};
}

Dlss::Dlss(Context& ctx, DlssMode mode)
: ctx_(ctx)
, mode_(mode)
{
    spdlog::warn("dlss: this build has no DLSS (configure with -DXI_DLSS=ON -DXI_DLSS_SDK=<sdk>); rendering without it");
}

Dlss::~Dlss() = default;

VkExtent2D Dlss::renderExtent(VkExtent2D output)
{
    return output;
}

bool Dlss::evaluate(VkCommandBuffer, const Inputs&)
{
    return false;
}

#endif

} // namespace xi::gfx
