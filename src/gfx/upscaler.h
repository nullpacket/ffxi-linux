// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/context.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>

namespace xi::gfx
{

// A temporal upscaler the client renders through (issue #139): NVIDIA DLSS (gfx/dlss.h, NVIDIA RTX only) or AMD FSR
// 3.1 (gfx/fsr.h, any Vulkan GPU). The scene renders at renderExtent() with a sub-pixel jitter; evaluate() writes the
// window-size image the interface draws over.
struct UpscaleInputs
{
    const Image* color     = nullptr; // render size, SHADER_READ_ONLY_OPTIMAL, SAMPLED usage
    VkImageView  colorView = VK_NULL_HANDLE;
    const Image* depth     = nullptr; // render size, SHADER_READ_ONLY_OPTIMAL (depth aspect), reverse-Z, infinite far
    const Image* motion    = nullptr; // render size, R16G16_SFLOAT, previous - current in render pixels
    const Image* output    = nullptr; // output size, GENERAL, STORAGE usage
    VkExtent2D   render{};
    glm::vec2    jitter{ 0.0f };  // render pixels, as added to the projection
    bool         reset   = false; // a new scene / camera cut: no history
    float        frameMs = 16.7f; // the time since the last frame (FSR)
    float        fovY    = 0.0f;  // radians (FSR); 0 = unknown
    float        nearZ   = 0.1f;  // the camera's near plane (FSR)
    // --rt with Ray Reconstruction (DLSS only, goal 38): the guides; set = denoise with DLSS-RR instead of upscaling only.
    const Image* diffuseAlbedo  = nullptr; // render size, GENERAL
    const Image* specularAlbedo = nullptr;
    const Image* normals        = nullptr; // world normal xyz, roughness in w (packed)
    glm::mat4    worldToView{ 1.0f }, viewToClip{ 1.0f };
};

class Upscaler
{
public:
    virtual ~Upscaler() = default;

    // Set up and ready; false (with a logged reason) otherwise, and the client renders without it.
    [[nodiscard]] virtual bool ok() const = 0;
    // "DLSS", "FSR".
    [[nodiscard]] virtual const char* name() const = 0;
    // The render resolution for an output size.
    [[nodiscard]] virtual VkExtent2D renderExtent(VkExtent2D output) = 0;
    // The output / render ratio per axis the mode aims at (the jitter sequence and the texture LOD bias follow it).
    [[nodiscard]] virtual float upscale() const = 0;
    // Records the upscale (and the context's (re)creation when the sizes change). False on failure.
    virtual bool evaluate(VkCommandBuffer cmd, const UpscaleInputs& in) = 0;
    // DLSS Ray Reconstruction (denoises --rt / --pathtrace from the guides) is there.
    [[nodiscard]] virtual bool rayReconstructionAvailable() const
    {
        return false;
    }
};

// Which upscaler the client starts (issue #139), from --upscaler / --dlss / --fsr and what this GPU and build can
// run. --dlss on a GPU without DLSS (AMD, Intel, a build without the SDK) falls back to FSR with the same mode.
enum class UpscalerKind
{
    None,
    Dlss,
    Fsr,
};
struct UpscalerChoice
{
    UpscalerKind kind = UpscalerKind::None;
    std::string  mode; // the mode name for that upscaler ("quality" ...; FSR also takes "dlaa" as "native")
    std::string  note; // why it differs from what was asked ("" when it doesn't)
};
// upscaler: "", "dlss", "fsr", "auto", "off"; dlssMode / fsrMode: the --dlss= / --fsr= values ("" = not given).
[[nodiscard]] UpscalerChoice chooseUpscaler(std::string_view upscaler, std::string_view dlssMode,
                                            std::string_view fsrMode, bool dlssPossible, bool fsrPossible);

} // namespace xi::gfx
