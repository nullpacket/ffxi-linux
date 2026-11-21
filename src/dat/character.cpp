// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/character.h"

#include "dat/chunk.h"
#include "xi/bytes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace xi::dat
{

namespace
{

// FFXiMain 0x1035f420: base (skeleton + motions) file id per race 0..8 (§10).
constexpr uint32_t kRaceBase[9] = { 7072, 7072, 10248, 13424, 16600, 19776, 19776, 23176, 26352 };

// The same table goes on to race 36 (CODE 0x100d2ede: races 0..8 and 29..36 are valid; others become 32). 29..31
// are the children: 29 Mithra child (`mf_s`), 30 Hume / Elvaan girl (`hf_s`), 31 boy (`hm_s`), on a `kids` skeleton
// with one-piece idl0 / wlk0 / run0 motions and no weapons (DATA; Elvaan children use face / gear indices 20 and up,
// INFERRED from file names). Other races: Hume male.
uint32_t raceBase(uint8_t race)
{
    switch (race)
    {
        case 29: return 30696;
        case 30: return 29912;
        case 31: return 30304;
        // 32..36: chocobos, yellow / black / blue / red / green (chocobo_re.md; `choc` skeleton, chi0 idle).
        case 32: return 55295;
        case 33: return 55329;
        case 34: return 55363;
        case 35: return 55397;
        case 36: return 55431;
        default: return kRaceBase[race < 9 ? race : 1];
    }
}

struct Range
{
    uint32_t start, count;
};
using SlotRanges = std::array<std::vector<Range>, 6>; // face, head, body, hands, legs, feet

// FFXiMain 0x1035f388: per race, (start file, count) ranges for face and the five armour slots (§10).
const SlotRanges& slotRanges(uint8_t race)
{
    static const SlotRanges r[9] = {
        {}, // 0 unused (treated as Hume male)
        SlotRanges{ { { { 7080, 32 } },
                      { { 7112, 256 }, { 63323, 48 }, { 63371, 16 }, { 71247, 256 }, { 98787, 32 }, { 102961, 64 } },
                      { { 7368, 256 }, { 63387, 48 }, { 63435, 16 }, { 71503, 256 }, { 98819, 32 }, { 103025, 64 } },
                      { { 7624, 256 }, { 63451, 48 }, { 63499, 16 }, { 71759, 256 }, { 98851, 32 }, { 103089, 64 } },
                      { { 7880, 256 }, { 63515, 48 }, { 63563, 16 }, { 72015, 256 }, { 98883, 32 }, { 103153, 64 } },
                      { { 8136, 256 }, { 63579, 48 }, { 63627, 16 }, { 72271, 256 }, { 98915, 32 }, { 103217, 64 } } } },
        SlotRanges{ { { { 10256, 32 } },
                      { { 10288, 256 }, { 63771, 48 }, { 63819, 16 }, { 72783, 256 }, { 98947, 32 }, { 103281, 64 } },
                      { { 10544, 256 }, { 63835, 48 }, { 63883, 16 }, { 73039, 256 }, { 98979, 32 }, { 103345, 64 } },
                      { { 10800, 256 }, { 63899, 48 }, { 63947, 16 }, { 73295, 256 }, { 99011, 32 }, { 103409, 64 } },
                      { { 11056, 256 }, { 63963, 48 }, { 64011, 16 }, { 73551, 256 }, { 99043, 32 }, { 103473, 64 } },
                      { { 11312, 256 }, { 64027, 48 }, { 64075, 16 }, { 73807, 256 }, { 99075, 32 }, { 103537, 64 } } } },
        SlotRanges{ { { { 13432, 32 } },
                      { { 13464, 256 }, { 64219, 48 }, { 64267, 16 }, { 74319, 256 }, { 99107, 32 }, { 103601, 64 } },
                      { { 13720, 256 }, { 64283, 48 }, { 64331, 16 }, { 74575, 256 }, { 99139, 32 }, { 103665, 64 } },
                      { { 13976, 256 }, { 64347, 48 }, { 64395, 16 }, { 74831, 256 }, { 99171, 32 }, { 103729, 64 } },
                      { { 14232, 256 }, { 64411, 48 }, { 64459, 16 }, { 75087, 256 }, { 99203, 32 }, { 103793, 64 } },
                      { { 14488, 256 }, { 64475, 48 }, { 64523, 16 }, { 75343, 256 }, { 99235, 32 }, { 103857, 64 } } } },
        SlotRanges{ { { { 16608, 32 } },
                      { { 16640, 256 }, { 64667, 48 }, { 64715, 16 }, { 75855, 256 }, { 99267, 32 }, { 103921, 64 } },
                      { { 16896, 256 }, { 64731, 48 }, { 64779, 16 }, { 76111, 256 }, { 99299, 32 }, { 103985, 64 } },
                      { { 17152, 256 }, { 64795, 48 }, { 64843, 16 }, { 76367, 256 }, { 99331, 32 }, { 104049, 64 } },
                      { { 17408, 256 }, { 64859, 48 }, { 64907, 16 }, { 76623, 256 }, { 99363, 32 }, { 104113, 64 } },
                      { { 17664, 256 }, { 64923, 48 }, { 64971, 16 }, { 76879, 256 }, { 99395, 32 }, { 104177, 64 } } } },
        SlotRanges{ { { { 19784, 32 } },
                      { { 19816, 256 }, { 65115, 48 }, { 65163, 16 }, { 77391, 256 }, { 99427, 32 }, { 104241, 64 } },
                      { { 20072, 256 }, { 65179, 48 }, { 65227, 16 }, { 77647, 256 }, { 99459, 32 }, { 104305, 64 } },
                      { { 20328, 256 }, { 65243, 48 }, { 65291, 16 }, { 77903, 256 }, { 99491, 32 }, { 104369, 64 } },
                      { { 20584, 256 }, { 65307, 48 }, { 65355, 16 }, { 78159, 256 }, { 99523, 32 }, { 104433, 64 } },
                      { { 20840, 256 }, { 65371, 48 }, { 65419, 16 }, { 78415, 256 }, { 99555, 32 }, { 104497, 64 } } } },
        SlotRanges{ { { { 22952, 32 } },
                      { { 19816, 256 }, { 65115, 48 }, { 65171, 16 }, { 77391, 256 }, { 99443, 32 }, { 104241, 64 } },
                      { { 20072, 256 }, { 65179, 48 }, { 65235, 16 }, { 77647, 256 }, { 99475, 32 }, { 104305, 64 } },
                      { { 20328, 256 }, { 65243, 48 }, { 65299, 16 }, { 77903, 256 }, { 99507, 32 }, { 104369, 64 } },
                      { { 20584, 256 }, { 65307, 48 }, { 65363, 16 }, { 78159, 256 }, { 99539, 32 }, { 104433, 64 } },
                      { { 20840, 256 }, { 65371, 48 }, { 65427, 16 }, { 78415, 256 }, { 99571, 32 }, { 104497, 64 } } } },
        SlotRanges{ { { { 23184, 32 } },
                      { { 23216, 256 }, { 65563, 48 }, { 65611, 16 }, { 78927, 256 }, { 99587, 32 }, { 104561, 64 } },
                      { { 23472, 256 }, { 65627, 48 }, { 65675, 16 }, { 79183, 256 }, { 99619, 32 }, { 104625, 64 } },
                      { { 23728, 256 }, { 65691, 48 }, { 65739, 16 }, { 79439, 256 }, { 99651, 32 }, { 104689, 64 } },
                      { { 23984, 256 }, { 65755, 48 }, { 65803, 16 }, { 79695, 256 }, { 99683, 32 }, { 104753, 64 } },
                      { { 24240, 256 }, { 65819, 48 }, { 65867, 16 }, { 79951, 256 }, { 99715, 32 }, { 104817, 64 } } } },
        SlotRanges{ { { { 26360, 32 } },
                      { { 26392, 256 }, { 66011, 48 }, { 66059, 16 }, { 80463, 256 }, { 99747, 32 }, { 104881, 64 } },
                      { { 26648, 256 }, { 66075, 48 }, { 66123, 16 }, { 80719, 256 }, { 99779, 32 }, { 104945, 64 } },
                      { { 26904, 256 }, { 66139, 48 }, { 66187, 16 }, { 80975, 256 }, { 99811, 32 }, { 105009, 64 } },
                      { { 27160, 256 }, { 66203, 48 }, { 66251, 16 }, { 81231, 256 }, { 99843, 32 }, { 105073, 64 } },
                      { { 27416, 256 }, { 66267, 48 }, { 66315, 16 }, { 81487, 256 }, { 99875, 32 }, { 105137, 64 } } } },
    };
    // Children (FFXiMain 0x1035f388 rows for races 29..31): one 64-file range per slot.
    static const SlotRanges kids[3] = {
        SlotRanges{ { { { 30376, 64 } }, { { 30568, 64 } }, { { 30312, 64 } }, { { 30504, 64 } }, { { 30632, 64 } }, { { 30440, 64 } } } },
        SlotRanges{ { { { 29592, 64 } }, { { 29784, 64 } }, { { 29528, 64 } }, { { 29720, 64 } }, { { 29848, 64 } }, { { 29656, 64 } } } },
        SlotRanges{ { { { 29984, 64 } }, { { 30176, 64 } }, { { 29920, 64 } }, { { 30112, 64 } }, { { 30240, 64 } }, { { 30048, 64 } } } },
    };
    if (race >= 29 && race <= 31)
    {
        return kids[race - 29];
    }
    // Chocobos (rows 32..36): no face; head 8, body 8 (index 1 has no saddle), hands 4 (the tail), legs 4 (index 0 is
    // an empty file, 1 the Noble chocobo's armour), feet 8 (chocobo_re.md §1).
    if (race >= 32 && race <= 36)
    {
        static const SlotRanges choc[5] = {
            SlotRanges{ { {}, { { 55313, 8 } }, { { 55297, 8 } }, { { 55321, 4 } }, { { 55325, 4 } }, { { 55305, 8 } } } },
            SlotRanges{ { {}, { { 55347, 8 } }, { { 55331, 8 } }, { { 55355, 4 } }, { { 55359, 4 } }, { { 55339, 8 } } } },
            SlotRanges{ { {}, { { 55381, 8 } }, { { 55365, 8 } }, { { 55389, 4 } }, { { 55393, 4 } }, { { 55373, 8 } } } },
            SlotRanges{ { {}, { { 55415, 8 } }, { { 55399, 8 } }, { { 55423, 4 } }, { { 55427, 4 } }, { { 55407, 8 } } } },
            SlotRanges{ { {}, { { 55449, 8 } }, { { 55433, 8 } }, { { 55457, 4 } }, { { 55461, 4 } }, { { 55441, 8 } } } },
        };
        return choc[race - 32];
    }
    return r[race < 9 && race > 0 ? race : 1];
}

using WeaponRanges = std::array<std::vector<Range>, 3>; // main, sub, ranged

// FFXiMain 0x1035f388, slots 6..8 (§10).
const WeaponRanges& weaponRanges(uint8_t race)
{
    static const WeaponRanges w[9] = {
        {},
        /* race 1 */ WeaponRanges{ { { { 8392, 512 }, { 63643, 128 }, { 72527, 256 }, { 107301, 300 }, { 0, 64 } }, { { 41199, 512 }, { 66459, 128 }, { 81999, 256 }, { 105201, 300 }, { 0, 64 } }, { { 9416, 256 } } } },
        /* race 2 */ WeaponRanges{ { { { 11568, 512 }, { 64091, 128 }, { 74063, 256 }, { 107601, 300 }, { 0, 64 } }, { { 42479, 512 }, { 66587, 128 }, { 82255, 256 }, { 105501, 300 }, { 0, 64 } }, { { 12592, 256 } } } },
        /* race 3 */ WeaponRanges{ { { { 14744, 512 }, { 64539, 128 }, { 75599, 256 }, { 107901, 300 }, { 0, 64 } }, { { 43759, 512 }, { 66715, 128 }, { 82511, 256 }, { 105801, 300 }, { 0, 64 } }, { { 15768, 256 } } } },
        /* race 4 */ WeaponRanges{ { { { 17920, 512 }, { 64987, 128 }, { 77135, 256 }, { 108201, 300 }, { 0, 64 } }, { { 45039, 512 }, { 66843, 128 }, { 82767, 256 }, { 106101, 300 }, { 0, 64 } }, { { 18944, 256 } } } },
        /* race 5 */ WeaponRanges{ { { { 21096, 512 }, { 65435, 128 }, { 78671, 256 }, { 108501, 300 }, { 0, 64 } }, { { 46319, 512 }, { 66971, 128 }, { 83023, 256 }, { 106401, 300 }, { 0, 64 } }, { { 22120, 256 } } } },
        /* race 6 */ WeaponRanges{ { { { 21096, 512 }, { 65435, 128 }, { 78671, 256 }, { 108501, 300 }, { 0, 64 } }, { { 46319, 512 }, { 66971, 128 }, { 83023, 256 }, { 106401, 300 }, { 0, 64 } }, { { 22120, 256 } } } },
        /* race 7 */ WeaponRanges{ { { { 24496, 512 }, { 65883, 128 }, { 80207, 256 }, { 108801, 300 }, { 0, 64 } }, { { 47599, 512 }, { 67099, 128 }, { 83279, 256 }, { 106701, 300 }, { 0, 64 } }, { { 25520, 256 } } } },
        /* race 8 */ WeaponRanges{ { { { 27672, 512 }, { 66331, 128 }, { 81743, 256 }, { 109101, 300 }, { 0, 64 } }, { { 48879, 512 }, { 67227, 128 }, { 83535, 256 }, { 107001, 300 }, { 0, 64 } }, { { 28696, 256 } } } },
    };
    static const WeaponRanges none{};
    if (race >= 29)
    {
        return none; // children carry no weapons (CODE 0x100d3b33; their rows 6..8 are zero)
    }
    return w[race < 9 && race > 0 ? race : 1];
}

std::optional<uint32_t> slotFileId(uint8_t race, std::size_t slot, uint16_t look)
{
    const uint32_t idx    = look & 0xFFF;
    uint32_t       acc    = 0;
    const auto&    ranges = slot < 6 ? slotRanges(race)[slot] : weaponRanges(race)[slot - 6];
    for (const auto& r : ranges)
    {
        if (idx < acc + r.count)
        {
            return r.start ? std::optional<uint32_t>(r.start - acc + idx) : std::nullopt;
        }
        acc += r.count;
    }
    // Past every range: the game logs it and uses index 0 (CODE 0x100d3806.., log 0x1035fb1c).
    return !ranges.empty() && ranges.front().start ? std::optional<uint32_t>(ranges.front().start) : std::nullopt;
}

uint32_t npcFileId(uint16_t model)
{
    if (model >= 3500) return 101739u + model - 3500u;
    if (model >= 3000) return 99907u + model - 3000u;
    if (model >= 1500) return 51795u + model - 1500u;
    return 1300u + model;
}

RowMatrix multiply(const RowMatrix& a, const RowMatrix& b)
{
    RowMatrix r{};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            float s = 0;
            for (int k = 0; k < 4; ++k)
            {
                s += a[static_cast<std::size_t>(i * 4 + k)] * b[static_cast<std::size_t>(k * 4 + j)];
            }
            r[static_cast<std::size_t>(i * 4 + j)] = s;
        }
    }
    return r;
}

// D3DXMatrixRotationQuaternion (row-vector convention) + translation row.
RowMatrix boneLocal(const std::array<float, 4>& q, const std::array<float, 3>& t)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),     0,
             2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),     0,
             2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y), 0,
             t[0],                    t[1],                    t[2],                    1 };
}

// Mirror sign vectors selected by bone-ref bits 14..15 (FFXiMain 0x103515f8).
constexpr float kMirror[4][3] = { { 1, 1, 1 }, { -1, 1, 1 }, { 1, -1, 1 }, { 1, 1, -1 } };

// Appends one Os2 (type 42) mesh, and its mirrored copy when flagged, to the asset.
void appendOs2(std::span<const std::byte> p, std::size_t boneCount, CharacterAsset& out)
{
    // Hide groups (dat-format §10.3; CODE 0x1002e5ec): triangles after a 0x8010 material whose group another worn mesh
    // hides are not drawn (hair under headgear, glove cuffs under sleeves, boot tops under trousers ...).
    unsigned group = 0;
    float    lightScale = 1.0f, strength = 0.0f;
    bool     env = false, spec = false;
    const auto version = load<uint8_t>(p, 0);
    const auto flags2  = load<uint8_t>(p, 2);
    const auto flags4  = load<uint8_t>(p, 4);
    // Class 1 (payload[2] & 0x7F == 1; os2 class 1 notes, dat-format §10.3): hair / cape / skirt soft bodies on unique
    // NPCs, simulated by the game (anchored vertices follow bones, the rest are springs). Drawn here rigidly in the
    // rest pose the game itself uses on a reset: 12-byte / 32-byte vertices without normals, bone = ref & 0x7F.
    const bool cloth = (flags2 & 0x7F) == 1;
    if (version > 1 || ((flags2 & 0x7F) != 0 && !cloth))
    {
        return;
    }
    std::array<std::pair<std::size_t, std::size_t>, 6> sec{};
    for (std::size_t i = 0; i < 6; ++i)
    {
        sec[i] = { static_cast<std::size_t>(load<uint32_t>(p, 6 + 6 * i)) * 2, static_cast<std::size_t>(load<uint16_t>(p, 10 + 6 * i)) * 2 };
    }
    std::vector<uint16_t> boneTable(sec[1].second / 2);
    for (std::size_t i = 0; i < boneTable.size(); ++i)
    {
        boneTable[i] = load<uint16_t>(p, sec[1].first + 2 * i);
    }
    const auto n1 = load<uint16_t>(p, sec[2].first);
    const auto n2 = load<uint16_t>(p, sec[2].first + 2);
    const std::size_t nv = static_cast<std::size_t>(n1) + n2;
    std::vector<uint16_t> refs(2 * nv);
    for (std::size_t i = 0; i < refs.size(); ++i)
    {
        refs[i] = load<uint16_t>(p, sec[3].first + 2 * i);
    }
    struct V
    {
        float p0[3], p1[3], n0[3], n1[3], w1;
        bool  two;
    };
    ClothMesh cm;
    std::vector<V> verts(nv);
    std::size_t    vo = sec[4].first;
    if (cloth)
    {
        for (std::size_t i = 0; i < n1; ++i, vo += 12)
        {
            auto& v = verts[i];
            for (int k = 0; k < 3; ++k)
            {
                v.p0[k] = load<float>(p, vo + 4 * static_cast<std::size_t>(k));
                v.n0[k] = 0.0f;
            }
            v.two = false;
            v.w1  = 0.0f;
        }
        for (std::size_t i = 0; i < n2; ++i, vo += 32)
        {
            auto& v = verts[n1 + i];
            for (int k = 0; k < 3; ++k)
            {
                v.p0[k] = load<float>(p, vo + 8 * static_cast<std::size_t>(k));
                v.p1[k] = load<float>(p, vo + 8 * static_cast<std::size_t>(k) + 4);
                v.n0[k] = v.n1[k] = 0.0f;
            }
            v.w1  = load<float>(p, vo + 28);
            v.two = true;
        }
        // The soft body (CODE 0x100231c0 / 0x10023630; payload offsets = object field - 0x30): the first A1 one-bone and
        // A2 two-bone vertices are anchored; springs {u16 a, u16 b, f32 rest} with index bit 15 = pinned end, in two
        // sets with their own stiffness; velocity damping, gravity (+g x 0.1 per tick, y down) and inertia.
        cm.first = out.clothParticles;
        const auto a1 = load<uint16_t>(p, 0x2a), a2 = load<uint16_t>(p, 0x2c);
        cm.particles.resize(nv);
        for (std::size_t i = 0; i < nv; ++i)
        {
            auto&       q  = cm.particles[i];
            const auto& v  = verts[i];
            q.bone0        = static_cast<uint8_t>(std::min<std::size_t>(refs[2 * i] & 0x7F, boneCount ? boneCount - 1 : 0));
            q.bone1        = v.two ? static_cast<uint8_t>(std::min<std::size_t>(refs[2 * i + 1] & 0x7F, boneCount ? boneCount - 1 : 0)) : q.bone0;
            q.weight1      = v.w1;
            q.anchor       = i < n1 ? i < a1 : i - n1 < a2;
            for (int k = 0; k < 3; ++k)
            {
                q.p0[k] = v.p0[k];
                q.p1[k] = v.two ? v.p1[k] : 0.0f;
            }
        }
        const std::size_t so = static_cast<std::size_t>(load<uint32_t>(p, 0x2e)) * 2;
        const std::size_t s1 = load<uint16_t>(p, 0x34) >> 2, s2 = load<uint16_t>(p, 0x36) >> 2;
        for (std::size_t i = 0; i < s1 + s2 && so + 8 * i + 8 <= p.size(); ++i)
        {
            const auto a = load<uint16_t>(p, so + 8 * i), b = load<uint16_t>(p, so + 8 * i + 2);
            ClothMesh::Spring sp{ static_cast<uint16_t>(a & 0x7FFF), static_cast<uint16_t>(b & 0x7FFF), load<float>(p, so + 8 * i + 4),
                                  (a & 0x8000) != 0, (b & 0x8000) != 0, static_cast<uint8_t>(i < s1 ? 0 : 1) };
            if (sp.a < nv && sp.b < nv && std::isfinite(sp.rest))
            {
                cm.springs.push_back(sp);
            }
        }
        cm.stiffness[0] = load<float>(p, 0x38);
        cm.stiffness[1] = load<float>(p, 0x3c);
        cm.damping      = load<float>(p, 0x40);
        cm.gravity      = load<float>(p, 0x44);
        cm.inertia      = load<float>(p, 0x48);
        const std::size_t ids = static_cast<std::size_t>(load<uint32_t>(p, 0x4c)) * 2;
        for (std::size_t i = 0; i < load<uint16_t>(p, 0x50) && ids + 2 * i + 2 <= p.size(); ++i)
        {
            cm.spheres.push_back(load<uint16_t>(p, ids + 2 * i));
        }
        // Normals from the triangles (CODE 0x10024830 recomputes them each frame): area-weighted face normals of the
        // positions in their first bone's space (two-bone: the first bone's weighted part, unweighted).
        auto pos = [&](uint16_t i) {
            const auto& v = verts[i];
            const float w = v.two ? std::max(1.0f - v.w1, 1e-3f) : 1.0f;
            return std::array<float, 3>{ v.p0[0] / w, v.p0[1] / w, v.p0[2] / w };
        };
        auto face = [&](uint16_t a, uint16_t b, uint16_t c) {
            if (a >= nv || b >= nv || c >= nv)
            {
                return;
            }
            cm.triangles.push_back({ a, b, c });
            const auto pa = pos(a), pb = pos(b), pc = pos(c);
            const float e1[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] }, e2[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
            // Clockwise fronts (§10.3): the outward normal is e2 x e1.
            const float n[3] = { e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0] };
            for (const uint16_t i : { a, b, c })
            {
                for (int k = 0; k < 3; ++k)
                {
                    verts[i].n0[k] += n[k];
                }
            }
        };
        for (std::size_t o = sec[0].first; o + 4 <= p.size();)
        {
            const auto op = load<uint16_t>(p, o);
            if (op == 0xFFFF)
            {
                break;
            }
            if (op & 0x8000)
            {
                o += ((op >> 4) & 0xF) == 0 ? 18 : 46;
                continue;
            }
            const std::size_t n = load<uint16_t>(p, o + 2);
            if (op == 0x0054 || op == 0x0043)
            {
                const std::size_t stride = op == 0x0054 ? 30 : 10;
                for (std::size_t t = 0; t < n; ++t)
                {
                    const std::size_t q = o + 4 + stride * t;
                    face(load<uint16_t>(p, q), load<uint16_t>(p, q + 2), load<uint16_t>(p, q + 4));
                }
                o += 4 + stride * n;
            }
            else if (op == 0x5453)
            {
                uint16_t    a = load<uint16_t>(p, o + 4), b = load<uint16_t>(p, o + 6), c = load<uint16_t>(p, o + 8);
                std::size_t q = o + 34;
                face(a, b, c);
                for (std::size_t t = 1; t < n; ++t, q += 10)
                {
                    a = b;
                    b = c;
                    c = load<uint16_t>(p, q);
                    (t & 1) ? face(b, a, c) : face(a, b, c);
                }
                o += 24 + 10 * n;
            }
            else if (op == 0x4353)
            {
                for (std::size_t t = 0; t < n; ++t)
                {
                    const uint16_t a = load<uint16_t>(p, o + 4 + 2 * t), b = load<uint16_t>(p, o + 6 + 2 * t), c = load<uint16_t>(p, o + 8 + 2 * t);
                    (t & 1) ? face(b, a, c) : face(a, b, c);
                }
                o += 12 + 2 * n; // as the main parser
            }
            else
            {
                break;
            }
        }
        for (auto& v : verts)
        {
            const float l = std::sqrt(v.n0[0] * v.n0[0] + v.n0[1] * v.n0[1] + v.n0[2] * v.n0[2]);
            for (int k = 0; k < 3; ++k)
            {
                v.n0[k] = l > 1e-6f ? v.n0[k] / l : (k == 1 ? -1.0f : 0.0f);
                if (v.two)
                {
                    v.n0[k] *= 1.0f - v.w1; // pre-weighted like the positions
                    v.n1[k] = v.n0[k] / std::max(1.0f - v.w1, 1e-3f) * v.w1;
                }
            }
        }
    }
    for (std::size_t i = 0; !cloth && i < n1; ++i, vo += 24)
    {
        auto& v = verts[i];
        for (int k = 0; k < 3; ++k)
        {
            v.p0[k] = load<float>(p, vo + 4 * static_cast<std::size_t>(k));
            v.n0[k] = load<float>(p, vo + 12 + 4 * static_cast<std::size_t>(k));
        }
        v.two = false;
        v.w1  = 0.0f;
    }
    for (std::size_t i = 0; !cloth && i < n2; ++i, vo += 56)
    {
        auto& v = verts[n1 + i];
        float f[14];
        for (std::size_t k = 0; k < 14; ++k)
        {
            f[k] = load<float>(p, vo + 4 * k);
        }
        // x1,x2,y1,y2,z1,z2,w1,w2, nx1,nx2,ny1,ny2,nz1,nz2
        for (int k = 0; k < 3; ++k)
        {
            v.p0[k] = f[2 * k];
            v.p1[k] = f[2 * k + 1];
            v.n0[k] = f[8 + 2 * k];
            v.n1[k] = f[8 + 2 * k + 1];
        }
        v.w1  = f[7]; // weight of the second bone (f[6] = first bone's weight; they sum to 1)
        v.two = true;
    }
    const bool useTable = (flags2 & 0x80) != 0;
    auto       boneOf   = [&](uint16_t ref, bool mirror) -> uint8_t {
        const uint16_t i = mirror ? ((ref >> 7) & 0x7F) : (ref & 0x7F);
        const uint16_t b = useTable && i < boneTable.size() ? boneTable[i] : i;
        return static_cast<uint8_t>(b < boneCount ? b : 0);
    };

    if (cloth)
    {
        out.clothParticles = static_cast<uint16_t>(out.clothParticles + cm.particles.size());
        out.cloth.push_back(cm);
    }
    for (const bool mirror : { false, true })
    {
        if (mirror && !(flags4 & 1))
        {
            break;
        }
        std::string  texture;
        std::size_t  o = sec[0].first;
        auto corner = [&](uint16_t idx, float u, float vv) {
            CharVertex cv;
            if (idx >= nv)
            {
                return cv;
            }
            const auto& v  = verts[idx];
            const auto  r0 = refs[2 * idx], r1 = refs[2 * idx + 1];
            const float* s0 = mirror ? kMirror[r0 >> 14] : kMirror[0];
            const float* s1 = mirror ? kMirror[r1 >> 14] : kMirror[0];
            for (int k = 0; k < 3; ++k)
            {
                cv.p0[k] = v.p0[k] * s0[k];
                cv.n0[k] = v.n0[k] * s0[k];
                cv.p1[k] = v.two ? v.p1[k] * s1[k] : 0.0f;
                cv.n1[k] = v.two ? v.n1[k] * s1[k] : 0.0f;
            }
            cv.bone0   = boneOf(r0, mirror);
            cv.bone1   = v.two ? boneOf(r1, mirror) : cv.bone0;
            cv.weight1 = v.two ? v.w1 : 0.0f;
            cv.uv[0]   = u;
            cv.uv[1]   = vv;
            if (cloth)
            {
                cv.cloth = static_cast<int16_t>(cm.first + idx);
            }
            return cv;
        };
        auto emit = [&](uint16_t a, uint16_t b, uint16_t c, const float* ua, const float* ub, const float* uc) {
            if (group != 0 && ((out.hiddenGroups >> group) & 1))
            {
                return;
            }
            // Mirroring flips handedness: swap two corners to keep clockwise fronts.
            if (mirror)
            {
                std::swap(b, c);
                std::swap(ub, uc);
            }
            if (out.parts.empty() || out.parts.back().texture != texture || out.parts.back().lightScale != lightScale ||
                out.parts.back().env != env || out.parts.back().spec != spec || out.parts.back().strength != strength)
            {
                CharacterAsset::Part part{ texture, static_cast<uint32_t>(out.vertices.size()), 0 };
                part.lightScale = lightScale;
                part.env        = env;
                part.spec       = spec;
                part.strength   = strength;
                out.parts.push_back(part);
            }
            out.vertices.push_back(corner(a, ua[0], ua[1]));
            out.vertices.push_back(corner(b, ub[0], ub[1]));
            out.vertices.push_back(corner(c, uc[0], uc[1]));
            out.parts.back().count += 3;
        };
        const float zero[2] = { 0, 0 };
        group                = 0;
        lightScale           = 1.0f;
        strength             = 0.0f;
        env = spec           = false;
        for (;;)
        {
            const auto op = load<uint16_t>(p, o);
            if (op == 0xFFFF)
            {
                break;
            }
            if (op & 0x8000)
            {
                if (((op >> 4) & 0xF) == 0)
                {
                    texture = std::string(reinterpret_cast<const char*>(p.data() + o + 2), 16); // 0x8000 texture name
                    o += 18;
                }
                else
                {
                    // 0x8010 material: u16 +0x0C bits 8..11 = hide group; a group-4 material with u16 +0x1C set counts
                    // as group 8 (CODE 0x1002cb0d).
                    group = (load<uint16_t>(p, o + 2 + 0x0C) >> 8) & 0xF;
                    if (group == 4 && load<uint16_t>(p, o + 2 + 0x1C) != 0)
                    {
                        group = 8;
                    }
                    // Lighting scale (+0x10), environment map (+0x0E), specular map (+0x28 != 0) and their strength
                    // (+0x24 x 0.5 = TFACTOR alpha; CODE 0x1002e184 / 0x1002e2ca).
                    lightScale = load<float>(p, o + 2 + 0x10);
                    if (!(lightScale > 0.0f && lightScale < 4.0f))
                    {
                        lightScale = 1.0f;
                    }
                    env      = load<uint16_t>(p, o + 2 + 0x0E) != 0;
                    spec     = !env && load<float>(p, o + 2 + 0x28) != 0.0f; // env wins (the spec branch is the else)
                    strength = std::clamp(load<float>(p, o + 2 + 0x24) * 0.5f, 0.0f, 255.0f) / 255.0f;
                    o += 46;
                }
                continue;
            }
            const auto n = load<uint16_t>(p, o + 2);
            if (op == 0x0054) // triangle list: n x (3 idx, 3 uv)
            {
                for (std::size_t t = 0; t < n; ++t)
                {
                    const std::size_t q = o + 4 + 30 * t;
                    float             uv[6];
                    for (std::size_t k = 0; k < 6; ++k)
                    {
                        uv[k] = load<float>(p, q + 6 + 4 * k);
                    }
                    emit(load<uint16_t>(p, q), load<uint16_t>(p, q + 2), load<uint16_t>(p, q + 4), uv, uv + 2, uv + 4);
                }
                o += 4 + 30 * static_cast<std::size_t>(n);
            }
            else if (op == 0x5453) // strip: first tri (3 idx, 3 uv), then n-1 x (idx, uv)
            {
                std::size_t q = o + 4;
                uint16_t    a = load<uint16_t>(p, q), b = load<uint16_t>(p, q + 2), c = load<uint16_t>(p, q + 4);
                float       ua[2] = { load<float>(p, q + 6), load<float>(p, q + 10) }, ub[2] = { load<float>(p, q + 14), load<float>(p, q + 18) },
                      uc[2] = { load<float>(p, q + 22), load<float>(p, q + 26) };
                emit(a, b, c, ua, ub, uc);
                q += 30;
                for (std::size_t t = 1; t < n; ++t)
                {
                    const uint16_t d     = load<uint16_t>(p, q);
                    float          ud[2] = { load<float>(p, q + 2), load<float>(p, q + 6) };
                    q += 10;
                    a = b;
                    b = c;
                    c = d;
                    std::copy_n(ub, 2, ua);
                    std::copy_n(uc, 2, ub);
                    std::copy_n(ud, 2, uc);
                    if (t & 1)
                    {
                        emit(b, a, c, ub, ua, uc);
                    }
                    else
                    {
                        emit(a, b, c, ua, ub, uc);
                    }
                }
                o += 24 + 10 * static_cast<std::size_t>(n);
            }
            else if (op == 0x0043) // untextured list: n x (3 idx, u32 colour)
            {
                const std::string saved = texture;
                texture.clear();
                for (std::size_t t = 0; t < n; ++t)
                {
                    const std::size_t q = o + 4 + 10 * t;
                    emit(load<uint16_t>(p, q), load<uint16_t>(p, q + 2), load<uint16_t>(p, q + 4), zero, zero, zero);
                }
                texture = saved;
                o += 4 + 10 * static_cast<std::size_t>(n);
            }
            else if (op == 0x4353) // untextured strip
            {
                const std::string saved = texture;
                texture.clear();
                for (std::size_t t = 0; t < n; ++t)
                {
                    const uint16_t a = load<uint16_t>(p, o + 4 + 2 * t), b = load<uint16_t>(p, o + 6 + 2 * t), c = load<uint16_t>(p, o + 8 + 2 * t);
                    if (t & 1)
                    {
                        emit(b, a, c, zero, zero, zero);
                    }
                    else
                    {
                        emit(a, b, c, zero, zero, zero);
                    }
                }
                texture = saved;
                o += 12 + 2 * static_cast<std::size_t>(n);
            }
            else
            {
                spdlog::warn("Os2: unknown command {:#06x}", op);
                return;
            }
        }
    }
}

// The hide groups (bit n = group n) a mesh with header byte 3 = `code` switches off (CODE 0x1002ded4).
uint16_t os2HiddenGroups(uint8_t code)
{
    switch (code)
    {
        case 0x02: return 1u << 1;
        case 0x03: return (1u << 1) | (1u << 2);
        case 0x04: return (1u << 1) | (1u << 2) | (1u << 3);
        case 0x05: return (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 8);
        case 0x06: return (1u << 1) | (1u << 2) | (1u << 3) | (1u << 8);
        case 0x12: return 1u << 5;
        case 0x22: return 1u << 7;
        case 0x32: return 1u << 6;
        default: return 0;
    }
}

// Adds the global "cubemap spec" texture (file 0, chunk `spec`: a 64x64 highlight blob; CODE 0x100d6c60) when a part
// uses the specular map.
void addSpecTexture(const DatIndex& index, CharacterAsset& out)
{
    if (std::ranges::none_of(out.parts, [](const CharacterAsset::Part& p) { return p.spec; }) || !index.path(0))
    {
        return;
    }
    const auto         data = index.read(0);
    std::vector<Chunk> chunks;
    parseChunks(data, chunks);
    for (const auto& c : chunks)
    {
        if (c.type == 0x20 && c.name == "spec")
        {
            out.textures.push_back(parseTexture(c.payload));
            out.specTexture = out.textures.back().name;
            return;
        }
    }
}

// Reads every texture and Os2 mesh from one file into the asset.
void appendFile(const DatIndex& index, uint32_t fileId, CharacterAsset& out, bool takeSkeleton)
{
    if (!index.path(fileId))
    {
        return;
    }
    const auto         data = index.read(fileId);
    std::vector<Chunk> chunks;
    parseChunks(data, chunks);
    for (const auto& c : chunks)
    {
        if (takeSkeleton && c.type == 41 && out.skeleton.bones.empty())
        {
            out.skeleton = Skeleton::parse(c.payload);
        }
    }
    for (const auto& c : chunks)
    {
        try
        {
            if (c.type == 0x20 || c.type == 0x1B)
            {
                out.textures.push_back(parseTexture(c.payload));
                // The environment texture is looked up by its chunk name `evev` in the actor's files (CODE 0x100d6cc2);
                // INFERRED: the first one found serves the whole actor.
                if (c.name == "evev" && out.envTexture.empty())
                {
                    out.envTexture = out.textures.back().name;
                }
            }
            else if (c.type == 42 && !out.skeleton.bones.empty())
            {
                const std::size_t before = out.parts.size();
                appendOs2(c.payload, out.skeleton.bones.size(), out);
                if (c.name.size() == 4 && c.name.starts_with("wep") && c.name[3] >= '0' && c.name[3] <= '9')
                {
                    for (std::size_t k = before; k < out.parts.size(); ++k)
                    {
                        out.parts[k].wep = static_cast<int8_t>(c.name[3] - '0');
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            spdlog::debug("character file {}: {}: {}", fileId, c.name, e.what());
        }
    }
}

} // namespace

Skeleton Skeleton::parse(std::span<const std::byte> p)
{
    Skeleton   s;
    const auto n = load<uint16_t>(p, 2);
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::size_t o = 4 + 30 * i;
        Bone              b;
        b.parent = load<uint8_t>(p, o);
        for (std::size_t k = 0; k < 4; ++k)
        {
            b.rotation[k] = load<float>(p, o + 2 + 4 * k);
        }
        for (std::size_t k = 0; k < 3; ++k)
        {
            b.translation[k] = load<float>(p, o + 18 + 4 * k);
        }
        const auto local = boneLocal(b.rotation, b.translation);
        s.bindWorld.push_back(i == 0 ? local : multiply(local, s.bindWorld[b.parent < i ? b.parent : 0]));
        s.bones.push_back(b);
    }
    const std::size_t table = 4 + 30 * static_cast<std::size_t>(n);
    const auto        count = load<uint16_t>(p, table);
    for (std::size_t i = 0; i < count && table + 4 + 26 * (i + 1) <= p.size(); ++i)
    {
        const std::size_t o = table + 4 + 26 * i;
        s.points.push_back({ load<int16_t>(p, o), { load<float>(p, o + 14), load<float>(p, o + 18), load<float>(p, o + 22) } });
    }
    for (std::size_t i = 0; i < 3; ++i)
    {
        const std::size_t o = table + 4 + 26 * static_cast<std::size_t>(count) + 24 * i;
        for (std::size_t k = 0; k < 6 && o + 24 <= p.size(); ++k)
        {
            s.bounds[i][k] = load<float>(p, o + 4 * k);
        }
    }
    for (std::size_t i = 0; i < 2; ++i)
    {
        const std::size_t o = table + 4 + 26 * static_cast<std::size_t>(count) + 0x48 + 12 * i;
        if (o + 12 <= p.size())
        {
            s.lookLimits[i] = { load<float>(p, o), load<float>(p, o + 4), load<float>(p, o + 8) };
        }
    }
    for (std::size_t o = table + 4 + 26 * static_cast<std::size_t>(count) + 0x48 + 0x24; o + 0x12 <= p.size(); o += 0x12)
    {
        const Skeleton::Sphere sp{ load<int16_t>(p, o), load<float>(p, o + 2), { load<float>(p, o + 6), load<float>(p, o + 10), load<float>(p, o + 14) } };
        if (sp.bone < 0 || static_cast<std::size_t>(sp.bone) >= n || !(sp.radius > 0.0f && sp.radius < 10.0f))
        {
            break; // the 0xCD padding after the last record
        }
        s.spheres.push_back(sp);
    }
    return s;
}

namespace
{

// A byte of a weapon model's 16-byte `info` chunk (type 69). Byte 3 is the weapon's motion class, a per-race index:
// the same model can have a different class on another race (§10.6). Byte 14 of a ranged model is its ranged class
// (effect-format §C.5: the `lcNN` / `lsNN` schedulers).
std::optional<uint8_t> weaponInfoByte(const DatIndex& index, uint32_t fileId, std::size_t at)
{
    if (!index.path(fileId))
    {
        return std::nullopt;
    }
    const auto         data = index.read(fileId);
    std::vector<Chunk> chunks;
    parseChunks(data, chunks);
    for (const auto& c : chunks)
    {
        if (c.type == 69 && c.payload.size() > at)
        {
            return static_cast<uint8_t>(c.payload[at]);
        }
    }
    return std::nullopt;
}

} // namespace

namespace
{
// Bytes `at`, `at + 1`, ... of the first chunk of `type`'s payload in a file (footwear sound bytes, CODE 0x100d0b86 /
// 0x100cf2a1; the client's resource object puts the payload at +0x30, so its +0x31 is payload byte 1).
std::optional<std::vector<uint8_t>> chunkBytes(const DatIndex& index, uint32_t fileId, uint8_t type, std::size_t at, std::size_t n)
{
    if (!index.path(fileId))
    {
        return std::nullopt;
    }
    const auto         data = index.read(fileId);
    std::vector<Chunk> chunks;
    parseChunks(data, chunks);
    for (const auto& c : chunks)
    {
        if (c.type == type && c.payload.size() >= at + n)
        {
            std::vector<uint8_t> out(n);
            for (std::size_t k = 0; k < n; ++k)
            {
                out[k] = static_cast<uint8_t>(c.payload[at + k]);
            }
            return out;
        }
    }
    return std::nullopt;
}
} // namespace

float mountCameraHeight(const DatIndex& index, uint32_t fileId)
{
    if (!index.path(fileId))
    {
        return 0.0f;
    }
    const auto         data = index.read(fileId);
    std::vector<Chunk> chunks;
    parseChunks(data, chunks);
    for (const auto& c : chunks)
    {
        if (c.type == 0x45 && c.name == "info" && c.payload.size() >= 15)
        {
            const auto b = static_cast<uint8_t>(c.payload[14]);
            return b == 0xFF ? 0.0f : static_cast<float>(b) * 0.01f;
        }
    }
    return 0.0f;
}

std::optional<CharacterAsset> loadPcAsset(const DatIndex& index, uint8_t race, uint8_t face, const std::array<uint16_t, 8>& gear)
{
    CharacterAsset a;
    const uint32_t base = raceBase(race);
    // The groups the worn meshes hide, by each Os2 header's byte 3 (CODE 0x1002ded4..0x1002dfaa), before any mesh is read.
    for (std::size_t s = 0; s < 5; ++s)
    {
        if (const auto f = slotFileId(race, s + 1, gear[s]); f && index.path(*f))
        {
            const auto         data = index.read(*f);
            std::vector<Chunk> chunks;
            parseChunks(data, chunks);
            for (const auto& c : chunks)
            {
                if (c.type == 42 && c.payload.size() > 3)
                {
                    a.hiddenGroups |= os2HiddenGroups(static_cast<uint8_t>(c.payload[3]));
                }
            }
        }
    }
    appendFile(index, base, a, true);
    a.motionFiles = { base, base + 1, base + 2, base + 3, base + 4 }; // §10.4: pieces 0 / 1 / 2 across these
    if (race >= 32 && race <= 36)
    {
        a.motionFiles = { base }; // chocobos: one-piece motions in the base file only (base + 1 is a copy the game skips)
    }
    if (a.skeleton.bones.empty())
    {
        return std::nullopt;
    }
    if (const auto f = slotFileId(race, 0, face))
    {
        appendFile(index, *f, a, false);
        a.gearFiles.push_back(*f);
    }
    // Armour (head..feet) then weapons (main, sub, ranged). A look id with no index bits (low 12 = 0) is
    // "nothing equipped" for weapons.
    for (std::size_t s = 0; s < 8; ++s)
    {
        if (s >= 5 && (gear[s] & 0xFFF) == 0)
        {
            continue;
        }
        if (const auto f = slotFileId(race, s + 1, gear[s]))
        {
            if (s < 5)
            {
                a.gearFiles.push_back(*f);
            }
            const std::size_t partsBefore = a.parts.size();
            appendFile(index, *f, a, false);
            if (s == 5 && a.parts.size() > partsBefore && a.parts[partsBefore].count > 0) // main weapon: one-bone mesh
            {
                a.mainWeaponBone = a.vertices[a.parts[partsBefore].first].bone0;
            }
            if (s == 1) // body: the cloth variant (dat-format §10.8)
            {
                if (const auto b = chunkBytes(index, *f, 0x45, 9, 1))
                {
                    a.bodyVariant = (*b)[0];
                }
            }
            if (s == 4) // feet: the footwear sound, from the `info` chunk (type 0x45)
            {
                if (const auto b = chunkBytes(index, *f, 0x45, 1, 2))
                {
                    a.shoe = static_cast<int8_t>((*b)[0]), a.shoeVariant = static_cast<int8_t>((*b)[1]);
                }
            }
        }
    }
    // Barefoot / defaults from the skeleton resource (+0x30 foot layout, +0x31 / +0x32 footwear), only where unset.
    if (const auto b = chunkBytes(index, base, 41, 0, 3))
    {
        a.footLayout = (*b)[0];
        if (a.shoe < 0)
        {
            a.shoe = static_cast<int8_t>((*b)[1]);
        }
        if (a.shoeVariant < 0)
        {
            a.shoeVariant = static_cast<int8_t>((*b)[2]);
        }
    }
    // Battle motions by the main weapon's class (no weapon: model 0). Pieces 0/1 sit at base + 2601 + class and
    // piece 2 at base + 2729 + class; class 10 (unarmed) uses base + 2727 / base + 2855 (§10.6, data-derived).
    if (const auto wf = slotFileId(race, 6, static_cast<uint16_t>(gear[5] & 0xFFF)))
    {
        a.weaponFiles = { *wf };
        if (const auto cls = weaponInfoByte(index, *wf, 3))
        {
            a.weaponClass = *cls;
            if (const auto style = weaponInfoByte(index, *wf, 4))
            {
                a.sheathStyle = std::max(0, static_cast<int>(static_cast<int8_t>(*style))); // max(0, s8) (CODE 0x100d2e00)
            }
            if (*cls < 10)
            {
                a.battleMotionFiles = { base + 2601 + *cls, base + 2729 + *cls };
            }
            else if (*cls == 10)
            {
                a.battleMotionFiles = { base + 2727, base + 2855 };
            }
        }
    }
    if ((gear[7] & 0xFFF) != 0)
    {
        if (const auto rf = slotFileId(race, 8, static_cast<uint16_t>(gear[7] & 0xFFF)))
        {
            if (const auto rc = weaponInfoByte(index, *rf, 14))
            {
                a.rangedClass = *rc;
            }
        }
    }
    addSpecTexture(index, a);
    return a;
}

std::optional<CharacterAsset> loadNpcAsset(const DatIndex& index, uint16_t modelId)
{
    return loadModelFile(index, npcFileId(modelId));
}

std::optional<EffectLook> effectLook(const DatIndex& index, uint32_t fileId)
{
    if (!index.path(fileId))
    {
        return std::nullopt;
    }
    const auto         data = index.read(fileId);
    std::vector<Chunk> chunks;
    parseChunks(data, chunks);
    for (const auto& c : chunks)
    {
        if (c.type != 7 || c.name != "init" || c.payload.size() < 0x18)
        {
            continue;
        }
        // Records from the scheduler header's +0x14 (less the 16-byte chunk header): op, length in dwords, +8 value.
        EffectLook  l;
        bool        any = false;
        const auto  p   = c.payload;
        std::size_t o   = load<uint32_t>(p, 0x14) - 0x10;
        while (o + 8 <= p.size())
        {
            const auto w  = load<uint32_t>(p, o);
            const auto op = w & 0xFF;
            const auto ln = (w >> 8) & 0x1F;
            if (op == 0 || ln == 0)
            {
                break;
            }
            if (ln >= 3 && o + 12 <= p.size())
            {
                const auto v = load<uint32_t>(p, o + 8);
                if (op == 0x8D)
                {
                    l.race = static_cast<int8_t>(v & 0xFF), any = true;
                }
                else if (op >= 0x8E && op <= 0x96)
                {
                    l.slots[op - 0x8E] = static_cast<uint16_t>(v), any = true;
                }
                else if (op == 0x98)
                {
                    l.model = static_cast<int>(v & 0xFFFF), any = true; // vtable +0x104 -> +0x108(0, v): slot 0
                }
                else if (op == 0x99)
                {
                    l.scale = load<float>(p, o + 8), any = true;
                }
            }
            o += 4 * ln;
        }
        if (any)
        {
            return l;
        }
    }
    return std::nullopt;
}

namespace
{
std::optional<CharacterAsset> loadModelFileDepth(const DatIndex& index, uint32_t fileId, int depth)
{
    if (!index.path(fileId))
    {
        return std::nullopt;
    }
    CharacterAsset a;
    appendFile(index, fileId, a, true);
    a.motionFiles = { fileId };
    if (a.skeleton.bones.empty() || a.vertices.empty())
    {
        // Effects only: the `init` scheduler builds a PC look or names another model (EffectLook).
        const auto l = depth < 4 ? effectLook(index, fileId) : std::nullopt;
        if (!l)
        {
            return std::nullopt;
        }
        std::optional<CharacterAsset> b;
        if (l->race > 0)
        {
            b = loadPcAsset(index, static_cast<uint8_t>(l->race), static_cast<uint8_t>(l->slots[0] & 0xFF),
                            { l->slots[1], l->slots[2], l->slots[3], l->slots[4], l->slots[5], l->slots[6], l->slots[7], l->slots[8] });
        }
        else if (l->model > 0)
        {
            b = loadModelFileDepth(index, npcFileId(static_cast<uint16_t>(l->model)), depth + 1);
        }
        if (b)
        {
            b->effectFiles.insert(b->effectFiles.begin(), fileId);
            b->scale *= l->scale > 0.0f && l->scale < 100.0f ? l->scale : 1.0f;
        }
        return b;
    }
    addSpecTexture(index, a);
    return a;
}
} // namespace

std::optional<CharacterAsset> loadModelFile(const DatIndex& index, uint32_t fileId)
{
    return loadModelFileDepth(index, fileId, 0);
}

} // namespace xi::dat
