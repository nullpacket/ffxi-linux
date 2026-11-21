// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/context.h"
#include "gfx/upscaler.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace xi::gfx
{

// NVIDIA DLSS 4 Super Resolution / DLAA through NGX on Vulkan (docs/dlss.md, PLANS goal 15). Only built with the
// CMake option XI_DLSS and the SDK (XI_DLSS_SDK); only used with the client's --dlss flag. Without both, nothing
// here runs and the client renders as it always has.
enum class DlssMode
{
    Dlaa,             // native resolution, anti-aliasing only
    Quality,          // ~67 % of the output per axis
    Balanced,         // ~58 %
    Performance,      // 50 %
    UltraPerformance, // ~33 %
};

// "quality", "balanced", "performance", "ultra-performance", "dlaa"; empty = Quality.
[[nodiscard]] std::optional<DlssMode> parseDlssMode(std::string_view s);
[[nodiscard]] const char*             dlssModeName(DlssMode m);
// The render / output ratio per axis the mode aims at (the SDK's optimal sizes match it closely).
[[nodiscard]] float dlssRatio(DlssMode m);
// NVIDIA's texture LOD bias for a mode: log2(render / output) - 1 (DLSS guide §3.5); XI_DLSS_LOD_BIAS overrides.
[[nodiscard]] float dlssLodBias(DlssMode m);
// Built with the SDK (XI_DLSS).
[[nodiscard]] bool dlssBuilt();

// Sub-pixel jitter: Halton (2, 3), centred, in render pixels; the sequence length grows with the upscale ratio.
[[nodiscard]] glm::vec2 dlssJitter(uint64_t frame, float upscale);
// Adds a clip-space offset (NDC units) to a view-projection, as a jittered projection would.
[[nodiscard]] glm::mat4 jitterViewProj(const glm::mat4& viewProj, glm::vec2 ndcOffset);

class Dlss : public Upscaler
{
public:
    // The instance / device extensions NGX asks for; empty when DLSS isn't built in. Before the Context.
    [[nodiscard]] static ContextExtensions extensions();

    // Initialises NGX and checks Super Resolution is available. ok() is false (with a logged reason) otherwise.
    Dlss(Context& ctx, DlssMode mode);
    ~Dlss() override;
    Dlss(const Dlss&)            = delete;
    Dlss& operator=(const Dlss&) = delete;

    [[nodiscard]] bool ok() const override
    {
        return ok_;
    }
    [[nodiscard]] const char* name() const override
    {
        return "DLSS";
    }
    [[nodiscard]] float upscale() const override
    {
        return 1.0f / dlssRatio(mode_);
    }
    [[nodiscard]] DlssMode mode() const
    {
        return mode_;
    }
    // The render resolution for an output size (the SDK's optimal setting for the mode).
    [[nodiscard]] VkExtent2D renderExtent(VkExtent2D output) override;

    using Inputs = UpscaleInputs;
    // Ray Reconstruction (NGX's dlssd feature) is there on this GPU / driver / SDK.
    [[nodiscard]] bool rayReconstructionAvailable() const override
    {
        return rrAvailable_;
    }
    // Records the evaluation (and the feature's (re)creation when the sizes change). False on failure.
    bool evaluate(VkCommandBuffer cmd, const Inputs& in) override;

private:
    bool evaluateRR(VkCommandBuffer cmd, const Inputs& in);
    struct Impl;
    Context&              ctx_;
    DlssMode              mode_;
    bool                  ok_ = false;
    bool                  rrAvailable_ = false;
    std::unique_ptr<Impl> impl_;
};

} // namespace xi::gfx
