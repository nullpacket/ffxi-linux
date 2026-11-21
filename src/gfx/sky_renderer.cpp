// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/sky_renderer.h"

#include "gfx/pipeline.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <filesystem>

namespace xi::gfx
{

namespace
{

struct SkyPush
{
    glm::vec4       forward; // xyz, w = tan(fovY/2)
    glm::vec4       right;   // xyz, w = aspect
    glm::vec4       up;
    VkDeviceAddress gradient;
};
static_assert(sizeof(SkyPush) <= kClientPushConstantBytes); // Vulkan's guaranteed push-constant size (issue #139)

} // namespace

SkyRenderer::SkyRenderer(Context& ctx)
: ctx_(ctx)
{
    for (auto& b : buffers_)
    {
        b = ctx_.createBuffer(sizeof(GradientGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    }
    VkPushConstantRange        pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyPush) };
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcr;
    VK_CHECK(vkCreatePipelineLayout(ctx_.device(), &lci, nullptr, &layout_));
    const auto   dir  = shaderDir();
    PipelineDesc pd;
    pd.vert        = loadShader(ctx_.device(), dir / "sky.vert.spv");
    pd.frag        = loadShader(ctx_.device(), dir / "sky.frag.spv");
    pd.layout      = layout_;
    pd.colorFormat = ctx_.colorFormat();
    pd.depthFormat = VK_FORMAT_D32_SFLOAT; // attachment present; the pipeline disables test/write below
    pipeline_      = createPipeline(ctx_.device(), pd, /*depthTest=*/false);
    vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
}

SkyRenderer::~SkyRenderer()
{
    ctx_.waitIdle();
    for (auto& b : buffers_)
    {
        ctx_.destroyBuffer(b);
    }
    vkDestroyPipeline(ctx_.device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device(), layout_, nullptr);
}

void SkyRenderer::draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::vec3& forward, const glm::vec3& right, const glm::vec3& up, float fovY, float aspect,
                       const Gradient& g)
{
    auto&        b   = buffers_[frameSlot % buffers_.size()];
    GradientGpu& gpu = *static_cast<GradientGpu*>(b.mapped);
    for (int i = 0; i < 8; ++i)
    {
        gpu.colors[i] = glm::vec4(g.colors[static_cast<std::size_t>(i)], 1.0f);
    }
    gpu.positions[0] = glm::vec4(g.positions[0], g.positions[1], g.positions[2], g.positions[3]);
    gpu.positions[1] = glm::vec4(g.positions[4], g.positions[5], g.positions[6], g.positions[7]);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    SkyPush p{ glm::vec4(forward, std::tan(fovY * 0.5f)), glm::vec4(right, aspect), glm::vec4(up, 0.0f), ctx_.address(b) };
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace xi::gfx
