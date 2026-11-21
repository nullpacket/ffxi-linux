// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/effect.h"
#include "dat/zone.h"
#include "xi/bytes.h"

#include "dat/chunk.h"

#include <algorithm>
#include <cstring>

namespace xi::dat
{

namespace
{

template <typename T>
T rd(std::span<const std::byte> s, std::size_t at)
{
    T v{};
    if (at + sizeof(T) <= s.size())
    {
        std::memcpy(&v, s.data() + at, sizeof(T));
    }
    return v;
}

FxName nameOf(const std::string& s)
{
    return fxName(s);
}

bool isFxType(uint8_t t)
{
    return t == 0x05 || t == 0x06 || t == 0x07 || t == 0x19 || t == 0x1F || t == 0x21 || t == 0x25 || t == 0x3D || t == 0x3E || t == 0x54 || t == 0x2F;
}

// D3DCOLOR bytes (B, G, R, A) to R, G, B, A.
std::array<uint8_t, 4> bgra(std::span<const std::byte> s, std::size_t at)
{
    return { rd<uint8_t>(s, at + 2), rd<uint8_t>(s, at + 1), rd<uint8_t>(s, at), rd<uint8_t>(s, at + 3) };
}

std::string name16(std::span<const std::byte> s, std::size_t at)
{
    std::string n(16, ' ');
    for (std::size_t i = 0; i < 16; ++i)
    {
        n[i] = static_cast<char>(rd<uint8_t>(s, at + i));
    }
    return n;
}

} // namespace

std::string fxNameString(FxName n)
{
    std::string s(4, ' ');
    for (std::size_t i = 0; i < 4; ++i)
    {
        const auto c = static_cast<char>((n >> (8 * i)) & 0xFF);
        s[i]         = c >= 0x20 && c < 0x7F ? c : '?';
    }
    return s;
}

EffectFile::EffectFile(const DatIndex& index, uint32_t fileId)
: fileId_(fileId)
, data_(index.read(fileId))
{
    std::vector<Chunk> chunks;
    parseChunks(data_, chunks);
    // Type 1 opens a block, type 0 closes it (dat-format §2); resources belong to the innermost open block.
    std::vector<int> open;
    blocks_.push_back(-1); // block 0 = file root
    blockNames_.push_back(0);
    open.push_back(0);
    for (const auto& c : chunks)
    {
        if (c.type == 0x01)
        {
            blocks_.push_back(open.back());
            blockNames_.push_back(nameOf(c.name));
            open.push_back(static_cast<int>(blocks_.size()) - 1);
            continue;
        }
        if (c.type == 0x00)
        {
            if (open.size() > 1)
            {
                open.pop_back();
            }
            continue;
        }
        if (c.type == 0x20)
        {
            if (c.payload.size() > 17)
            {
                textures_.push_back({ loadString(c.payload, 0x01, 16), c.payload, nullptr, false, nameOf(c.name) });
            }
            continue;
        }
        if (isFxType(c.type))
        {
            resources_.push_back({ static_cast<FxType>(c.type), nameOf(c.name), open.back(), c.payload });
        }
    }
}

FxMesh meshFromModel(const Model& m)
{
    FxMesh out;
    for (const auto& piece : m.pieces)
    {
        FxMesh::Group g;
        const bool    blank = piece.texture.find_first_not_of(' ') == std::string::npos;
        g.texName           = blank ? std::string() : piece.texture;
        g.first             = static_cast<uint32_t>(out.vertices.size());
        for (const auto i : piece.triangles)
        {
            if (i >= piece.vertices.size())
            {
                continue;
            }
            const auto& v = piece.vertices[i];
            const auto  c = v.color;
            out.vertices.push_back({ v.pos[0], v.pos[1], v.pos[2], v.uv[0], v.uv[1],
                                     { static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 24) } });
        }
        g.count = static_cast<uint32_t>(out.vertices.size()) - g.first;
        g.count -= g.count % 3;
        if (g.count > 0)
        {
            out.groups.push_back(std::move(g));
        }
    }
    return out;
}

bool EffectFile::underBlock(int b, FxName name) const
{
    for (; b > 0; b = blocks_[static_cast<std::size_t>(b)])
    {
        if (blockNames_[static_cast<std::size_t>(b)] == name)
        {
            return true;
        }
    }
    return false;
}

const FxResource* EffectFile::find(FxType type, FxName name, int fromBlock, FxName mask) const
{
    const auto match = [&](const FxResource& r) { return r.type == type && (r.name & mask) == (name & mask); };
    // CODE 0x100721f0: each enclosing block is searched depth-first through all its sub-blocks (marking the one already
    // searched), nearest block first. A Jeuno street lamp's `evst` (in `_l00/ligh`) starts `lton` from `_l00/lt08`.
    const auto within = [&](int block, int ancestor) {
        for (int b = block; b >= 0; b = blocks_[static_cast<std::size_t>(b)])
        {
            if (b == ancestor)
            {
                return true;
            }
        }
        return false;
    };
    for (int b = fromBlock; b >= 0; b = blocks_[static_cast<std::size_t>(b)])
    {
        for (const auto& r : resources_)
        {
            if (match(r) && within(r.block, b))
            {
                return &r; // file order is the game's depth-first order
            }
        }
    }
    for (const auto& r : resources_)
    {
        if (match(r))
        {
            return &r;
        }
    }
    return nullptr;
}

const Texture* EffectFile::decode(const TextureEntry& e) const
{
    if (!e.decoded && !e.failed)
    {
        try
        {
            e.decoded = std::make_unique<Texture>(parseTexture(e.payload));
        }
        catch (const std::exception&)
        {
            e.failed = true;
        }
    }
    return e.decoded.get();
}

const Texture* EffectFile::texture(std::string_view n) const
{
    for (const auto& e : textures_)
    {
        if (e.name.size() >= 16 && n.size() >= 16 && e.name.compare(0, 16, n.substr(0, 16)) == 0)
        {
            return decode(e);
        }
    }
    for (const auto& e : textures_)
    {
        if (e.name.size() >= 16 && n.size() >= 16 && e.name.compare(8, 8, n.substr(8, 8)) == 0)
        {
            return decode(e);
        }
    }
    return nullptr;
}

const Texture* EffectFile::textureByChunk(FxName chunk) const
{
    for (const auto& e : textures_)
    {
        if (e.chunk == chunk)
        {
            return decode(e);
        }
    }
    return nullptr;
}

const FxMesh* EffectFile::mesh(const FxResource& r) const
{
    if (auto it = meshes_.find(&r); it != meshes_.end())
    {
        return it->second.get();
    }
    auto       m = std::make_unique<FxMesh>();
    const auto p = r.payload;
    if (r.type == FxType::D3m)
    {
        // §M.2: format, flags, nTex, nPlain, nTri, triCount[k], padding, texName[nTex][16], 36-byte vertices.
        const int      nTex   = rd<uint8_t>(p, 4);
        const int      nPlain = rd<uint8_t>(p, 5);
        const uint32_t nTri   = rd<uint16_t>(p, 6);
        const int      k      = nTex + nPlain;
        const std::size_t names = 8 + 2 * static_cast<std::size_t>(k & ~3) + ((k & 3) ? 6 : 0);
        const std::size_t verts = names + 16 * static_cast<std::size_t>(nTex);
        if (verts + 36ull * 3 * nTri <= p.size())
        {
            uint32_t first = 0;
            for (int g = 0; g < k; ++g)
            {
                const uint32_t tris = rd<uint16_t>(p, 8 + 2 * static_cast<std::size_t>(g));
                m->groups.push_back({ g < nTex ? name16(p, names + 16 * static_cast<std::size_t>(g)) : std::string(), first * 3, tris * 3 });
                first += tris;
            }
            for (uint32_t i = 0; i < 3 * nTri; ++i)
            {
                const std::size_t at = verts + 36 * static_cast<std::size_t>(i);
                m->vertices.push_back({ rd<float>(p, at), rd<float>(p, at + 4), rd<float>(p, at + 8), rd<float>(p, at + 28), rd<float>(p, at + 32), bgra(p, at + 24) });
            }
        }
    }
    else if (r.type == FxType::D3b)
    {
        // §M.4 (CODE builder 0x1003f7d0): +0x02 bits 0..6 frames; +0x04 nPos, +0x06 nNrm; +0x08 index stream offset
        // (+ s8 @+0x03 << 16); +0x0A nTri; +0x0C colour per corner; +0x0E UV per corner (+0x10000 if colOff >= uvOff);
        // +0x10 texName; frames from +0x20 (f32 positions, then packed normals). Per corner: position[posIdx], UV and
        // colour (doubled, clamped) from its own arrays. DATA: all 57 chunks (2800-3099, Selbina `kamo`) check.
        const int         frames = rd<uint8_t>(p, 2) & 0x7F;
        const int         nPos   = rd<int16_t>(p, 4), nNrm = rd<int16_t>(p, 6);
        const std::size_t idx    = rd<uint16_t>(p, 8) + (static_cast<std::size_t>(rd<int8_t>(p, 3)) << 16);
        const int         nTri   = rd<uint16_t>(p, 0x0A);
        const std::size_t col    = rd<uint16_t>(p, 0x0C);
        std::size_t       uv     = rd<uint16_t>(p, 0x0E);
        if (col >= uv)
        {
            uv += 0x10000;
        }
        const std::size_t stride = 12u * static_cast<std::size_t>(std::max(nPos, 0)) + 4u * static_cast<std::size_t>(std::max(nNrm, 0));
        const std::size_t corners = 3u * static_cast<std::size_t>(nTri);
        if (frames > 0 && nPos > 0 && idx + 2 * corners <= p.size() && uv + 8 * corners <= p.size() && col + 4 * corners <= p.size() &&
            0x20 + frames * stride <= p.size())
        {
            const std::string tex = name16(p, 0x10);
            for (int f = 0; f < frames; ++f)
            {
                std::vector<float> pos;
                for (std::size_t c = 0; c < corners; ++c)
                {
                    const std::size_t k = std::min<std::size_t>(rd<uint16_t>(p, idx + 2 * c), static_cast<std::size_t>(nPos - 1));
                    const std::size_t o = 0x20 + f * stride + 12 * k;
                    pos.insert(pos.end(), { rd<float>(p, o), rd<float>(p, o + 4), rd<float>(p, o + 8) });
                }
                m->morph.push_back(std::move(pos));
            }
            for (std::size_t c = 0; c < corners; ++c)
            {
                auto rgba = bgra(p, col + 4 * c);
                for (auto& b : rgba)
                {
                    b = static_cast<uint8_t>(std::min(2 * static_cast<int>(b), 255)); // CODE 0x1003f5a3: doubled on the CPU
                }
                const auto& f0 = m->morph.front();
                m->vertices.push_back({ f0[3 * c], f0[3 * c + 1], f0[3 * c + 2], rd<float>(p, uv + 8 * c), rd<float>(p, uv + 8 * c + 4), rgba });
            }
            m->groups.push_back({ tex.find_first_not_of(' ') == std::string::npos ? std::string() : tex, 0, static_cast<uint32_t>(corners) });
            if (frames < 2)
            {
                m->morph.clear();
            }
        }
    }
    else if (r.type == FxType::D3a)
    {
        // §M.3: uvMode, nGroups, flags, texName[16] @8, groups from 0x18: {u8, u8, nQuad, u8} + 6 * nQuad 24-byte vertices.
        const int         nGroups = rd<int16_t>(p, 2);
        const std::string tex     = name16(p, 8);
        std::size_t       at      = 0x18;
        m->frames                 = true;
        m->uvPixels               = rd<int16_t>(p, 0) > 0 && rd<uint8_t>(p, 7) == 0;
        for (int g = 0; g < nGroups && at + 4 <= p.size(); ++g)
        {
            const uint32_t nQuad = rd<uint8_t>(p, at + 2);
            at += 4;
            FxMesh::Group grp{ tex, static_cast<uint32_t>(m->vertices.size()), 6 * nQuad };
            for (uint32_t i = 0; i < 6 * nQuad && at + 24 <= p.size(); ++i, at += 24)
            {
                m->vertices.push_back({ rd<float>(p, at), rd<float>(p, at + 4), rd<float>(p, at + 8), rd<float>(p, at + 16), rd<float>(p, at + 20), bgra(p, at + 12) });
            }
            m->groups.push_back(std::move(grp));
        }
    }
    const auto* out = m.get();
    meshes_.emplace(&r, std::move(m));
    return out;
}

FxFlare FxFlare::parse(std::span<const std::byte> p)
{
    FxFlare f;
    if (p.size() < 0x18)
    {
        return f;
    }
    f.texName         = name16(p, 8);
    const int nGroups = rd<int16_t>(p, 2);
    std::size_t at    = 0x18;
    for (int g = 0; g < nGroups && at + 20 <= p.size(); ++g)
    {
        const uint32_t nQuad = rd<uint8_t>(p, at + 2);
        Element        e;
        e.t = rd<float>(p, at + 4);
        at += 20;
        for (uint32_t i = 0; i < 6 * nQuad && at + 24 <= p.size(); ++i, at += 24)
        {
            e.vertices.push_back({ rd<float>(p, at), rd<float>(p, at + 4), rd<float>(p, at + 8), rd<float>(p, at + 16), rd<float>(p, at + 20), bgra(p, at + 12) });
        }
        f.elements.push_back(std::move(e));
    }
    return f;
}

FxCurve FxCurve::parse(std::span<const std::byte> payload)
{
    FxCurve c;
    for (std::size_t at = 0; at + 8 <= payload.size(); at += 8)
    {
        const float t = rd<float>(payload, at), v = rd<float>(payload, at + 4);
        c.keys.emplace_back(t, v);
        if (t >= 1.0f)
        {
            break;
        }
    }
    return c;
}

float FxCurve::eval(float t, float base, bool smooth) const
{
    if (keys.empty())
    {
        return base;
    }
    if (!smooth)
    {
        t = std::clamp(t, 0.0f, 1.0f);
    }
    auto value = [&](std::size_t i) { return i == 0 ? base : keys[i].second; };
    std::size_t b = 1;
    while (b < keys.size() && keys[b].first < t)
    {
        ++b;
    }
    if (b >= keys.size())
    {
        return value(keys.size() - 1);
    }
    const auto a = b - 1;
    const float span = keys[b].first - keys[a].first;
    if (span <= 0.0f)
    {
        return value(b);
    }
    const float u = (t - keys[a].first) / span;
    if (!smooth)
    {
        return value(a) + (value(b) - value(a)) * u;
    }
    if (u < 0.5f)
    {
        const float w = 2 * u * u;
        return value(a) * (1 - w) + value(b) * w;
    }
    const float w = 2 * (1 - u) * (1 - u);
    return value(a) * w + value(b) * (1 - w);
}

std::optional<LeavePlan> leavePlan(const EffectFile& f)
{
    const auto* r = f.find(FxType::Scheduler, fxName("coff"));
    if (!r)
    {
        return std::nullopt;
    }
    const auto p     = r->payload;
    const auto start = p.size() >= 0x18 ? load<uint32_t>(p, 0x14) : 0u;
    LeavePlan  plan;
    int        tick = 0;
    for (std::size_t at = start >= 0x10 ? start - 0x10 : 0x40; at + 8 <= p.size();)
    {
        const auto w   = load<uint32_t>(p, at);
        const auto len = (w >> 8) & 0x1F;
        if ((w & 0xFF) == 0 || len == 0)
        {
            break;
        }
        const float t   = static_cast<float>(tick) / 60.0f;
        const float dur = static_cast<float>(load<int16_t>(p, at + 6)) / 60.0f;
        switch (w & 0xFF)
        {
            case 0x05:
                if (at + 12 <= p.size() && fxNameString(load<uint32_t>(p, at + 8)).starts_with("ch1"))
                {
                    plan.getDown = true;
                }
                else if (at + 12 <= p.size() && fxNameString(load<uint32_t>(p, at + 8)).starts_with("run"))
                {
                    plan.runAt = t;
                }
                break;
            case 0x6E:
                if (at + 12 <= p.size())
                {
                    plan.moveAt  = t;
                    plan.moveFor = dur;
                    plan.speed   = load<float>(p, at + 8);
                }
                break;
            case 0x29:
                plan.fadeAt  = t;
                plan.fadeFor = dur;
                break;
            default: break;
        }
        tick += load<int16_t>(p, at + 4); // post-delay: the gap before the next record
        at += 4 * len;
    }
    return plan;
}

} // namespace xi::dat
