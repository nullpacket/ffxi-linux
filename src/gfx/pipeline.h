// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/vk.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace xi::gfx
{

VkShaderModule loadShader(VkDevice device, const std::filesystem::path& spvPath);

// The compiled shaders' folder: XI_SHADER_DIR, else shaders/ next to the executable (the build tree), else the
// install's share/ffxi-linux/shaders.
std::filesystem::path shaderDir();

struct PipelineDesc
{
    VkShaderModule                                 vert = VK_NULL_HANDLE;
    VkShaderModule                                 frag = VK_NULL_HANDLE;
    VkPipelineLayout                               layout = VK_NULL_HANDLE;
    VkFormat                                       colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat                                       depthFormat = VK_FORMAT_UNDEFINED; // UNDEFINED = no depth
    std::vector<VkVertexInputBindingDescription>   bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    VkPrimitiveTopology                            topology   = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags                                cullMode   = VK_CULL_MODE_NONE;
    VkFrontFace                                    frontFace  = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool                                           alphaBlend = false;
    uint32_t                                       fragSpec0  = 0; // fragment specialization constant 0
    bool                                           depthBias  = false; // small bias toward the camera (decals/water)
    std::optional<VkPipelineColorBlendAttachmentState> blend;          // overrides alphaBlend when set
};

// Graphics pipeline for dynamic rendering, with dynamic viewport/scissor.
VkPipeline createPipeline(VkDevice device, const PipelineDesc& desc, bool depthTest = true);

} // namespace xi::gfx
