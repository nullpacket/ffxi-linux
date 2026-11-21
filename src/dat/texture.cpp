// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/texture.h"

#include "xi/bytes.h"

#include <fmt/format.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace xi::dat
{

namespace
{

constexpr std::size_t kInfoHeader = 0x11; // BITMAPINFOHEADER offset within the payload
constexpr std::size_t kBody       = 0x39; // 0x11 + 40

[[noreturn]] void fail(const std::string& what)
{
    throw std::runtime_error("texture: " + what);
}

// RGB565 -> RGB888
std::array<uint8_t, 3> rgb565(uint16_t c)
{
    const auto r = static_cast<uint8_t>((c >> 11) & 0x1F);
    const auto g = static_cast<uint8_t>((c >> 5) & 0x3F);
    const auto b = static_cast<uint8_t>(c & 0x1F);
    return { static_cast<uint8_t>((r << 3) | (r >> 2)), static_cast<uint8_t>((g << 2) | (g >> 4)), static_cast<uint8_t>((b << 3) | (b >> 2)) };
}

// Decodes the 8-byte colour part of a DXT block into 16 RGBA texels (alpha left at 255 unless DXT1 punch-through).
void decodeColourBlock(std::span<const std::byte> blk, bool dxt1, std::array<std::array<uint8_t, 4>, 16>& out)
{
    const auto c0 = load<uint16_t>(blk, 0);
    const auto c1 = load<uint16_t>(blk, 2);
    const auto a  = rgb565(c0);
    const auto b  = rgb565(c1);

    std::array<std::array<uint8_t, 4>, 4> pal{};
    pal[0] = { a[0], a[1], a[2], 255 };
    pal[1] = { b[0], b[1], b[2], 255 };
    if (c0 > c1 || !dxt1)
    {
        for (int i = 0; i < 3; ++i)
        {
            pal[2][i] = static_cast<uint8_t>((2 * a[i] + b[i]) / 3);
            pal[3][i] = static_cast<uint8_t>((a[i] + 2 * b[i]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    }
    else
    {
        for (int i = 0; i < 3; ++i)
        {
            pal[2][i] = static_cast<uint8_t>((a[i] + b[i]) / 2);
        }
        pal[2][3] = 255;
        pal[3]    = { 0, 0, 0, 0 };
    }
    const auto bits = load<uint32_t>(blk, 4);
    for (int t = 0; t < 16; ++t)
    {
        out[t] = pal[(bits >> (2 * t)) & 3];
    }
}

// Pixel i of a bottom-up image, as an index into the same image stored top-down.
std::size_t flippedRow(std::size_t i, uint32_t width, uint32_t height)
{
    const std::size_t y = i / width;
    return (height - 1 - y) * width + i % width;
}

} // namespace

Texture parseTexture(std::span<const std::byte> p)
{
    if (p.size() < kBody)
    {
        fail("payload too small");
    }
    Texture tex;
    tex.name                = loadString(p, 0x01, 16);
    const auto flags        = load<uint8_t>(p, 0);
    const auto kind         = (flags >> 4) & 7;
    tex.width               = load<uint32_t>(p, kInfoHeader + 4) & 0xFFFF;
    tex.height              = load<uint32_t>(p, kInfoHeader + 8) & 0xFFFF;
    const auto bitCount     = load<uint16_t>(p, kInfoHeader + 0xE);
    const auto paletteBits  = load<uint32_t>(p, kInfoHeader + 0x24);
    if (tex.width == 0 || tex.height == 0 || tex.width > 4096 || tex.height > 4096)
    {
        fail(fmt::format("bad dimensions {}x{}", tex.width, tex.height));
    }

    if (kind == 2)
    {
        // Fourcc is stored byte-reversed ("1TXD" for DXT1).
        const auto fourcc = loadString(p, kBody, 4);
        const auto size   = load<uint32_t>(p, kBody + 4);
        if (fourcc == "1TXD")
        {
            tex.format = Texture::Format::DXT1;
        }
        else if (fourcc == "3TXD")
        {
            tex.format = Texture::Format::DXT3;
        }
        else if (fourcc == "5TXD")
        {
            tex.format = Texture::Format::DXT5;
        }
        else
        {
            fail("unknown fourcc " + fourcc);
        }
        const std::size_t blocks    = ((tex.width + 3) / 4) * ((tex.height + 3) / 4);
        const std::size_t blockSize = tex.format == Texture::Format::DXT1 ? 8 : 16;
        if (size < blocks * blockSize || kBody + 12 + blocks * blockSize > p.size())
        {
            fail(fmt::format("DXT data size {} too small for {}x{}", size, tex.width, tex.height));
        }
        const auto body = p.subspan(kBody + 12, blocks * blockSize);
        tex.data.assign(body.begin(), body.end());
        return tex;
    }

    // Paletted / direct colour. Kind 3 has an extra u32 before the palette; kind 0 behaves as kind 1. The pixel rows are
    // stored bottom-up (the BITMAPINFOHEADER convention): the client's fill (CODE 0x10039d80) writes destination row y
    // from source row height - 1 - y for every non-DXT format, so `data` is turned top-down here (#79). DXT blocks are
    // copied as stored (top-down).
    std::size_t off = kBody + (kind == 3 ? 4 : 0);
    if (kind != 0 && kind != 1 && kind != 3)
    {
        fail(fmt::format("unsupported kind {}", kind));
    }
    const std::size_t pixels = static_cast<std::size_t>(tex.width) * tex.height;
    tex.format               = Texture::Format::BGRA8;
    tex.data.resize(pixels * 4);

    if (bitCount == 4 || bitCount == 8)
    {
        const std::size_t entries    = bitCount == 4 ? 16 : 256;
        const std::size_t entryBytes = paletteBits / 8;
        if (entryBytes != 3 && entryBytes != 4)
        {
            fail(fmt::format("unsupported palette entry size {}", paletteBits));
        }
        const auto palette = p.subspan(off, entries * entryBytes);
        off += entries * entryBytes;
        const std::size_t pixBytes = bitCount == 8 ? pixels : (pixels + 1) / 2;
        if (off + pixBytes > p.size())
        {
            fail("pixel data truncated");
        }
        for (std::size_t i = 0; i < pixels; ++i)
        {
            const auto b   = std::to_integer<uint8_t>(p[off + (bitCount == 8 ? i : i / 2)]);
            const auto idx = bitCount == 8 ? b : ((i & 1) ? (b >> 4) : (b & 0xF));
            const auto o   = flippedRow(i, tex.width, tex.height) * 4;
            for (std::size_t c = 0; c < 4; ++c)
            {
                tex.data[o + c] = c < entryBytes ? palette[idx * entryBytes + c] : std::byte{ 0xFF };
            }
        }
        return tex;
    }
    if (bitCount == 32)
    {
        if (off + pixels * 4 > p.size())
        {
            fail("pixel data truncated");
        }
        const std::size_t row = std::size_t{ tex.width } * 4;
        for (std::size_t y = 0; y < tex.height; ++y)
        {
            std::copy_n(p.begin() + static_cast<std::ptrdiff_t>(off + y * row), row,
                        tex.data.begin() + static_cast<std::ptrdiff_t>((tex.height - 1 - y) * row));
        }
        return tex;
    }
    fail(fmt::format("unsupported bit count {} (kind {})", bitCount, kind));
}

std::vector<uint8_t> toRgba(const Texture& tex)
{
    std::vector<uint8_t> out(static_cast<std::size_t>(tex.width) * tex.height * 4);
    if (!tex.compressed())
    {
        for (std::size_t i = 0; i < out.size(); i += 4)
        {
            out[i + 0] = std::to_integer<uint8_t>(tex.data[i + 2]);
            out[i + 1] = std::to_integer<uint8_t>(tex.data[i + 1]);
            out[i + 2] = std::to_integer<uint8_t>(tex.data[i + 0]);
            out[i + 3] = std::to_integer<uint8_t>(tex.data[i + 3]);
        }
        return out;
    }

    const bool        dxt1      = tex.format == Texture::Format::DXT1;
    const std::size_t blockSize = dxt1 ? 8 : 16;
    const uint32_t    bw        = (tex.width + 3) / 4;
    const uint32_t    bh        = (tex.height + 3) / 4;
    std::span<const std::byte> data(tex.data);
    for (uint32_t by = 0; by < bh; ++by)
    {
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            const auto                             blk = data.subspan((by * bw + bx) * blockSize, blockSize);
            std::array<std::array<uint8_t, 4>, 16> texels{};
            decodeColourBlock(blk.subspan(dxt1 ? 0 : 8, 8), dxt1, texels);
            if (tex.format == Texture::Format::DXT3)
            {
                const auto alpha = load<uint64_t>(blk, 0);
                for (int t = 0; t < 16; ++t)
                {
                    texels[t][3] = static_cast<uint8_t>(((alpha >> (4 * t)) & 0xF) * 17);
                }
            }
            else if (tex.format == Texture::Format::DXT5)
            {
                const auto a0 = std::to_integer<uint8_t>(blk[0]);
                const auto a1 = std::to_integer<uint8_t>(blk[1]);
                uint64_t   ab = 0;
                for (int i = 0; i < 6; ++i)
                {
                    ab |= static_cast<uint64_t>(std::to_integer<uint8_t>(blk[2 + i])) << (8 * i);
                }
                std::array<uint8_t, 8> ap{ a0, a1 };
                for (int i = 1; i < 7; ++i)
                {
                    ap[i + 1] = a0 > a1 ? static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7)
                                        : (i < 5 ? static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5) : (i == 5 ? 0 : 255));
                }
                for (int t = 0; t < 16; ++t)
                {
                    texels[t][3] = ap[(ab >> (3 * t)) & 7];
                }
            }
            for (int t = 0; t < 16; ++t)
            {
                const uint32_t x = bx * 4 + t % 4;
                const uint32_t y = by * 4 + t / 4;
                if (x < tex.width && y < tex.height)
                {
                    std::copy(texels[t].begin(), texels[t].end(), out.begin() + static_cast<std::ptrdiff_t>((y * tex.width + x) * 4));
                }
            }
        }
    }
    return out;
}

void writePng(const std::string& path, uint32_t width, uint32_t height, std::span<const uint8_t> rgba)
{
    std::vector<uint8_t> raw;
    raw.reserve((width * 4 + 1) * height);
    for (uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), rgba.begin() + y * width * 4, rgba.begin() + (y + 1) * width * 4);
    }
    uLongf               zlen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
    {
        fail("zlib compress failed");
    }
    z.resize(zlen);

    std::ofstream f(path, std::ios::binary);
    // Written into sized buffers: GCC 14 mistakes the growing inserts for overflows (-Wstringop-overflow).
    auto          be32  = [](uint8_t* p, uint32_t x) { for (int k = 0; k < 4; ++k) p[k] = static_cast<uint8_t>(x >> (24 - 8 * k)); };
    auto          chunk = [&](const char* type, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> c(12 + body.size()); // length, type, body, CRC of type + body
        be32(c.data(), static_cast<uint32_t>(body.size()));
        std::copy(type, type + 4, c.begin() + 4);
        std::copy(body.begin(), body.end(), c.begin() + 8);
        be32(c.data() + 8 + body.size(), static_cast<uint32_t>(crc32(0, c.data() + 4, static_cast<uInt>(4 + body.size()))));
        f.write(reinterpret_cast<const char*>(c.data()), static_cast<std::streamsize>(c.size()));
    };
    const char sig[] = "\x89PNG\r\n\x1a\n";
    f.write(sig, 8);
    std::vector<uint8_t> ihdr(13);
    be32(ihdr.data(), width);
    be32(ihdr.data() + 4, height);
    ihdr[8] = 8; // 8-bit RGBA (colour type 6), no interlace
    ihdr[9] = 6;
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
}

} // namespace xi::dat
