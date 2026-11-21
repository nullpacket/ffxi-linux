// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "client/effects.h"
#include "gfx/context.h"
#include "gfx/texture_set.h"

#include <glm/glm.hpp>

#include <array>
#include <unordered_map>
#include <vector>

namespace xi::gfx
{

// Draws the effect runtime's batches (client::EffectSystem::build): alpha-blended ones first, then additive ones,
// depth-tested without depth writes. Textures are uploaded on first use into an own bindless set.
//
// Effects that sample the screen (batches with `screen`, the scheduler's captured-image layers) draw in a second pass:
// the caller ends rendering, copyScene() copies the scene so far, the caller resumes rendering (load ops LOAD, the depth
// stored by the first pass) and drawScreen() draws them.
class EffectRenderer
{
public:
    static constexpr uint32_t kMaxVertices = 1u << 18;

    explicit EffectRenderer(Context& ctx);
    ~EffectRenderer();
    EffectRenderer(const EffectRenderer&)            = delete;
    EffectRenderer& operator=(const EffectRenderer&) = delete;

    // Uploads textures the batches need; call outside a render pass.
    void prepare(const std::vector<client::EffectSystem::Batch>& batches);
    // Once per frame before draw() / drawScreen(): they append to this frame slot's vertex buffer, in either order
    // (with Ray Reconstruction the screen pass draws before the others, issue #101).
    void beginFrame(uint32_t frameSlot)
    {
        used_[frameSlot % used_.size()] = 0;
    }
    // The batches without `screen`.
    void draw(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const std::vector<client::EffectSystem::Batch>& batches);

    // True when this frame needs the screen pass (screen batches, layers, or a new freeze frame to capture).
    [[nodiscard]] bool needsScreen(const std::vector<client::EffectSystem::Batch>& batches, const std::vector<client::EffectSystem::ScreenLayer>& layers,
                                   uint32_t freezeSerial) const;
    // Outside rendering: copies `scene` (COLOR_ATTACHMENT_OPTIMAL, left so) into this frame's image, and into the frozen
    // image when `freezeSerial` changed.
    void copyScene(VkCommandBuffer cmd, VkImage scene, VkExtent2D extent, uint32_t freezeSerial);
    // The screen copies at this size, made now (a GPU wait and two allocations) rather than in the first frame that needs
    // them; the client calls it every frame, a no-op while the size holds.
    void ensureScreen(VkExtent2D extent);
    // Inside the resumed rendering: the screen batches, then the layers over the whole target.
    void drawScreen(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& viewProj, const std::vector<client::EffectSystem::Batch>& batches,
                    const std::vector<client::EffectSystem::ScreenLayer>& layers);
    // The screen images' set (binding 0 = sampler2D[2]: this frame's copy, the frozen one) for other renderers'
    // screen passes (SkinnedRenderer::drawRefraction), valid after copyScene().
    [[nodiscard]] VkDescriptorSetLayout screenLayout() const
    {
        return screenLayout_;
    }
    [[nodiscard]] VkDescriptorSet screenSet() const
    {
        return screenSet_;
    }
    // A render target that batches sample as texture `key` (Batch::texture compared by address): made on the first
    // call (COLOR_ATTACHMENT | SAMPLED, RGBA8 UNORM) and then returned. The caller renders into it each frame and
    // leaves it in SHADER_READ_ONLY_OPTIMAL before the batches draw (the detailed shadows' silhouettes).
    const Image& renderTexture(const dat::Texture* key, VkExtent2D extent);

private:
    struct Vertex
    {
        glm::vec4 posTex; // render space; w = texture index (-1 = none, -2 = the screen, -3 / -4 layers)
        glm::vec4 color;
        glm::vec4 uv;
        glm::vec4 stage; // texture stage 1's factor (EffectSystem::Vertex::stage)
    };

    Context&                                         ctx_;
    TextureSet                                       textures_;
    std::unordered_map<const dat::Texture*, int>     textureIndex_;
    std::unordered_map<const dat::Texture*, Image>   renderTextures_; // owned by textures_; listed for renderTexture()
    std::array<Buffer, Context::kFramesInFlight>     buffers_;
    std::array<uint32_t, Context::kFramesInFlight>   used_{};  // vertices written by draw() this frame
    VkPipelineLayout                                 layout_   = VK_NULL_HANDLE;
    VkPipeline                                       additive_ = VK_NULL_HANDLE;
    VkPipeline                                       subtract_ = VK_NULL_HANDLE;
    VkPipeline                                       alpha_    = VK_NULL_HANDLE;
    VkPipeline                                       multiply_ = VK_NULL_HANDLE;
    VkPipeline                                       layerAlpha_ = VK_NULL_HANDLE; // no depth test
    VkPipeline                                       layerAdd_   = VK_NULL_HANDLE;
    // Screen images: [0] this frame's copy, [1] the frozen one.
    VkDescriptorSetLayout                            screenLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool                                 screenPool_   = VK_NULL_HANDLE;
    VkDescriptorSet                                  screenSet_    = VK_NULL_HANDLE;
    VkSampler                                        screenSampler_ = VK_NULL_HANDLE;
    std::array<Image, 2>                             screen_{};
    VkExtent2D                                       screenExtent_{};
    uint32_t                                         frozenSerial_ = 0;
    bool                                             frozenValid_  = false;
};

} // namespace xi::gfx
