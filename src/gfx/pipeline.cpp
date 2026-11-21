// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/pipeline.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <fstream>

namespace xi::gfx
{

VkShaderModule loadShader(VkDevice device, const std::filesystem::path& spvPath)
{
    std::ifstream f(spvPath, std::ios::binary | std::ios::ate);
    if (!f)
    {
        throw std::runtime_error(fmt::format("cannot open shader {}", spvPath.string()));
    }
    std::vector<uint32_t> code(static_cast<std::size_t>(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size() * 4));
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = code.size() * 4;
    ci.pCode    = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
    return m;
}

std::filesystem::path shaderDir()
{
    static const std::filesystem::path dir = [] {
        if (const char* env = std::getenv("XI_SHADER_DIR"); env && *env)
        {
            return std::filesystem::path(env);
        }
        const char*                 base = SDL_GetBasePath();
        const std::filesystem::path exe  = base ? base : ".";
        std::error_code             ec;
        // The build tree keeps them next to the executables; an install puts them in <prefix>/share, found from
        // <prefix>/bin so a moved (unpacked) install still works, then at the configured prefix.
        for (const auto& cand : { exe / "shaders", exe / ".." / "share" / "ffxi-linux" / "shaders", std::filesystem::path(XI_INSTALL_SHADER_DIR) })
        {
            if (std::filesystem::exists(cand / "mesh.vert.spv", ec))
            {
                return cand.lexically_normal();
            }
        }
        return exe / "shaders";
    }();
    return dir;
}

VkPipeline createPipeline(VkDevice device, const PipelineDesc& d, bool depthTest)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, d.vert, "main", nullptr };
    VkSpecializationMapEntry specEntry{ 0, 0, sizeof(uint32_t) };
    VkSpecializationInfo     spec{ 1, &specEntry, sizeof(uint32_t), &d.fragSpec0 };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, d.frag, "main", &spec };

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = static_cast<uint32_t>(d.bindings.size());
    vi.pVertexBindingDescriptions      = d.bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(d.attributes.size());
    vi.pVertexAttributeDescriptions    = d.attributes.data();

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = d.topology;

    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = d.cullMode;
    rs.frontFace   = d.frontFace;
    rs.lineWidth   = 1.0f;
    if (d.depthBias)
    {
        // Reverse-Z: toward the camera is larger depth, so the bias is positive.
        rs.depthBiasEnable         = VK_TRUE;
        rs.depthBiasConstantFactor = 2.0f;
        rs.depthBiasSlopeFactor    = 1.0f;
    }

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = depthTest && d.depthFormat != VK_FORMAT_UNDEFINED;
    ds.depthWriteEnable = depthTest && d.depthFormat != VK_FORMAT_UNDEFINED && !d.alphaBlend;
    ds.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL; // reverse-Z

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (d.alphaBlend)
    {
        blend.blendEnable         = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp        = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    }
    if (d.blend)
    {
        blend = *d.blend;
    }
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments    = &blend;

    const VkDynamicState             dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyn;

    VkPipelineRenderingCreateInfo rci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rci.colorAttachmentCount    = 1;
    rci.pColorAttachmentFormats = &d.colorFormat;
    rci.depthAttachmentFormat   = d.depthFormat;

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.pNext               = &rci;
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState   = &ms;
    pci.pDepthStencilState  = &ds;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &dy;
    pci.layout              = d.layout;
    VkPipeline p            = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &p));
    return p;
}

} // namespace xi::gfx
