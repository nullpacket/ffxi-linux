// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xi::dat
{

// Texture chunk (types 0x20 / 0x1B). Layout: docs/dat-format.md §4.
struct Texture
{
    enum class Format : uint8_t
    {
        DXT1,
        DXT3,
        DXT5,
        BGRA8, // expanded from palette / direct colour
    };

    std::string            name; // 16-char "category name" field, trailing spaces kept
    uint32_t               width  = 0;
    uint32_t               height = 0;
    Format                 format = Format::BGRA8;
    std::vector<std::byte> data; // DXT blocks, or width*height*4 BGRA

    // Bytes of block-compressed data, or of BGRA8 pixels.
    [[nodiscard]] bool compressed() const
    {
        return format != Format::BGRA8;
    }
};

// Parses a texture chunk payload (the bytes after the 16-byte chunk header). Throws on malformed input.
Texture parseTexture(std::span<const std::byte> payload);

// Decodes any texture to tightly packed RGBA8 (for previews/tests; the renderer uploads DXT directly).
std::vector<uint8_t> toRgba(const Texture& tex);

// Minimal RGBA8 PNG writer (zlib deflate, no filtering).
void writePng(const std::string& path, uint32_t width, uint32_t height, std::span<const uint8_t> rgba);

} // namespace xi::dat
