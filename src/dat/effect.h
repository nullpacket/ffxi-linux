// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"
#include "dat/texture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Visual-effect resources (docs/effect-format.md): schedulers (type 7), generators (5), keyframes (0x19), effect
// meshes D3m (0x1F) and D3a (0x21), and textures (0x20), as one DAT's block tree. Names are 4 characters; a lookup
// starts in a block and walks up to the file root, as the client's resource tree does (§S.2).
namespace xi::dat
{

// A 4-character resource name as a little-endian u32 ('hit1' = bytes 68 69 74 31).
using FxName = uint32_t;
inline FxName fxName(std::string_view s)
{
    FxName n = 0;
    for (std::size_t i = 0; i < 4; ++i)
    {
        n |= static_cast<FxName>(static_cast<uint8_t>(i < s.size() ? s[i] : ' ')) << (8 * i);
    }
    return n;
}
std::string fxNameString(FxName n);

enum class FxType : uint8_t
{
    Camera    = 0x06, // cutscene camera keys (scheduler op 0x04; event-format §6)
    Generator = 0x05,
    Scheduler = 0x07,
    KeyFrame  = 0x19,
    D3m       = 0x1F,
    Texture   = 0x20,
    D3a       = 0x21,
    D3b       = 0x25, // morphing indexed mesh (§M.4; the seagull `kamo`, Protect's shell ...)
    Vtx       = 0x3E, // a path: u32 n, 12 pad, n x {x, y, z, w} (CODE 0x10063a80; the seagulls' flight)
    Sep       = 0x3D, // sound: an SeSep program (docs/sound-format.md)
    Afb       = 0x54, // weapon trail (§M.5)
    Weather   = 0x2F, // an environment record (dat-format §9.2; scheduler op 0x7E applies one)
};

struct FxResource
{
    FxType                     type;
    FxName                     name;
    int                        block;   // index into EffectFile::blocks
    std::span<const std::byte> payload; // into EffectFile::data (after the 16-byte chunk header)
};

// D3m / D3a geometry, unpacked: triangles in model space with per-vertex colour (0x80 = 1.0 scale kept as 0..1
// floats of the raw byte / 255), grouped by texture name (16 bytes, "category name").
struct FxMesh
{
    struct Vertex
    {
        float x, y, z, u, v;
        std::array<uint8_t, 4> rgba; // file bytes in R, G, B, A order
    };

    struct Group
    {
        std::string texName; // empty = untextured
        uint32_t    first = 0, count = 0; // vertices (a multiple of 3)
    };
    std::vector<Vertex> vertices;
    std::vector<Group>  groups;   // D3a: one group per frame
    bool                frames = false; // D3a: groups are flipbook frames, draw one
    bool                uvPixels = false; // D3a §M.3: u / v in texels (uvMode > 0, byte +7 = 0): divide by the texture size
    // D3b §M.4: the morph frames, x / y / z per vertex (vertices hold frame 0); the particle blends them by weight.
    std::vector<std::vector<float>> morph;
};
// A lens flare's elements (resource type 0x39; CODE class 0x10052f60, draw 0x10054800). The chunk is a D3a (§M.3
// header), but each group is {u8, u8, nQuad, u8}, f32 t, 3 f32 (0 in data), then 6 * nQuad vertices. t places the
// element on the line from the sun (0) through the screen centre (0.5); x / y are in 1/16 of the screen width.
struct FxFlare
{
    struct Element
    {
        float                       t = 0.0f;
        std::vector<FxMesh::Vertex> vertices; // triangle list
    };
    std::string          texName;
    std::vector<Element> elements;

    static FxFlare parse(std::span<const std::byte> d3a);
};
struct Model; // dat/zone.h
// A zone Mmb as an effect mesh: zone effects (e.g. the sea) draw unplaced Mmbs as particles (CODE 0x100494a0).
// Colours become R, G, B, A bytes (Mmb vertex colours are D3DCOLOR B, G, R, A); untextured pieces get an empty name.
FxMesh meshFromModel(const Model& m);


class EffectFile
{
public:
    // The DAT `fileId`; throws if it cannot be read.
    EffectFile(const DatIndex& index, uint32_t fileId);

    [[nodiscard]] uint32_t fileId() const
    {
        return fileId_;
    }
    // Resource `name` of `type`, searching `fromBlock` and then its ancestors (-1 = anywhere in the file, first match
    // in file order).
    // True when block `b` or one of its parents is named `name` (e.g. zone effects skip anything under `weat`).
    [[nodiscard]] bool underBlock(int b, FxName name) const;
    [[nodiscard]] const std::vector<FxResource>& resources() const
    {
        return resources_;
    }
    [[nodiscard]] const FxResource* find(FxType type, FxName name, int fromBlock = -1, FxName mask = 0xFFFFFFFF) const;
    // The block a resource lives in (for lookups made from it).
    [[nodiscard]] int blockOf(const FxResource& r) const
    {
        return r.block;
    }
    // Decoded mesh for a D3m / D3a resource (cached).
    [[nodiscard]] const FxMesh* mesh(const FxResource& r) const;
    // A texture by its 16-byte header name; falls back to matching only the last 8 bytes.
    [[nodiscard]] const Texture* texture(std::string_view name16) const;
    // A texture by its chunk name (type 0x20 chunk, e.g. L1 op 0x55's `hi10`); the first in the file.
    [[nodiscard]] const Texture* textureByChunk(FxName chunk) const;

private:
    uint32_t                                             fileId_;
    std::vector<std::byte>                               data_;
    std::vector<int>                                     blocks_; // parent of each block (-1 = root)
    std::vector<FxName>                                  blockNames_; // name of each block (0 = root)
    std::vector<FxResource>                              resources_;
    // Decoded on first use: a zone file used as an effect file holds every zone texture.
    struct TextureEntry
    {
        std::string                      name;
        std::span<const std::byte>       payload;
        mutable std::unique_ptr<Texture> decoded;
        mutable bool                     failed = false;
        FxName                           chunk  = 0;
    };
    std::vector<TextureEntry>                            textures_;
    const Texture*                                       decode(const TextureEntry& e) const;
    mutable std::unordered_map<const FxResource*, std::unique_ptr<FxMesh>> meshes_;
};

// A chocobo or mount leaving after a dismount, from its `coff` scheduler (dat-format §10.y; records run post-delay at
// 60 ticks a second, effect-format §S.1), in seconds: `ch1?` from 0, `run?` from runAt, op 0x6E walking `speed`
// yalms a second along its facing from moveAt for moveFor, op 0x29 tinting to alpha 0 from fadeAt over fadeFor.
// 39275: ch1 0..1.67, run, 12 / s 1.7..2.9, faded 2.5..2.9; mounts (102705..): no ch1, run at once, 10 / s.
struct LeavePlan
{
    bool  getDown = false; // a `ch1?` record (rider and bird get down first)
    float runAt = 0.0f, moveAt = 0.0f, moveFor = 0.0f, speed = 0.0f, fadeAt = 0.0f, fadeFor = 0.0f;
    [[nodiscard]] float end() const
    {
        return std::max(moveAt + moveFor, fadeAt + fadeFor);
    }
};
std::optional<LeavePlan> leavePlan(const EffectFile& f);

// Keyframe curve (type 0x19): (t, value) pairs from t = 0 up to the first t >= 1 (§K.1).
struct FxCurve
{
    std::vector<std::pair<float, float>> keys;
    static FxCurve parse(std::span<const std::byte> payload);
    // §K.2: linear (smooth = false) or per-segment quadratic ease-in-out, with key 0's value replaced by `base`.
    [[nodiscard]] float eval(float t, float base, bool smooth) const;
};

} // namespace xi::dat
