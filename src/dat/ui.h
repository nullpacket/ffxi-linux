// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"
#include "dat/texture.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Interface resources: sprite sheets (chunk type 0x31), window layouts (0x30) and their textures
// (docs/dat-format.md §14).
namespace xi::dat
{

// One textured quad of a sprite.
struct UiQuad
{
    std::array<int16_t, 8>                xy{};  // corners TL, TR, BL, BR (x, y) relative to the sprite origin
    uint16_t                              u = 0, v = 0, w = 0, h = 0; // texel rect in the texture
    uint8_t                               flags = 0; // bit 0 mirror U, bit 1 mirror V
    std::array<std::array<uint8_t, 4>, 4> rgba{};    // per corner, as stored (see §14.1 for the load-time adjustment)
    std::array<uint8_t, 4>                blend{};   // {enable, A, B, C}: PS2 GS blend (A-B)*C+Cd; 0 Cs, 1 Cd, 2 zero
    std::string                           texture;   // normalized texture name (see uiName)
};

using UiSprite = std::vector<UiQuad>;

// Chunk type 0x31: a named list of sprites.
struct UiSpriteSheet
{
    std::string           name;
    std::vector<UiSprite> sprites;

    static UiSpriteSheet parse(std::span<const std::byte> payload);
};

// One drawable reference of a layout record: sprite `shape` of sheet `sheet`, shown in visual state `type`
// (0 always; see §14.2).
struct UiSub
{
    uint16_t    type  = 0;
    uint16_t    shape = 0;
    std::string sheet; // normalized; language aliases resolved ("menu frames" -> "menu framesus")
};

// The window record and the element records of a layout share this shape (§14.2).
struct UiRecord
{
    int16_t                x = 0, y = 0;             // window: 640x480 design space; element: relative to the window
    int16_t                cursorX = 0, cursorY = 0; // pointing-hand offset
    int16_t                w = 0, h = 0;             // window: frame size; element: hit rect
    int16_t                frameX = 0, frameY = 0;   // window: frame offset; element: unknown (+0x0E/+0x10)
    uint16_t               id     = 0;               // element id (1-based)
    uint8_t                anchor = 0;               // window: bit 0 right, bit 1 bottom
    bool                   noAnchor = false;
    std::array<uint8_t, 6> nav{};                    // element: prev, next, up, down, right, left (0xFF = not selectable)
    std::vector<UiSub>     subs;
    int                    text1 = -1, text2 = -1;   // string ids (help text, INFERRED); -1 = none

    // The sub drawn for visual state `state` (0 normal, 5 disabled, ...); nullptr if none.
    [[nodiscard]] const UiSub* subFor(int state) const;
};

// Chunk type 0x30: a window layout.
struct UiLayout
{
    std::string           name; // "menu menuwind"
    UiRecord              window;
    std::vector<UiRecord> elements;

    [[nodiscard]] const UiRecord* element(uint16_t id) const;

    static UiLayout parse(std::span<const std::byte> payload);
};

// Lower-cased, with the 8+8 space padding squeezed out: "menu    newtex  " -> "menu newtex".
std::string uiName(std::span<const std::byte> name16);

// Everything the interface draws, gathered from the English menu files plus one window skin.
struct UiResources
{
    std::unordered_map<std::string, Texture>       textures;
    std::unordered_map<std::string, UiSpriteSheet> sheets;
    std::unordered_map<std::string, UiLayout>      layouts;

    [[nodiscard]] const UiSprite* sprite(const std::string& sheet, std::size_t index) const;

    // Loads the given files in order; later files override earlier names.
    static UiResources load(const DatIndex& index, std::span<const uint32_t> fileIds);
};

// English in-game menus, English title/lobby menus, and window skin n (0..7, the "Window type" option).
constexpr uint32_t kUiMenuFile  = 39542;
constexpr uint32_t kUiIconFile  = 39551; // "menu magiconw" / "magico2w" (spell list icons)
constexpr uint32_t kUiLobbyFile = 39541;
constexpr uint32_t uiSkinFile(unsigned n)
{
    return 14 + (n & 7);
}

} // namespace xi::dat
