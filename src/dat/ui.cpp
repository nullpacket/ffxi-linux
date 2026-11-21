// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/ui.h"

#include "dat/chunk.h"
#include "xi/bytes.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace xi::dat
{

std::string uiName(std::span<const std::byte> name16)
{
    std::string out;
    bool        space = false;
    for (std::size_t i = 0; i < name16.size() && i < 16; ++i)
    {
        const auto c = static_cast<unsigned char>(name16[i]);
        if (c == 0)
        {
            break;
        }
        if (c == ' ')
        {
            space = !out.empty();
            continue;
        }
        if (space)
        {
            out.push_back(' ');
            space = false;
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

UiSpriteSheet UiSpriteSheet::parse(std::span<const std::byte> p)
{
    // name[16]; u8 nTex; char tex[nTex][16]; u16 nSprites; per sprite: u8 nQuads; per quad (0x3D bytes):
    // s16 xy[8], u16 w, h, u, v, u8 flag, u8 rgba[4][4], u8 tail[4], char texture[16].
    UiSpriteSheet s;
    s.name           = uiName(p.subspan(0, 16));
    std::size_t o    = 17 + 16 * std::size_t{ static_cast<uint8_t>(p[16]) };
    const auto  n    = load<uint16_t>(p, o);
    o += 2;
    s.sprites.resize(n);
    for (auto& sprite : s.sprites)
    {
        const auto nq = load<uint8_t>(p, o++);
        sprite.resize(nq);
        for (auto& q : sprite)
        {
            if (o + 0x3D > p.size())
            {
                throw std::runtime_error("sprite sheet " + s.name + ": truncated");
            }
            for (std::size_t i = 0; i < 8; ++i)
            {
                q.xy[i] = load<int16_t>(p, o + 2 * i);
            }
            q.w    = load<uint16_t>(p, o + 0x10);
            q.h    = load<uint16_t>(p, o + 0x12);
            q.u    = load<uint16_t>(p, o + 0x14);
            q.v    = load<uint16_t>(p, o + 0x16);
            q.flags = static_cast<uint8_t>(p[o + 0x18]);
            for (std::size_t c = 0; c < 4; ++c)
            {
                for (std::size_t k = 0; k < 4; ++k)
                {
                    q.rgba[c][k] = static_cast<uint8_t>(p[o + 0x19 + 4 * c + k]);
                }
            }
            for (std::size_t k = 0; k < 4; ++k)
            {
                q.blend[k] = static_cast<uint8_t>(p[o + 0x29 + k]);
            }
            q.texture = uiName(p.subspan(o + 0x2D, 16));
            o += 0x3D;
        }
    }
    return s;
}

namespace
{

// English names for the sheets the client swaps per language on load (table at 0x1038a8c0, column 1).
std::string sheetAlias(std::string name)
{
    if (name == "menu frames")
    {
        return "menu framesus";
    }
    if (name == "menu key1top")
    {
        return "menu key1topu";
    }
    if (name == "menu lobbyps2")
    {
        return "menu lobbywin";
    }
    return name;
}

std::size_t parseRecord(std::span<const std::byte> p, std::size_t o, bool window, UiRecord& r)
{
    const auto len = load<uint16_t>(p, o);
    if (len < 0x20 || o + len > p.size())
    {
        throw std::runtime_error("layout record length");
    }
    r.x       = load<int16_t>(p, o + 0x02);
    r.y       = load<int16_t>(p, o + 0x04);
    r.cursorX = load<int16_t>(p, o + 0x06);
    r.cursorY = load<int16_t>(p, o + 0x08);
    r.w       = load<int16_t>(p, o + 0x0A);
    r.h       = load<int16_t>(p, o + 0x0C);
    r.frameX  = load<int16_t>(p, o + 0x0E);
    r.frameY  = load<int16_t>(p, o + 0x10);
    std::size_t nSubs = 0, len1 = 0, len2 = 0;
    if (window)
    {
        r.anchor   = load<uint8_t>(p, o + 0x13);
        nSubs      = load<uint8_t>(p, o + 0x14);
        len1       = load<uint8_t>(p, o + 0x15);
        len2       = load<uint8_t>(p, o + 0x16);
        r.noAnchor = load<uint8_t>(p, o + 0x17) != 0;
    }
    else
    {
        r.id = load<uint16_t>(p, o + 0x12);
        for (std::size_t i = 0; i < 6; ++i)
        {
            r.nav[i] = load<uint8_t>(p, o + 0x15 + i);
        }
        nSubs = load<uint8_t>(p, o + 0x1B);
        len1  = load<uint8_t>(p, o + 0x1D);
        len2  = load<uint8_t>(p, o + 0x1E);
    }
    std::size_t at = o + 0x20;
    for (std::size_t i = 0; i < nSubs; ++i, at += 20)
    {
        UiSub sub;
        sub.type  = load<uint16_t>(p, at);
        sub.shape = load<uint16_t>(p, at + 2);
        sub.sheet = sheetAlias(uiName(p.subspan(at + 4, 16)));
        r.subs.push_back(std::move(sub));
    }
    auto number = [&](std::size_t n) {
        if (at + n > p.size())
        {
            throw std::runtime_error("layout record strings");
        }
        const std::string s(reinterpret_cast<const char*>(p.data() + at), n);
        at += n + 1;
        return std::atoi(s.c_str());
    };
    r.text1 = number(len1);
    r.text2 = number(len2);
    return o + len;
}

} // namespace

const UiSub* UiRecord::subFor(int state) const
{
    // Every type-0 sub applies, and so does the sub whose type matches the state; the last match wins (0x10119b30).
    const int type = state == 0 ? 0 : state == 1 ? 1 : state == 3 ? 3 : state == 5 ? 4 : state == 7 ? 2 : state == 9 ? 5 : 6;
    const UiSub* out = nullptr;
    for (const auto& s : subs)
    {
        if (s.type == 0 || s.type == type)
        {
            out = &s;
        }
    }
    return out;
}

const UiRecord* UiLayout::element(uint16_t id) const
{
    for (const auto& e : elements)
    {
        if (e.id == id)
        {
            return &e;
        }
    }
    return nullptr;
}

UiLayout UiLayout::parse(std::span<const std::byte> p)
{
    UiLayout l;
    l.name       = uiName(p.subspan(0, 16));
    const auto n = load<uint8_t>(p, 0x11);
    auto       o = parseRecord(p, 0x20, true, l.window);
    l.elements.resize(n);
    for (auto& e : l.elements)
    {
        o = parseRecord(p, o, false, e);
    }
    return l;
}

const UiSprite* UiResources::sprite(const std::string& sheet, std::size_t index) const
{
    const auto it = sheets.find(sheet);
    return it != sheets.end() && index < it->second.sprites.size() ? &it->second.sprites[index] : nullptr;
}

UiResources UiResources::load(const DatIndex& index, std::span<const uint32_t> fileIds)
{
    UiResources r;
    for (const auto id : fileIds)
    {
        const auto           file = index.read(id);
        std::vector<Chunk>   chunks;
        if (!parseChunks(file, chunks))
        {
            spdlog::warn("ui: file {} is not a clean chunk file", id);
        }
        for (const auto& c : chunks)
        {
            try
            {
                if (c.type == 0x20 || c.type == 0x1B)
                {
                    auto t = parseTexture(c.payload);
                    r.textures.insert_or_assign(uiName(std::as_bytes(std::span(t.name))), std::move(t));
                }
                else if (c.type == 0x30)
                {
                    auto l = UiLayout::parse(c.payload);
                    r.layouts.insert_or_assign(l.name, std::move(l));
                }
                else if (c.type == 0x31)
                {
                    auto s = UiSpriteSheet::parse(c.payload);
                    r.sheets.insert_or_assign(s.name, std::move(s));
                }
            }
            catch (const std::exception& e)
            {
                spdlog::warn("ui: file {} chunk {}: {}", id, c.name, e.what());
            }
        }
    }
    return r;
}

} // namespace xi::dat
