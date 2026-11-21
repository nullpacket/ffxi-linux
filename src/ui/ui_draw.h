// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/ui.h"
#include "gfx/context.h"

#include <imgui.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Draws the game's own interface (dat/ui.h) through Dear ImGui's draw lists, with a pipeline of our own that
// reproduces the client's per-quad blend modes and MODULATE2X colour (docs/dat-format.md §14).
namespace xi::ui
{

// The client's interface blend modes (GS-style (A-B)*C+Cd, mapped at 0x1011f230).
enum class Blend : uint8_t
{
    Opaque,
    Alpha,        // (0,1,1): SRCALPHA, INVSRCALPHA
    Add,          // (0,2,1): SRCALPHA, ONE
    Replace,      // (0,2,2): SRCALPHA, ZERO
    Subtract,     // (2,0,1): Cd - Cs*As
    InvAlpha,     // (1,0,0): INVSRCALPHA, SRCALPHA
    OneSrcAlpha,  // (1,2,0): ONE, SRCALPHA
    Count,
};
Blend blendOf(const std::array<uint8_t, 4>& quadBlend);

// Colour in the interface's 0x80 = 1.0 convention.
struct UiColor
{
    uint8_t r = 0x80, g = 0x80, b = 0x80, a = 0x80;
};

class UiRenderer
{
public:
    UiRenderer(gfx::Context& ctx, const dat::UiResources& res);
    ~UiRenderer();
    UiRenderer(const UiRenderer&)            = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    struct Texture
    {
        ImTextureID id = ImTextureID_Invalid;
        float       width = 0, height = 0;
    };
    // The menu textures are uploaded with the renderer; an invalid id for names the resources do not hold.
    const Texture& texture(const std::string& name);
    // An extra texture under a private key (item icons).
    const Texture& texture(const std::string& key, const dat::Texture& tex);

    [[nodiscard]] const dat::UiResources& resources() const
    {
        return res_;
    }
    [[nodiscard]] float advance(unsigned char c) const;
    // Advance of a "font moji" cell (see mojiCell).
    [[nodiscard]] float cellAdvance(int cell) const;
    // The "font moji" cell of the character at s[i] (64 cells a row, 16 x 16): ASCII 0x20..0x7F are cells 0..95, then
    // Shift-JIS in code order from 0x8140, 188 cells per lead byte (0x40..0x7E, 0x80..0xFC; DATA: the texture holds the
    // full-width symbols, kana and kanji in that order, e.g. the item names' 0x8189 male sign). Moves i past it.
    static int mojiCell(std::string_view s, std::size_t& i);
    // A two-byte code drawn from "font usgaiji" instead (CODE 0x1011c870 / 0x1011cdf0: the code's JIS index k with
    // 0x2115 < k < 0x2144 is shape k - 0x2115), e.g. EF 27 / EF 28, the auto-translate brackets (shapes 8 / 9, "menu
    // markgree" / "menu markred"). The shape, or -1 (i is not moved).
    static int gaijiShape(std::string_view s, std::size_t i);
    // A "font usgaiji" shape's advance (maxX - minX).
    [[nodiscard]] float gaijiAdvance(int shape) const;
    // The client's own "font moji" width table (dat::mojiWidthsFromDll); replaces the widths measured from the ink.
    void setMojiWidths(const std::vector<uint8_t>& widths);
    // Advance of a "font fontshp" glyph in authored units (its quad's width minus one).
    [[nodiscard]] float shpAdvance(unsigned char c) const;

    // Draw-callback plumbing (used by UiPainter).
    struct Slot
    {
        UiRenderer* self = nullptr;
        Blend       mode = Blend::Alpha;
    };
    [[nodiscard]] const Slot* slot(Blend b) const
    {
        return &slots_[static_cast<std::size_t>(b)];
    }

    // ImDrawCallback: binds the pipeline of the Slot in UserCallbackData.
    static void bind(const ImDrawList*, const ImDrawCmd* cmd);

private:
    VkPipeline  pipeline(Blend b, VkPipelineLayout layout);
    void        upload(const std::vector<std::pair<std::string, const dat::Texture*>>& texs); // one submit for the batch

    gfx::Context&                            ctx_;
    const dat::UiResources&                  res_;
    std::unordered_map<std::string, Texture> textures_;
    std::vector<gfx::Image>                  images_;
    std::vector<VkDescriptorSet>             sets_;
    std::array<Slot, static_cast<std::size_t>(Blend::Count)>       slots_{};
    std::array<VkPipeline, static_cast<std::size_t>(Blend::Count)> pipelines_{};
    VkPipelineLayout                         pipelineLayout_ = VK_NULL_HANDLE;
    std::array<uint8_t, 8192>                advance_{};
};

// Records interface draws into one ImGui draw list. Positions are screen pixels; `scale` multiplies authored units.
// Restores ImGui's own pipeline when destroyed, so ImGui widgets drawn later into the same list are unaffected.
class UiPainter
{
public:
    UiPainter(UiRenderer& r, ImDrawList& dl, float scale);
    ~UiPainter();
    UiPainter(const UiPainter&)            = delete;
    UiPainter& operator=(const UiPainter&) = delete;

    [[nodiscard]] float scale() const
    {
        return scale_;
    }
    [[nodiscard]] UiRenderer& renderer()
    {
        return r_;
    }

    // A sprite with its origin at `pos`; `mod` multiplies the authored colours (0x80 = unchanged). Quads before
    // `firstQuad` are skipped (a window's own sprite starts at 1: quad 0 is the background the frame replaces).
    // `sx`/`sy` scale the authored offsets further (gauge fills), pivot at the origin.
    void sprite(const dat::UiSprite& s, ImVec2 pos, UiColor mod = {}, std::size_t firstQuad = 0, float sx = 1, float sy = 1);
    void sprite(const std::string& sheet, std::size_t index, ImVec2 pos, UiColor mod = {}, std::size_t firstQuad = 0, float sx = 1, float sy = 1);
    // A sprite rotated by `angle` radians (clockwise on screen) about its origin, scaled by `size` on top of the
    // painter scale (map icons, 0x1011fe30).
    void spriteRotated(const dat::UiSprite& s, ImVec2 pos, float angle, float size = 1, UiColor mod = {});
    // A sprite through a 2x2 transform of its authored offsets (screen x = m[0] x + m[1] y, y = m[2] x + m[3] y, on top
    // of the painter scale), mirrored quads kept (the compass dial, 0x1011fea0).
    void spriteAffine(const dat::UiSprite& s, ImVec2 pos, const std::array<float, 4>& m, UiColor mod = {});
    // Text in "font fontshp" (the menu font: glyph = sprite c - 0x20); returns the advance in screen pixels.
    // rightAlign: `pos.x` is the right edge.
    float textShp(std::string_view s, ImVec2 pos, UiColor mod = {}, bool rightAlign = false);
    [[nodiscard]] float textShpWidth(std::string_view s) const; // authored units
    // Quad 0 of a sprite drawn into a rect, its texture tiled at authored scale (the frame pieces, 0x1011e9f0).
    // skin: the window skin's pieces, whose vertex alpha keeps the loader's x1.5 (see quadColor).
    // opaque: draw without blending and with full alpha (window backgrounds; issue #51).
    void tiled(const dat::UiSprite& s, ImVec2 pos, ImVec2 size, UiColor mod = {}, bool skin = false, bool opaque = false);
    // The skin's 9-slice window frame, `size` in authored units (0x102345f0).
    void frame(ImVec2 pos, ImVec2 size, bool altBackground = false, uint8_t alpha = 0x80);
    // The "font moji" text font; returns the x after the last glyph. Colour as authored (0x80 = white).
    float text(std::string_view s, ImVec2 pos, UiColor c = {});
    [[nodiscard]] float textWidth(std::string_view s) const; // screen pixels
    // Clip following draws to a screen rectangle (nested; pop in order).
    void pushClip(ImVec2 min, ImVec2 max)
    {
        dl_.PushClipRect(min, max, true);
    }
    void popClip()
    {
        dl_.PopClipRect();
    }
    // A solid rectangle, `size` in authored units.
    void rect(ImVec2 pos, ImVec2 size, UiColor c);
    // Draw following quads with blend `b` instead of their authored one (effect particles: the pop-up glow is
    // additive); nullopt restores the authored blends.
    void overrideBlend(std::optional<Blend> b)
    {
        blendOverride_ = b;
    }
    // An item / status icon or a map (32x32 authored units by default). Drawn without blending, alpha-tested: the icon
    // textures store opaque as alpha 0x80, which under MODULATE2X with vertex alpha 0x80 is only half (issue #90).
    void icon(const std::string& key, const dat::Texture& tex, ImVec2 pos, UiColor mod = {}, float size = 32);

private:
    std::optional<Blend> blendOverride_;
    void setBlend(Blend b);
    void quad(const UiRenderer::Texture& t, const ImVec2 (&p)[4], const ImVec2 (&uv)[4], const ImU32 (&c)[4]);

    UiRenderer& r_;
    ImDrawList& dl_;
    float       scale_;
    Blend       blend_ = Blend::Count;
};

} // namespace xi::ui
