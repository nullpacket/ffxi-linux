// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/upscaler.h"

#include <memory>
#include <optional>
#include <string_view>

namespace xi::gfx
{

// AMD FidelityFX Super Resolution 3.1, the upscaler only (issue #139, docs/fsr.md): temporal upscaling on any Vulkan
// GPU (AMD, Intel, NVIDIA), the same inputs DLSS takes. Built with XI_FSR (on by default; the SDK is fetched, MIT);
// used with --fsr or --upscaler=fsr.
enum class FsrMode
{
    NativeAa,         // 1.0x: anti-aliasing only
    Quality,          // 1.5x per axis
    Balanced,         // 1.7x
    Performance,      // 2.0x
    UltraPerformance, // 3.0x
};

// "quality", "balanced", "performance", "ultra-performance", "native" (or "dlaa", DLSS's name for it); empty = Quality.
[[nodiscard]] std::optional<FsrMode> parseFsrMode(std::string_view s);
[[nodiscard]] const char*            fsrModeName(FsrMode m);
// The output / render ratio per axis (AMD's fixed ratios).
[[nodiscard]] float fsrUpscale(FsrMode m);
// AMD's texture LOD bias for a mode: log2(render / output) - 1 (FSR 3 guide, "Mipmap biasing").
[[nodiscard]] float fsrLodBias(FsrMode m);
// The render size for an output size: output / ratio per axis, at least 1.
[[nodiscard]] VkExtent2D fsrRenderExtent(VkExtent2D output, FsrMode m);
// Built with the SDK (XI_FSR).
[[nodiscard]] bool fsrBuilt();

class Fsr : public Upscaler
{
public:
    // Sets up the SDK's Vulkan back end; the upscaler's context is made on the first evaluate().
    Fsr(Context& ctx, FsrMode mode, float sharpness = 0.2f);
    ~Fsr() override;
    Fsr(const Fsr&)            = delete;
    Fsr& operator=(const Fsr&) = delete;

    [[nodiscard]] bool ok() const override
    {
        return ok_;
    }
    [[nodiscard]] const char* name() const override
    {
        return "FSR";
    }
    [[nodiscard]] FsrMode mode() const
    {
        return mode_;
    }
    [[nodiscard]] VkExtent2D renderExtent(VkExtent2D output) override
    {
        return fsrRenderExtent(output, mode_);
    }
    [[nodiscard]] float upscale() const override
    {
        return fsrUpscale(mode_);
    }
    bool evaluate(VkCommandBuffer cmd, const UpscaleInputs& in) override;

private:
    struct Impl;
    Context&              ctx_;
    FsrMode               mode_;
    float                 sharpness_ = 0.2f;
    bool                  ok_        = false;
    std::unique_ptr<Impl> impl_;
};

} // namespace xi::gfx
