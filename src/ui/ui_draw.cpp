// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/ui_draw.h"

#include "gfx/pipeline.h"

#include <SDL3/SDL.h>
#include <imgui_impl_vulkan.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace xi::ui
{

namespace
{

ImU32 vertexColor(UiColor c)
{
    return IM_COL32(c.r, c.g, c.b, c.a);
}

// Quad colours as the client loads them (0x1011e410): R, G, B, A + 1 unless 0xFF (A also unless 0). The loader also
// scales A by 1.5, but a lossless retail capture (party window, window type 6, 2026-10-19) matches only without it:
// with it the bar tracks and fills come out too strong. Where the 1.5 is undone was not traced.
// Then multiplied by the caller's modifier, min(255, c * m >> 7) per channel (0x1011ffe0).
// Window skin pieces (UiPainter::frame) keep the 1.5: the skin formula traced at 0x10117700 / 0x102345f0
// (ui-format-notes §"Formula to implement (skin pieces)": A = int((a + 1) * 1.5)). The retail capture that dropped it
// was of the bars over window type 6, whose background is drawn without blending (alpha unused); the blended types
// (1: background alpha 0x20..0x60) came out about a third too see-through, so overlapping windows mixed (issue #51).
// XI_UI_SKIN_ALPHA_OLD=1 draws skins with A + 1 as before.
ImU32 quadColor(const std::array<uint8_t, 4>& k, UiColor mod, bool skin = false)
{
    auto ch = [](int v, uint8_t m) { return static_cast<uint8_t>(std::min(255, (v * m) >> 7)); };
    auto rgb = [](uint8_t v) { return v == 0xFF ? 0xFF : v + 1; };
    static const bool skinOld = std::getenv("XI_UI_SKIN_ALPHA_OLD") != nullptr;
    const int a = k[3] == 0 || k[3] == 0xFF ? k[3] : skin && !skinOld ? std::min(255, (k[3] + 1) * 3 / 2) : k[3] + 1;
    return IM_COL32(ch(rgb(k[0]), mod.r), ch(rgb(k[1]), mod.g), ch(rgb(k[2]), mod.b), ch(a, mod.a));
}

} // namespace

Blend blendOf(const std::array<uint8_t, 4>& q)
{
    if (q[0] == 0)
    {
        return Blend::Opaque;
    }
    const int abc = q[1] * 100 + q[2] * 10 + q[3];
    switch (abc)
    {
    case 11: return Blend::Alpha;
    case 21: return Blend::Add;
    case 22: return Blend::Replace;
    case 201: return Blend::Subtract;
    case 100: return Blend::InvAlpha;
    case 120: return Blend::OneSrcAlpha;
    case 20:
    case 110: return Blend::Opaque; // "blending disabled"
    default: return Blend::Alpha;   // rare combinations: the client keeps the previous factors
    }
}

UiRenderer::UiRenderer(gfx::Context& ctx, const dat::UiResources& res)
: ctx_(ctx)
, res_(res)
{
    for (std::size_t i = 0; i < slots_.size(); ++i)
    {
        slots_[i] = { this, static_cast<Blend>(i) };
    }
    // Advance widths of the text font, measured from the glyph ink (the client's own table is not traced yet).
    advance_.fill(14); // full-width cells until the game's table (setMojiWidths) arrives (INFERRED)
    std::fill(advance_.begin(), advance_.begin() + 96, uint8_t{ 8 });
    if (const auto it = res.textures.find("font moji"); it != res.textures.end() && it->second.width >= 64 * 16)
    {
        const auto rgba = dat::toRgba(it->second);
        const auto w    = it->second.width;
        for (uint32_t c = 0; c < 96; ++c)
        {
            const uint32_t cx = (c % 64) * 16, cy = (c / 64) * 16;
            int            right = -1;
            for (uint32_t y = 0; y < 16; ++y)
            {
                for (uint32_t x = 0; x < 16; ++x)
                {
                    if (rgba[4 * ((cy + y) * w + cx + x) + 3] > 40)
                    {
                        right = std::max(right, static_cast<int>(x));
                    }
                }
            }
            advance_[c] = static_cast<uint8_t>(right < 0 ? 4 : right + 1);
        }
    }
    // Every menu texture now, in one submit: uploading each on its first draw stalled that frame (the 1024x2048 "font
    // moji" ~10 ms on the first in-zone frame, docs/performance.md goal 25). Item icons and the like still load on use.
    std::vector<std::pair<std::string, const dat::Texture*>> all;
    for (const auto& [name, tex] : res.textures)
    {
        if (tex.width > 0 && tex.height > 0)
        {
            all.emplace_back(name, &tex);
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    upload(all);
    spdlog::debug("ui: {} textures uploaded in {:.1f} ms", all.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

UiRenderer::~UiRenderer()
{
    ctx_.waitIdle();
    for (auto s : sets_)
    {
        ImGui_ImplVulkan_RemoveTexture(s);
    }
    for (auto& img : images_)
    {
        ctx_.destroyImage(img);
    }
    for (auto p : pipelines_)
    {
        if (p)
        {
            vkDestroyPipeline(ctx_.device(), p, nullptr);
        }
    }
}

float UiRenderer::advance(unsigned char c) const
{
    return advance_[c >= 0x20 && c < 0x80 ? c - 0x20 : '?' - 0x20];
}

float UiRenderer::cellAdvance(int cell) const
{
    return advance_[cell >= 0 && cell < static_cast<int>(advance_.size()) ? cell : '?' - 0x20];
}

int UiRenderer::mojiCell(std::string_view s, std::size_t& i)
{
    const auto u = static_cast<unsigned char>(s[i]);
    if (u < 0x80)
    {
        ++i;
        return u >= 0x20 ? u - 0x20 : '?' - 0x20;
    }
    if (((u >= 0x81 && u <= 0x9F) || (u >= 0xE0 && u <= 0xEF)) && i + 1 < s.size())
    {
        const auto t = static_cast<unsigned char>(s[i + 1]);
        if (t >= 0x40 && t <= 0xFC && t != 0x7F)
        {
            i += 2;
            const int lead = u < 0xA0 ? u - 0x81 : u - 0xC1;
            const int cell = 96 + lead * 188 + (t - 0x40 - (t >= 0x80 ? 1 : 0));
            return cell < 8192 ? cell : '?' - 0x20;
        }
    }
    ++i;
    return '?' - 0x20;
}

int UiRenderer::gaijiShape(std::string_view s, std::size_t i)
{
    // CODE 0x1011c870: Shift-JIS -> JIS row / cell -> index; only the range the gaiji font covers is followed here.
    if (i + 1 >= s.size())
    {
        return -1;
    }
    int lead  = static_cast<unsigned char>(s[i]);
    int trail = static_cast<unsigned char>(s[i + 1]);
    if (!((lead >= 0x81 && lead <= 0x9F) || (lead >= 0xE0 && lead <= 0xEF)))
    {
        return -1;
    }
    int row = ((lead <= 0x9F ? lead - 0x71 : lead - 0xB1) * 2) | 1;
    if (trail > 0x7F)
    {
        --trail;
    }
    if (trail >= 0x9E)
    {
        ++row;
        trail -= 0x7D;
    }
    else
    {
        trail -= 0x1F;
    }
    const int jis = row << 8 | trail;
    if (jis < 0x7921 || jis >= 0x9320)
    {
        return -1;
    }
    const int k = trail + row * 47 * 2 - 0xCD1;
    return k > 0x2115 && k < 0x2144 ? k - 0x2115 : -1;
}

float UiRenderer::gaijiAdvance(int shape) const
{
    const auto* g = res_.sprite("font usgaiji", static_cast<std::size_t>(shape));
    if (!g || g->empty())
    {
        return 8;
    }
    int lo = 1 << 20, hi = -(1 << 20);
    for (const auto& q : *g)
    {
        for (int k = 0; k < 4; ++k)
        {
            lo = std::min(lo, static_cast<int>(q.xy[2 * k]));
            hi = std::max(hi, static_cast<int>(q.xy[2 * k]));
        }
    }
    return static_cast<float>(std::max(1, hi - lo));
}

void UiRenderer::setMojiWidths(const std::vector<uint8_t>& widths)
{
    for (std::size_t i = 0; i < advance_.size() && i < widths.size(); ++i)
    {
        advance_[i] = widths[i];
    }
}

float UiRenderer::shpAdvance(unsigned char c) const
{
    const auto* g = res_.sprite("font fontshp", c >= 0x20 ? c - 0x20u : 0u);
    if (!g || g->empty())
    {
        return 4;
    }
    const auto& q = (*g)[0];
    return static_cast<float>(std::max(1, q.xy[2] - q.xy[0] - 1));
}

const UiRenderer::Texture& UiRenderer::texture(const std::string& name)
{
    if (const auto it = textures_.find(name); it != textures_.end())
    {
        return it->second;
    }
    const auto src = res_.textures.find(name);
    if (src == res_.textures.end())
    {
        spdlog::debug("ui: no texture '{}'", name);
        return textures_[name];
    }
    return texture(name, src->second);
}

const UiRenderer::Texture& UiRenderer::texture(const std::string& key, const dat::Texture& tex)
{
    if (const auto it = textures_.find(key); it != textures_.end())
    {
        return it->second;
    }
    upload({ { key, &tex } });
    return textures_[key];
}

void UiRenderer::upload(const std::vector<std::pair<std::string, const dat::Texture*>>& texs)
{
    // One submit (and one wait) for the whole batch.
    struct Pending
    {
        gfx::Image  img;
        gfx::Buffer staging;
        VkExtent2D  extent{};
    };
    std::vector<Pending> pending;
    for (const auto& [key, tex] : texs)
    {
        const auto       rgba = dat::toRgba(*tex);
        Pending          p;
        p.extent  = { tex->width, tex->height };
        p.img     = ctx_.createImage(p.extent, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        p.staging = ctx_.createBuffer(rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(p.staging.mapped, rgba.data(), rgba.size());
        pending.push_back(p);
    }
    ctx_.immediateSubmit([&](VkCommandBuffer cmd) {
        for (const auto& p : pending)
        {
            gfx::Context::transition(cmd, p.img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent      = { p.extent.width, p.extent.height, 1 };
            vkCmdCopyBufferToImage(cmd, p.staging.buffer, p.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            gfx::Context::transition(cmd, p.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });
    for (std::size_t i = 0; i < pending.size(); ++i)
    {
        auto& p = pending[i];
        ctx_.destroyBuffer(p.staging);
        const auto set = ImGui_ImplVulkan_AddTexture(VK_NULL_HANDLE, p.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        images_.push_back(p.img);
        sets_.push_back(set);
        auto& e  = textures_[texs[i].first];
        e.id     = reinterpret_cast<ImTextureID>(set);
        e.width  = static_cast<float>(p.extent.width);
        e.height = static_cast<float>(p.extent.height);
    }
}

VkPipeline UiRenderer::pipeline(Blend b, VkPipelineLayout layout)
{
    auto& p = pipelines_[static_cast<std::size_t>(b)];
    if (p && layout == pipelineLayout_)
    {
        return p;
    }
    if (layout != pipelineLayout_)
    {
        // ImGui recreated its pipeline layout (backend re-init): ours must match it.
        for (auto& old : pipelines_)
        {
            if (old)
            {
                vkDestroyPipeline(ctx_.device(), old, nullptr);
                old = VK_NULL_HANDLE;
            }
        }
        pipelineLayout_ = layout;
    }
    const auto         dir  = gfx::shaderDir();
    gfx::PipelineDesc  pd;
    pd.vert        = gfx::loadShader(ctx_.device(), dir / "ui.vert.spv");
    pd.frag        = gfx::loadShader(ctx_.device(), dir / "ui.frag.spv");
    pd.layout      = layout;
    pd.colorFormat = ctx_.colorFormat();
    pd.depthFormat = VK_FORMAT_D32_SFLOAT; // ImGui draws inside the scene pass (depth attached, unused)
    pd.bindings    = { { 0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX } };
    pd.attributes  = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos) },
                       { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv) },
                       { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col) } };
    VkPipelineColorBlendAttachmentState bs{};
    bs.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    bs.blendEnable         = b != Blend::Opaque;
    bs.colorBlendOp        = VK_BLEND_OP_ADD;
    bs.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    bs.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    bs.alphaBlendOp        = VK_BLEND_OP_ADD;
    switch (b)
    {
    case Blend::Opaque:
    case Blend::Alpha:
        bs.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        bs.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case Blend::Add:
        bs.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        bs.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case Blend::Replace:
        bs.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        bs.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case Blend::Subtract:
        bs.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        bs.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        bs.colorBlendOp        = VK_BLEND_OP_REVERSE_SUBTRACT;
        break;
    case Blend::InvAlpha:
        bs.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        bs.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        break;
    case Blend::OneSrcAlpha:
        bs.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        bs.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        break;
    case Blend::Count: break;
    }
    pd.blend = bs;
    p        = gfx::createPipeline(ctx_.device(), pd, /*depthTest=*/false);
    vkDestroyShaderModule(ctx_.device(), pd.vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), pd.frag, nullptr);
    return p;
}

void UiRenderer::bind(const ImDrawList*, const ImDrawCmd* cmd)
{
    const auto* slot = static_cast<const Slot*>(cmd->UserCallbackData);
    auto*       rs   = static_cast<ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    vkCmdBindPipeline(rs->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, slot->self->pipeline(slot->mode, rs->PipelineLayout));
}

// --- UiPainter ---

UiPainter::UiPainter(UiRenderer& r, ImDrawList& dl, float scale)
: r_(r)
, dl_(dl)
, scale_(scale)
{
}

UiPainter::~UiPainter()
{
    if (blend_ != Blend::Count)
    {
        dl_.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
}

void UiPainter::setBlend(Blend b)
{
    b = blendOverride_.value_or(b);
    if (b != blend_)
    {
        dl_.AddCallback(&UiRenderer::bind, const_cast<UiRenderer::Slot*>(r_.slot(b)));
        blend_ = b;
    }
}

void UiPainter::quad(const UiRenderer::Texture& t, const ImVec2 (&p)[4], const ImVec2 (&uv)[4], const ImU32 (&c)[4])
{
    dl_.PushTexture(ImTextureRef(t.id));
    dl_.PrimReserve(6, 4);
    const auto base = static_cast<ImDrawIdx>(dl_._VtxCurrentIdx);
    // Corners are TL, TR, BL, BR: triangles TL-TR-BR and TL-BR-BL.
    for (const int i : { 0, 1, 3, 0, 3, 2 })
    {
        dl_.PrimWriteIdx(static_cast<ImDrawIdx>(base + i));
    }
    for (int i = 0; i < 4; ++i)
    {
        dl_.PrimWriteVtx(p[i], uv[i], c[i]);
    }
    dl_.PopTexture();
}

void UiPainter::sprite(const dat::UiSprite& s, ImVec2 pos, UiColor mod, std::size_t firstQuad, float sx, float sy)
{
    for (std::size_t qi = firstQuad; qi < s.size(); ++qi)
    {
        const auto& q = s[qi];
        // "menu newtex" is the client's runtime text texture (each list row's text is rendered into it, with colour 0
        // in the data): this client draws text itself, so those quads are skipped (they would be opaque black).
        if (q.texture == "menu newtex")
        {
            continue;
        }
        const auto& t = r_.texture(q.texture);
        if (t.id == ImTextureID_Invalid || q.w == 0 || q.h == 0)
        {
            continue;
        }
        float u0 = q.u / t.width, v0 = q.v / t.height;
        float u1 = (q.u + q.w) / t.width, v1 = (q.v + q.h) / t.height;
        if (q.flags & 1)
        {
            std::swap(u0, u1);
        }
        if (q.flags & 2)
        {
            std::swap(v0, v1);
        }
        const ImVec2 uv[4] = { { u0, v0 }, { u1, v0 }, { u0, v1 }, { u1, v1 } };
        // Glyphs of "font fontshp" (texture "font font", texel alpha 0x77 / 0x88) keep the loader's 1.5 on their quad
        // alpha, as the window skins do: with A + 1 alone they came out about half transparent, so white text read as
        // grey (issue #70; the lossless retail party capture's name is ~200 over the dark window, ours was ~150).
        static const bool fontOld = std::getenv("XI_UI_FONT_ALPHA_OLD") != nullptr;
        const bool        font    = !fontOld && q.texture == "font font";
        ImVec2            p[4];
        ImU32             c[4];
        for (int i = 0; i < 4; ++i)
        {
            p[i] = { pos.x + q.xy[2 * i] * scale_ * sx, pos.y + q.xy[2 * i + 1] * scale_ * sy };
            c[i] = quadColor(q.rgba[i], mod, font);
        }
        setBlend(blendOf(q.blend));
        quad(t, p, uv, c);
    }
}

void UiPainter::sprite(const std::string& sheet, std::size_t index, ImVec2 pos, UiColor mod, std::size_t firstQuad, float sx, float sy)
{
    if (const auto* s = r_.resources().sprite(sheet, index))
    {
        sprite(*s, pos, mod, firstQuad, sx, sy);
    }
}

void UiPainter::spriteRotated(const dat::UiSprite& s, ImVec2 pos, float angle, float size, UiColor mod)
{
    const float cs = std::cos(angle), sn = std::sin(angle);
    for (const auto& q : s)
    {
        const auto& t = r_.texture(q.texture);
        if (t.id == ImTextureID_Invalid || q.w == 0 || q.h == 0)
        {
            continue;
        }
        const float  u0 = q.u / t.width, v0 = q.v / t.height, u1 = (q.u + q.w) / t.width, v1 = (q.v + q.h) / t.height;
        const ImVec2 uv[4] = { { u0, v0 }, { u1, v0 }, { u0, v1 }, { u1, v1 } };
        ImVec2       p[4];
        ImU32        c[4];
        for (int i = 0; i < 4; ++i)
        {
            const float x = q.xy[2 * i] * scale_ * size, y = q.xy[2 * i + 1] * scale_ * size;
            p[i]          = { pos.x + x * cs - y * sn, pos.y + x * sn + y * cs };
            c[i]          = quadColor(q.rgba[i], mod);
        }
        setBlend(blendOf(q.blend));
        quad(t, p, uv, c);
    }
}

void UiPainter::spriteAffine(const dat::UiSprite& s, ImVec2 pos, const std::array<float, 4>& m, UiColor mod)
{
    for (const auto& q : s)
    {
        const auto& t = r_.texture(q.texture);
        if (t.id == ImTextureID_Invalid || q.w == 0 || q.h == 0)
        {
            continue;
        }
        float u0 = q.u / t.width, v0 = q.v / t.height, u1 = (q.u + q.w) / t.width, v1 = (q.v + q.h) / t.height;
        if (q.flags & 1)
        {
            std::swap(u0, u1);
        }
        if (q.flags & 2)
        {
            std::swap(v0, v1);
        }
        const ImVec2 uv[4] = { { u0, v0 }, { u1, v0 }, { u0, v1 }, { u1, v1 } };
        ImVec2       p[4];
        ImU32        c[4];
        for (int i = 0; i < 4; ++i)
        {
            const float x = q.xy[2 * i] * scale_, y = q.xy[2 * i + 1] * scale_;
            p[i] = { pos.x + m[0] * x + m[1] * y, pos.y + m[2] * x + m[3] * y };
            c[i] = quadColor(q.rgba[i], mod);
        }
        setBlend(blendOf(q.blend));
        quad(t, p, uv, c);
    }
}

float UiPainter::textShpWidth(std::string_view s) const
{
    float w = 0;
    for (std::size_t i = 0; i < s.size();)
    {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80)
        {
            // A Shift-JIS character (e.g. the male / female signs in mannequin names): the text font's glyph.
            w += r_.cellAdvance(UiRenderer::mojiCell(s, i));
            continue;
        }
        w += r_.shpAdvance(c);
        ++i;
    }
    return w;
}

float UiPainter::textShp(std::string_view s, ImVec2 pos, UiColor mod, bool rightAlign)
{
    const float w = textShpWidth(s) * scale_;
    float       x = rightAlign ? pos.x - w : pos.x;
    for (std::size_t i = 0; i < s.size();)
    {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80)
        {
            // "font fontshp" has ASCII only: a Shift-JIS character comes from the text font (INFERRED: how the game
            // draws them in this font was not traced).
            const std::size_t at = i;
            const int         cell = UiRenderer::mojiCell(s, i);
            text(s.substr(at, i - at), ImVec2(x, pos.y - 2 * scale_), mod);
            x += r_.cellAdvance(cell) * scale_;
            continue;
        }
        if (c > 0x20)
        {
            sprite("font fontshp", c - 0x20u, ImVec2(x, pos.y), mod);
        }
        x += r_.shpAdvance(c) * scale_;
        ++i;
    }
    return w;
}

void UiPainter::tiled(const dat::UiSprite& s, ImVec2 pos, ImVec2 size, UiColor mod, bool skin, bool opaque)
{
    if (s.empty() || size.x <= 0 || size.y <= 0)
    {
        return;
    }
    const auto& q = s[0];
    const auto& t = r_.texture(q.texture);
    if (t.id == ImTextureID_Invalid || q.w == 0 || q.h == 0)
    {
        return;
    }
    // The client draws one quad with wrapped UVs; ImGui's sampler clamps, so emit one quad per tile.
    setBlend(opaque ? Blend::Opaque : blendOf(q.blend));
    for (float ty = 0; ty < size.y; ty += q.h)
    {
        for (float tx = 0; tx < size.x; tx += q.w)
        {
            const float  w = std::min<float>(q.w, size.x - tx), h = std::min<float>(q.h, size.y - ty);
            const float  u0 = q.u / t.width, v0 = q.v / t.height, u1 = (q.u + w) / t.width, v1 = (q.v + h) / t.height;
            const ImVec2 a(pos.x + tx * scale_, pos.y + ty * scale_), b(a.x + w * scale_, a.y + h * scale_);
            const ImVec2 p[4]  = { a, { b.x, a.y }, { a.x, b.y }, b };
            const ImVec2 uv[4] = { { u0, v0 }, { u1, v0 }, { u0, v1 }, { u1, v1 } };
            ImU32        c[4];
            for (int i = 0; i < 4; ++i)
            {
                c[i] = quadColor(q.rgba[i], mod, skin);
                if (opaque)
                {
                    c[i] |= IM_COL32(0, 0, 0, 255);
                }
            }
            quad(t, p, uv, c);
        }
    }
}

void UiPainter::frame(ImVec2 pos, ImVec2 size, bool altBackground, uint8_t alpha)
{
    const dat::UiSpriteSheet* skin = nullptr;
    for (const auto& [name, sheet] : r_.resources().sheets)
    {
        if (name.starts_with("menu win0"))
        {
            skin = &sheet;
            break;
        }
    }
    if (!skin || skin->sprites.size() < 10)
    {
        return;
    }
    const UiColor mod{ 0x80, 0x80, 0x80, alpha };
    const auto&   pc = skin->sprites;
    const float   W = size.x, H = size.y;
    auto at = [&](float x, float y) { return ImVec2(pos.x + x * scale_, pos.y + y * scale_); };
    // The background is solid in the game for every window type (user, issue #51): the blended reading of skin 14's
    // background quad (vertex alpha 0x20..0x60) left windows see-through, which the game's windows never are. So it is
    // drawn without blending while the window is fully shown; a fading window (alpha < 0x80, the open animation)
    // still blends. XI_UI_TRANSLUCENT_WINDOWS=1 brings back the blended background.
    static const bool translucent = std::getenv("XI_UI_TRANSLUCENT_WINDOWS") != nullptr;
    tiled(pc[altBackground ? 9 : 0], at(0, 0), { W, H }, mod, true, !translucent && alpha >= 0x80);
    tiled(pc[2], at(24, 0), { W - 48, 5 }, mod, true);
    tiled(pc[7], at(24, H - 5), { W - 48, 5 }, mod, true);
    if (H > 48)
    {
        tiled(pc[4], at(0, 24), { 5, H - 48 }, mod, true);
        tiled(pc[5], at(W - 5, 24), { 5, H - 48 }, mod, true);
    }
    tiled(pc[1], at(0, 0), { 24, 24 }, mod, true);
    tiled(pc[3], at(W - 24, 0), { 24, 24 }, mod, true);
    tiled(pc[6], at(0, H - 24), { 24, 24 }, mod, true);
    tiled(pc[8], at(W - 24, H - 24), { 24, 24 }, mod, true);
}

float UiPainter::textWidth(std::string_view s) const
{
    float w = 0;
    for (std::size_t i = 0; i < s.size();)
    {
        if (const int g = UiRenderer::gaijiShape(s, i); g >= 0)
        {
            w += r_.gaijiAdvance(g);
            i += 2;
            continue;
        }
        w += r_.cellAdvance(UiRenderer::mojiCell(s, i));
    }
    return w * scale_;
}

float UiPainter::text(std::string_view s, ImVec2 pos, UiColor c)
{
    const auto& t = r_.texture("font moji");
    if (t.id == ImTextureID_Invalid)
    {
        return pos.x;
    }
    setBlend(Blend::Alpha);
    const ImU32 col = vertexColor(c);
    const ImU32 cs[4] = { col, col, col, col };
    float       x   = pos.x;
    for (std::size_t i = 0; i < s.size();)
    {
        if (const int g = UiRenderer::gaijiShape(s, i); g >= 0)
        {
            // A gaiji glyph keeps its own colours (the auto-translate brackets' green / red).
            sprite("font usgaiji", static_cast<std::size_t>(g), ImVec2(std::round(x), std::round(pos.y)), UiColor{ 0x80, 0x80, 0x80, c.a });
            x += r_.gaijiAdvance(g) * scale_;
            i += 2;
            setBlend(Blend::Alpha);
            continue;
        }
        const int cell = UiRenderer::mojiCell(s, i);
        if (cell != 0)
        {
            const float  u0 = static_cast<float>(cell % 64) * 16 / t.width, v0 = static_cast<float>(cell / 64) * 16 / t.height;
            const float  u1 = u0 + 16 / t.width, v1 = v0 + 16 / t.height;
            const ImVec2 a(std::round(x), std::round(pos.y)), b(a.x + 16 * scale_, a.y + 16 * scale_);
            const ImVec2 p[4]  = { a, { b.x, a.y }, { a.x, b.y }, b };
            const ImVec2 uv[4] = { { u0, v0 }, { u1, v0 }, { u0, v1 }, { u1, v1 } };
            quad(t, p, uv, cs);
        }
        x += r_.cellAdvance(cell) * scale_;
    }
    return x;
}

void UiPainter::rect(ImVec2 pos, ImVec2 size, UiColor c)
{
    // ImGui's white texel through our MODULATE2X pipeline: the colour keeps the 0x80 = 1.0 convention.
    setBlend(Blend::Alpha);
    dl_.AddRectFilled(pos, ImVec2(pos.x + size.x * scale_, pos.y + size.y * scale_), vertexColor(c));
}

void UiPainter::icon(const std::string& key, const dat::Texture& tex, ImVec2 pos, UiColor mod, float size)
{
    const auto& t = r_.texture(key, tex);
    if (t.id == ImTextureID_Invalid)
    {
        return;
    }
    // The game's icon sprite (CODE 0x100268c0 / 0x10026a00, item icons through 0x1015b4b0) only turns blending on when
    // its caller asks (ALPHABLENDENABLE + BLENDOP ADD, then off again); the item grids, lists and the status strip
    // don't, so icons are drawn with blending off and only the alpha test (alpha 0 is cut). Their textures keep
    // alpha 0x80 = opaque (item records, file 87), which with blending on and vertex alpha 0x80 came out half
    // see-through (#90). INFERRED: the state the icons inherit is blending off (the inherited render state was not
    // traced; retail icons are opaque). Not done: the scaled draw's ALPHAREF 0x60 with linear filtering (0x10026a00).
    setBlend(Blend::Opaque);
    const ImVec2 b(pos.x + size * scale_, pos.y + size * scale_);
    const ImVec2 p[4]  = { pos, { b.x, pos.y }, { pos.x, b.y }, b };
    const ImVec2 uv[4] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    const ImU32  c     = vertexColor(mod);
    quad(t, p, uv, { c, c, c, c });
}

} // namespace xi::ui
