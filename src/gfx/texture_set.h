// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/texture.h"
#include "gfx/context.h"

#include <string>
#include <vector>

namespace xi::gfx
{

// Uploads DAT textures (DXT1/3/5 as BC1/2/3, paletted as BGRA8) and exposes them as one bindless
// sampled-image array, so a draw selects its texture with an index in a push constant / vertex attribute.
class TextureSet
{
public:
    static constexpr uint32_t kMaxTextures = 4096;

    explicit TextureSet(Context& ctx);
    ~TextureSet();
    // The mip LOD bias of sets made from now on (0 = as always). DLSS sets it negative (goal 15: the textures are
    // sampled for the output resolution, not the lower render resolution).
    static float& lodBias();
    TextureSet(const TextureSet&)            = delete;
    TextureSet& operator=(const TextureSet&) = delete;

    // Uploads one texture; returns its index in the bindless array.
    uint32_t add(const dat::Texture& tex);
    // Several textures in one upload (one queue wait instead of one each); returns their indices in order.
    std::vector<uint32_t> add(const std::vector<const dat::Texture*>& texs);

    // A texture decoded to RGBA8 (CPU only, any thread: the character loader decodes on its worker).
    struct Decoded
    {
        uint32_t             width = 0, height = 0;
        std::vector<uint8_t> rgba;
        std::string          hdKey; // with --hd-textures: its cache key (dat/hd_textures.h), else empty
    };
    static Decoded decode(const dat::Texture& tex);
    // Creates the images and records their copies and mips into `cmd`; the staging buffers are appended to
    // `staging` (the caller frees them once `cmd` has run). Returns the indices; commit() before drawing with them.
    std::vector<uint32_t> record(const std::vector<const Decoded*>& texs, VkCommandBuffer cmd, std::vector<Buffer>& staging);

    // An image made elsewhere (a render target the caller keeps in SHADER_READ_ONLY_OPTIMAL while it is sampled); the
    // set owns it from now on. Returns its index; commit() before drawing with it.
    uint32_t addImage(Image image)
    {
        images_.push_back(image);
        keys_.emplace_back();
        return static_cast<uint32_t>(images_.size() - 1);
    }

    // With --hd-textures, texture i's cache key: dat::hd::load(*dat::hd::runtimeDir(), hdKey(i), "_n" / "_r") gives
    // its generated normal / roughness map (for --rt / --pathtrace). Empty without the flag or for render targets.
    [[nodiscard]] const std::string& hdKey(uint32_t i) const
    {
        return keys_[i];
    }

    // Writes all uploaded textures into the descriptor set (call after a batch of add()).
    void commit();

    [[nodiscard]] VkDescriptorSetLayout layout() const
    {
        return layout_;
    }
    [[nodiscard]] VkDescriptorSet set() const
    {
        return set_;
    }
    [[nodiscard]] uint32_t size() const
    {
        return static_cast<uint32_t>(images_.size());
    }
    [[nodiscard]] VkExtent2D extent(uint32_t i) const
    {
        return images_[i].extent;
    }

private:
    Context&              ctx_;
    VkSampler             sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_  = VK_NULL_HANDLE;
    VkDescriptorPool      pool_    = VK_NULL_HANDLE;
    VkDescriptorSet       set_     = VK_NULL_HANDLE;
    std::vector<Image>       images_;
    std::vector<std::string> keys_; // parallel to images_
    uint32_t                 committed_ = 0;
};

} // namespace xi::gfx
