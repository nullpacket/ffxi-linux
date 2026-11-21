// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/texture_set.h"

#include "gfx/raytracing.h"

#include "dat/hd_textures.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include <spdlog/spdlog.h>

namespace xi::gfx
{

float& TextureSet::lodBias()
{
    static float bias = 0.0f;
    return bias;
}

TextureSet::TextureSet(Context& ctx)
: ctx_(ctx)
{
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter        = VK_FILTER_LINEAR;
    sci.minFilter        = VK_FILTER_LINEAR;
    sci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.anisotropyEnable = VK_TRUE;
    sci.maxAnisotropy    = 16.0f;
    sci.maxLod           = VK_LOD_CLAMP_NONE;
    sci.mipLodBias       = lodBias();
    VK_CHECK(vkCreateSampler(ctx_.device(), &sci, nullptr, &sampler_));

    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = kMaxTextures;
    binding.stageFlags      = rtGeometryUsage() ? VkShaderStageFlags{ VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT } : VkShaderStageFlags{ VK_SHADER_STAGE_FRAGMENT_BIT }; // --rt: hit shading samples them
    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    bfi.bindingCount  = 1;
    bfi.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.pNext        = &bfi;
    lci.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = 1;
    lci.pBindings    = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_.device(), &lci, nullptr, &layout_));

    VkDescriptorPoolSize       ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pci.maxSets       = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &ps;
    VK_CHECK(vkCreateDescriptorPool(ctx_.device(), &pci, nullptr, &pool_));

    const uint32_t                                     count = kMaxTextures;
    VkDescriptorSetVariableDescriptorCountAllocateInfo vai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO };
    vai.descriptorSetCount = 1;
    vai.pDescriptorCounts  = &count;
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.pNext              = &vai;
    ai.descriptorPool     = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &layout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx_.device(), &ai, &set_));
}

TextureSet::~TextureSet()
{
    ctx_.waitIdle();
    for (auto& img : images_)
    {
        ctx_.destroyImage(img);
    }
    vkDestroyDescriptorPool(ctx_.device(), pool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_.device(), layout_, nullptr);
    vkDestroySampler(ctx_.device(), sampler_, nullptr);
}

uint32_t TextureSet::add(const dat::Texture& tex)
{
    return add(std::vector<const dat::Texture*>{ &tex }).front();
}

std::vector<uint32_t> TextureSet::add(const std::vector<const dat::Texture*>& texs)
{
    std::vector<Decoded>        decoded;
    std::vector<const Decoded*> ptrs;
    decoded.reserve(texs.size());
    for (const auto* tex : texs)
    {
        decoded.push_back(decode(*tex));
    }
    for (const auto& d : decoded)
    {
        ptrs.push_back(&d);
    }
    std::vector<Buffer>   staging;
    std::vector<uint32_t> ids;
    ctx_.immediateSubmit([&](VkCommandBuffer cmd) { ids = record(ptrs, cmd, staging); });
    for (auto& b : staging)
    {
        ctx_.destroyBuffer(b);
    }
    return ids;
}

TextureSet::Decoded TextureSet::decode(const dat::Texture& tex)
{
    // --hd-textures (goal 41): the upscaled copy from the cache when there is one; same UVs, just more texels.
    if (const auto& dir = dat::hd::runtimeDir())
    {
        const auto key = dat::hd::key(tex);
        if (auto img = dat::hd::load(*dir, key))
        {
            spdlog::debug("hd-textures: {} {}x{} -> {}x{}", key, tex.width, tex.height, img->width, img->height);
            return Decoded{ img->width, img->height, std::move(img->rgba), key };
        }
        Decoded d{ tex.width, tex.height, dat::toRgba(tex), key };
        if (dat::hd::dumpMissing() && tex.width >= 8 && tex.height >= 8)
        {
            const auto p = *dir / "pending" / (key + ".png");
            std::error_code ec;
            if (!std::filesystem::exists(p, ec))
            {
                std::filesystem::create_directories(p.parent_path(), ec);
                dat::writePng(p.string(), d.width, d.height, d.rgba);
            }
        }
        return d;
    }
    return Decoded{ tex.width, tex.height, dat::toRgba(tex), {} };
}

std::vector<uint32_t> TextureSet::record(const std::vector<const Decoded*>& texs, VkCommandBuffer cmd, std::vector<Buffer>& staging)
{
    if (images_.size() + texs.size() > kMaxTextures)
    {
        throw std::runtime_error("TextureSet full");
    }
    // FFXI textures have no mip chain. Decode to RGBA8 and build mips on the GPU with linear blits; this trades
    // ~4x memory against DXT for stable distant surfaces. (Possible later optimisation: GPU BC re-encode.)
    struct Pending
    {
        Image    img;
        Buffer   staging;
        uint32_t width, height, mips;
        std::string key;
    };
    std::vector<Pending> pending;
    pending.reserve(texs.size());
    for (const auto* tex : texs)
    {
        const VkExtent2D extent{ tex->width, tex->height };
        uint32_t         mips = 1;
        for (uint32_t d = std::max(tex->width, tex->height); d > 1; d >>= 1)
        {
            ++mips;
        }
        const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM; // raw values, like D3D8 (see Context swapchain format)
        auto img     = ctx_.createImage(extent, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        VK_IMAGE_ASPECT_COLOR_BIT, mips);
        auto buf     = ctx_.createBuffer(tex->rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(buf.mapped, tex->rgba.data(), tex->rgba.size());
        pending.push_back({ img, buf, tex->width, tex->height, mips, tex->hdKey });
    }

    {
        for (const auto& p : pending)
        {
            Context::transition(cmd, p.img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent      = { p.width, p.height, 1 };
            vkCmdCopyBufferToImage(cmd, p.staging.buffer, p.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            int32_t w = static_cast<int32_t>(p.width), h = static_cast<int32_t>(p.height);
            for (uint32_t m = 1; m < p.mips; ++m)
            {
                Context::transition(cmd, p.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1);
                const int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
                VkImageBlit   blit{};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1 };
                blit.srcOffsets[1]  = { w, h, 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 };
                blit.dstOffsets[1]  = { nw, nh, 1 };
                vkCmdBlitImage(cmd, p.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                Context::transition(cmd, p.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1);
                w = nw;
                h = nh;
            }
            Context::transition(cmd, p.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, p.mips - 1, 1);
        }
    }

    std::vector<uint32_t> ids;
    ids.reserve(pending.size());
    for (auto& p : pending)
    {
        staging.push_back(p.staging);
        images_.push_back(p.img);
        keys_.push_back(p.key);
        ids.push_back(static_cast<uint32_t>(images_.size() - 1));
    }
    return ids;
}

void TextureSet::commit()
{
    if (committed_ == images_.size())
    {
        return;
    }
    std::vector<VkDescriptorImageInfo> infos;
    for (uint32_t i = committed_; i < images_.size(); ++i)
    {
        infos.push_back({ sampler_, images_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
    }
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet          = set_;
    w.dstBinding      = 0;
    w.dstArrayElement = committed_;
    w.descriptorCount = static_cast<uint32_t>(infos.size());
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = infos.data();
    vkUpdateDescriptorSets(ctx_.device(), 1, &w, 0, nullptr);
    committed_ = static_cast<uint32_t>(images_.size());
}

} // namespace xi::gfx
