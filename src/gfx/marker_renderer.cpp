// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/marker_renderer.h"

#include "gfx/pipeline.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace xi::gfx
{

namespace
{

struct MarkerPush
{
    glm::mat4       viewProj;
    VkDeviceAddress instances;
};
static_assert(sizeof(MarkerPush) <= kClientPushConstantBytes); // Vulkan's guaranteed push-constant size (issue #139)

} // namespace

MarkerRenderer::MarkerRenderer(Context& ctx)
: ctx_(ctx)
{
    for (auto& b : buffers_)
    {
        b = ctx_.createBuffer(sizeof(Instance) * kMaxMarkers, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    }
    VkPushConstantRange        pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(MarkerPush) };
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcr;
    VK_CHECK(vkCreatePipelineLayout(ctx_.device(), &lci, nullptr, &layout_));

    const auto  dir  = shaderDir();
    PipelineDesc pd;
    pd.vert        = loadShader(ctx_.device(), dir / "marker.vert.spv");
    pd.frag        = loadShader(ctx_.device(), dir / "marker.frag.spv");
    pd.layout      = layout_;
    pd.colorFormat = ctx_.colorFormat();
    pd.depthFormat = VK_FORMAT_D32_SFLOAT;
    pd.cullMode    = VK_CULL_MODE_BACK_BIT;
    pipeline_      = createPipeline(ctx_.device(), pd);
    vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
}

MarkerRenderer::~MarkerRenderer()
{
    ctx_.waitIdle();
    for (auto& b : buffers_)
    {
        ctx_.destroyBuffer(b);
    }
    vkDestroyPipeline(ctx_.device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device(), layout_, nullptr);
}

void MarkerRenderer::draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const std::vector<Marker>& markers)
{
    const auto count = static_cast<uint32_t>(std::min<std::size_t>(markers.size(), kMaxMarkers));
    if (count == 0)
    {
        return;
    }
    auto& buf  = buffers_[frameSlot % buffers_.size()];
    auto* inst = static_cast<Instance*>(buf.mapped);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& m = markers[i];
        inst[i]       = { glm::vec4(m.feet, m.height), m.color, glm::vec4(m.width, 0, 0, 0) };
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    MarkerPush p{ viewProj, ctx_.address(buf) };
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 36, count, 0, 0);
}

} // namespace xi::gfx
