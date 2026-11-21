// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/motion.h"

#include "dat/chunk.h"
#include "xi/bytes.h"

#include <algorithm>

#include <cmath>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xi::dat
{

namespace
{

// Hamilton product a (x) b, quaternions as x, y, z, w (client 0x10032a80).
std::array<float, 4> qmul(const std::array<float, 4>& a, const std::array<float, 4>& b)
{
    return { a[3] * b[0] + b[3] * a[0] + (a[1] * b[2] - a[2] * b[1]), a[3] * b[1] + b[3] * a[1] + (a[2] * b[0] - a[0] * b[2]),
             a[3] * b[2] + b[3] * a[2] + (a[0] * b[1] - a[1] * b[0]), a[3] * b[3] - (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) };
}

RowMatrix rowFromQuatTrans(const std::array<float, 4>& q, const std::array<float, 3>& t)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),     0,
             2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),     0,
             2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y), 0,
             t[0],                    t[1],                    t[2],                    1 };
}

RowMatrix rowMul(const RowMatrix& a, const RowMatrix& b)
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

} // namespace

Motion Motion::parse(std::span<const std::byte> p)
{
    Motion     m;
    const auto e = load<int16_t>(p, 2);
    m.frames_    = static_cast<uint16_t>(std::max<int16_t>(1, load<int16_t>(p, 4)));
    m.speed_     = load<float>(p, 6);
    m.keys_.resize((p.size() - 0x0A) / 4);
    for (std::size_t i = 0; i < m.keys_.size(); ++i)
    {
        m.keys_[i] = load<float>(p, 0x0A + 4 * i);
    }
    for (int i = 0; i < e; ++i)
    {
        const std::size_t o = 0x0A + 84 * static_cast<std::size_t>(i);
        Track             t;
        t.bone = load<uint32_t>(p, o);
        // dw1..4 rotation idx, dw5..8 defaults, dw9..11 translation idx, dw12..14 defaults, dw15..17 scale idx,
        // dw18..20 defaults.
        for (std::size_t k = 0; k < 4; ++k)
        {
            t.index[k] = load<uint32_t>(p, o + 4 + 4 * k);
            t.value[k] = load<float>(p, o + 20 + 4 * k);
        }
        for (std::size_t k = 0; k < 3; ++k)
        {
            t.index[4 + k] = load<uint32_t>(p, o + 36 + 4 * k);
            t.value[4 + k] = load<float>(p, o + 48 + 4 * k);
            t.index[7 + k] = load<uint32_t>(p, o + 60 + 4 * k);
            t.value[7 + k] = load<float>(p, o + 72 + 4 * k);
        }
        t.driven = !(t.index[0] & 0x80000000u); // INFERRED: "bone not driven by this motion"
        m.tracks_.push_back(t);
    }
    return m;
}

void Motion::sample(float frame, std::vector<BonePose>& out) const
{
    const float f  = std::fmod(std::max(frame, 0.0f), static_cast<float>(frames_));
    const auto  f0 = static_cast<uint32_t>(f);
    const auto  f1 = (f0 + 1) % frames_;
    const float a  = f - static_cast<float>(f0);
    for (const auto& t : tracks_)
    {
        if (!t.driven || t.bone >= out.size())
        {
            continue;
        }
        // Keys at frames f0 and f1, then the blend. The rotation takes the shorter way round: q and -q are the same
        // rotation and the data flips sign between keys on some bones, which a plain per-component blend turns into a
        // swing through the near-zero quaternion half way between keys. At the game's 30 fps only whole frames are
        // shown, so this matters from 60 fps up.
        float k0[10], k1[10];
        for (std::size_t k = 0; k < 10; ++k)
        {
            const uint32_t idx = t.index[k] & 0x7FFFFFFFu;
            if (idx == 0 || idx + frames_ > keys_.size())
            {
                k0[k] = k1[k] = t.value[k];
            }
            else
            {
                k0[k] = keys_[idx + f0];
                k1[k] = keys_[idx + f1];
            }
        }
        if (k0[0] * k1[0] + k0[1] * k1[1] + k0[2] * k1[2] + k0[3] * k1[3] < 0.0f)
        {
            for (std::size_t k = 0; k < 4; ++k)
            {
                k1[k] = -k1[k];
            }
        }
        float v[10];
        for (std::size_t k = 0; k < 10; ++k)
        {
            v[k] = k0[k] + (k1[k] - k0[k]) * a;
        }
        auto&       bp  = out[t.bone];
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
        bp.driven       = true;
        bp.rotation     = len > 1e-6f ? std::array<float, 4>{ v[0] / len, v[1] / len, v[2] / len, v[3] / len } : std::array<float, 4>{ 0, 0, 0, 1 };
        bp.translation  = { v[4], v[5], v[6] };
        bp.scale        = { v[7], v[8], v[9] };
    }
}

namespace
{

// One file's Mo2 chunks, parsed once per process (keyed by the file's path): characters of one race share their
// motion files, and reading + parsing them was most of a new character's load time (docs/performance.md).
const std::vector<std::pair<std::string, Motion>>& fileMotions(const DatIndex& index, uint32_t file, const std::filesystem::path& path)
{
    static std::mutex                                                                lock;
    static std::unordered_map<std::string, std::vector<std::pair<std::string, Motion>>> cache;
    const std::scoped_lock                                                           guard(lock);
    auto                                                                             it = cache.find(path.string());
    if (it == cache.end())
    {
        std::vector<std::pair<std::string, Motion>> motions;
        const auto                                  data = index.read(file);
        std::vector<Chunk>                          chunks;
        parseChunks(data, chunks);
        for (const auto& c : chunks)
        {
            if (c.type == 43)
            {
                motions.emplace_back(c.name, Motion::parse(c.payload));
            }
        }
        it = cache.emplace(path.string(), std::move(motions)).first;
    }
    return it->second;
}

// Mo2 chunks by name across `files`; the first file holding a name wins (§10.4).
std::unordered_map<std::string, Motion> collectMotions(const DatIndex& index, const std::vector<uint32_t>& files)
{
    std::unordered_map<std::string, Motion> out;
    for (const auto f : files)
    {
        const auto path = index.path(f);
        if (!path)
        {
            continue;
        }
        for (const auto& [name, motion] : fileMotions(index, f, *path))
        {
            if (!out.contains(name))
            {
                out.emplace(name, motion);
            }
        }
    }
    return out;
}

MotionSet assemble(const std::unordered_map<std::string, Motion>& motions, const std::string& stem)
{
    MotionSet set;
    for (char piece = '0'; piece <= '3'; ++piece)
    {
        if (const auto it = motions.find(stem + piece); it != motions.end())
        {
            set.pieces.push_back(it->second);
        }
    }
    return set;
}

} // namespace

MotionSet loadMotionSet(const DatIndex& index, const std::vector<uint32_t>& files, const std::string& stem)
{
    return assemble(collectMotions(index, files), stem);
}

namespace
{

// The motion stems a scheduler chunk (type 7) plays, in time order: op 5 records name "xyz?" (all pieces of stem
// "xyz"); op 3 hands off to another scheduler of the same file, followed one level. Records: u8 op, u8 length in
// dwords (low 5 bits), s16 delay, s16 duration, then op data (a 4-character name at +8; op 5: u16 blend-in at
// +0x18). CODE 0x10057ee0. Timing is post-delay (effect-format §S.1): a record runs, then waits its delay.
struct TimedStem
{
    std::string stem;
    float       at       = 0.0f;
    float       duration = 0.0f;
    float       blendIn  = 0.0f;
};

void schedulerStems(const std::vector<Chunk>& chunks, const std::string& name, int depth, float start,
                    std::vector<TimedStem>& out)
{
    for (const auto& c : chunks)
    {
        if (c.type != 7 || c.name != name || c.payload.size() < 0x18)
        {
            continue;
        }
        const auto  p = c.payload;
        std::size_t o = load<uint32_t>(p, 0x14) - 0x10;
        float       t = start;
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
                std::string n(reinterpret_cast<const char*>(p.data() + o + 8), 4);
                const bool  printable = std::all_of(n.begin(), n.end(), [](char ch) { return ch > 0x20 && ch < 0x7F; });
                const auto  dur       = static_cast<float>(std::max<int16_t>(load<int16_t>(p, o + 6), 0));
                const auto  blend =
                    ln >= 7 && o + 0x1A <= p.size() ? static_cast<float>(load<uint16_t>(p, o + 0x18)) : 0.0f;
                if (op == 5 && n.back() == '?')
                {
                    out.push_back({ n.substr(0, 3), t, dur, blend }); // all pieces of the stem
                }
                else if (op == 5 && printable)
                {
                    out.push_back({ n, t, dur, blend }); // one motion by its full name
                }
                else if (op == 3 && depth < 2 && printable)
                {
                    schedulerStems(chunks, n, depth + 1, t, out);
                }
            }
            t += static_cast<float>(std::max<int16_t>(load<int16_t>(p, o + 4), 0));
            o += ln * 4;
        }
        std::stable_sort(out.begin(), out.end(), [](const TimedStem& a, const TimedStem& b) { return a.at < b.at; });
        return;
    }
}

void setTiming(MotionSet& set, const TimedStem& s)
{
    set.at       = s.at;
    set.duration = s.duration;
    set.blendIn  = s.blendIn;
}

} // namespace

std::vector<MotionSet> loadWeaponSkillMotions(const DatIndex& index, uint8_t race, uint16_t animation)
{
    // fileA bases by race (CODE table 0x1035f4cc): Hume M/F, Elvaan M/F, Tarutaru M/F, Mithra, Galka.
    static constexpr uint32_t kBase[9] = { 0, 33227, 33995, 34763, 35531, 36299, 36299, 37067, 37835 };
    if (race == 0 || race > 8 || animation >= 256)
    {
        return {};
    }
    const uint32_t fileA = kBase[race] + animation;
    // fileB (piece 2): +256 or +512 by the body model's info byte 9 (1 / other); the first variant, as for stances.
    const uint32_t     fileB = fileA + 256;
    std::vector<Chunk> chunks;
    const auto         data = index.read(fileA); // the chunks point into it
    parseChunks(data, chunks);
    std::vector<TimedStem> stems;
    schedulerStems(chunks, "main", 0, 0.0f, stems);
    const auto             motions = collectMotions(index, { fileA, fileB });
    std::vector<MotionSet> out;
    for (const auto& s : stems)
    {
        if (auto set = assemble(motions, s.stem); !set.empty())
        {
            setTiming(set, s);
            out.push_back(std::move(set));
        }
    }
    return out;
}

MotionSet loadEmoteMotion(const DatIndex& index, uint8_t race, uint8_t emote)
{
    // Race base files (skeleton + motions, dat-format §10.1); Tarutaru M/F share one.
    static constexpr uint32_t kBase[9] = { 0, 7072, 10248, 13424, 16600, 19776, 19776, 23176, 26352 };
    // Emote id (LSB enums/emote.h) -> group file offset from base + 2984, and the motion stem. The groups follow the
    // ids: 0-5 in group 0, then eight per group (6-13, 14-21, 22-29, 30-37); the stems are matched by name (INFERRED).
    struct Entry
    {
        uint8_t     emote;
        uint8_t     group;
        const char* stem;
    };
    static constexpr Entry kEmotes[] = {
        { 0, 0, "poi" },  { 1, 0, "bow" },  { 2, 0, "sl1" },  { 3, 0, "kne" },  { 4, 0, "lau" },  { 5, 0, "wee" },   // point bow salute kneel laugh cry
        { 6, 1, "den" },  { 7, 1, "nod" },  { 8, 1, "wav" },  { 9, 1, "wav" },  { 10, 1, "wel" }, { 11, 1, "gla" },  // no yes wave goodbye welcome joy
        { 12, 1, "che" }, { 13, 1, "clp" },                                                                          // cheer clap
        { 14, 2, "clp" }, { 16, 2, "pok" }, { 18, 2, "sta" }, { 19, 2, "sig" }, { 20, 2, "cmf" }, { 21, 2, "sur" }, // praise poke stagger sigh comfort surprised
        { 24, 3, "blu" }, { 25, 3, "ang" }, { 28, 3, "nod" }, { 29, 3, "pan" },                                     // blush angry doze panic
        { 32, 4, "thk" }, { 33, 4, "fum" },                                                                          // think fume
        { 41, 6, "dig" }, { 42, 7, "mow" },                                                                          // excavation harvesting
        { 43, 10, "rs0" }, { 44, 11, "tos" },                                                                        // hurray toss
        { 65, 12, "dc0" }, { 66, 12, "dc1" }, { 67, 12, "dc2" }, { 68, 12, "dc3" },                                 // dance 1-4
    };
    if (race == 0 || race > 8)
    {
        return {};
    }
    for (const auto& e : kEmotes)
    {
        if (e.emote == emote)
        {
            const uint32_t file = kBase[race] + 2984 + e.group;
            // Piece 2 has two variants (+64 / +128), like the stance files; the first, as elsewhere.
            return assemble(collectMotions(index, { file, file + 64 }), e.stem);
        }
    }
    return {};
}

std::vector<MotionSet> loadMonsterSkillMotions(const DatIndex& index, uint16_t animation, const std::vector<uint32_t>& modelMotionFiles)
{
    // CODE 0x100a8c20: base by animation range.
    const uint32_t file = animation >= 0x800 ? 84743u + (animation - 0x800u)
                        : animation >= 0x600 ? 59193u + (animation - 0x600u)
                        : animation >= 0x200 ? 49647u + (animation - 0x200u)
                                             : 3900u + animation;
    std::vector<Chunk> chunks;
    const auto         data = index.read(file); // the chunks point into it
    parseChunks(data, chunks);
    std::vector<TimedStem> stems;
    schedulerStems(chunks, "main", 0, 0.0f, stems);
    auto files = modelMotionFiles;
    files.push_back(file);
    const auto             motions = collectMotions(index, files);
    std::vector<MotionSet> out;
    for (const auto& s : stems)
    {
        if (s.stem.size() == 4)
        {
            if (const auto it = motions.find(s.stem); it != motions.end())
            {
                MotionSet set;
                set.pieces.push_back(it->second);
                setTiming(set, s);
                out.push_back(std::move(set));
            }
        }
        else if (auto set = assemble(motions, s.stem); !set.empty())
        {
            setTiming(set, s);
            out.push_back(std::move(set));
        }
    }
    return out;
}

namespace
{

// The draw / sheathe stem of sheath style `style` (dat-format §10.6; CODE 0x100d2e00 / 0x100d2e20): the class files'
// scheduler "in N" / "outN" (N = '0' + style) plays it with a caster motion op 0x05 whose 4CC ("inc?") sits at +0x10.
std::string sheathStem(const DatIndex& index, const std::vector<uint32_t>& files, bool draw, int style)
{
    const std::string name = (draw ? std::string("in ") : std::string("out")) + static_cast<char>('0' + std::clamp(style, 0, 9));
    for (const auto f : files)
    {
        if (!index.path(f))
        {
            continue;
        }
        const auto         data = index.read(f);
        std::vector<Chunk> chunks;
        parseChunks(data, chunks);
        for (const auto& c : chunks)
        {
            if (c.type != 7 || c.name != name || c.payload.size() < 0x18)
            {
                continue;
            }
            const auto  p = c.payload;
            std::size_t o = load<uint32_t>(p, 0x14) - 0x10;
            while (o + 8 <= p.size())
            {
                const auto w  = load<uint32_t>(p, o);
                const auto op = w & 0xFF;
                const auto ln = (w >> 8) & 0x1F;
                if (op == 0 || ln == 0)
                {
                    break;
                }
                for (const std::size_t at : { std::size_t{ 0x10 }, std::size_t{ 8 } })
                {
                    if (op == 5 && ln * 4 >= at + 4 && o + at + 4 <= p.size())
                    {
                        const std::string n(reinterpret_cast<const char*>(p.data() + o + at), 4);
                        if (n.back() == '?' && (n.rfind("in", 0) == 0 || n.rfind("ot", 0) == 0))
                        {
                            return n.substr(0, 3);
                        }
                    }
                }
                o += ln * 4;
            }
        }
    }
    return {};
}

} // namespace

CharacterMotions loadCharacterMotions(const DatIndex& index, const std::vector<uint32_t>& files, const std::vector<uint32_t>& battleFiles, int sheathStyle)
{
    const auto       all    = collectMotions(index, files);
    const auto       battle = battleFiles.empty() ? all : collectMotions(index, battleFiles);
    CharacterMotions m;
    m.idle   = assemble(all, "idl");
    if (m.idle.empty())
    {
        m.idle = assemble(all, "chi"); // chocobos (and riders): `chi` is their idle (CODE 0x1008a0d0)
    }
    m.walk   = assemble(all, "wlk");
    m.run    = assemble(all, "run");
    m.battle = assemble(battle, "btl");
    if (!battleFiles.empty())
    {
        auto merged = all;
        for (const auto& [name, motion] : battle)
        {
            merged[name] = motion; // the class file's pieces win
        }
        m.battleWalk = assemble(merged, "wlk");
        m.battleRun  = assemble(merged, "run");
        // mv? from the race files with the class's am?1 as piece 1.
        auto strafe = [&](const char* legs, const char* arms) {
            MotionSet set = assemble(all, legs);
            if (const auto it = battle.find(arms); it != battle.end() && set.pieces.size() >= 2)
            {
                set.pieces[1] = it->second;
            }
            return set;
        };
        m.battleBack  = strafe("mvb", "amb1");
        m.battleLeft  = strafe("mvl", "aml1");
        m.battleRight = strafe("mvr", "amr1");
    }
    m.death  = assemble(all, "ded");
    m.corpse = assemble(all, "cor");
    // Resting (status 33, /heal and the logout wait): the status motion picker plays `rx1` (CODE 0x100c8c1b; sit,
    // status 47, `si1`); rx0 / rx2 kneel down and stand up.
    m.restDown = assemble(all, "rx0");
    m.rest     = assemble(all, "rx1");
    m.restUp   = assemble(all, "rx2");
    m.sitDown  = assemble(all, "si0");
    m.sit      = assemble(all, "si1");
    m.sitUp    = assemble(all, "si2");
    {
        // Flinches are single motions with any piece digit (dfm1, dfi6 ...); the class file's win when it has them.
        auto merged = all;
        for (const auto& [name, motion] : battle)
        {
            merged[name] = motion;
        }
        auto anyPiece = [&](const std::string& stem) {
            MotionSet set;
            for (char piece = '0'; piece <= '9'; ++piece)
            {
                if (const auto it = merged.find(stem + piece); it != merged.end())
                {
                    set.pieces.push_back(it->second);
                }
            }
            return set;
        };
        m.flinchFront     = anyPiece("dfm");
        m.flinchBack      = anyPiece("dbm");
        m.flinchFrontIdle = anyPiece("dfi");
        m.flinchBackIdle  = anyPiece("dbi");
    }
    {
        auto cast = [&](const char* p) -> CharacterMotions::Cast {
            return { assemble(all, std::string(p) + "0"), assemble(all, std::string(p) + "1"), assemble(all, std::string(p) + "2") };
        };
        const bool mob = !assemble(all, "ma0").empty() && assemble(all, "mb0").empty();
        m.casts        = mob ? std::array{ cast("ma"), cast("ma"), cast("ma") } : std::array{ cast("mw"), cast("mb"), cast("mi") };
    }
    // Draw / sheathe: the stems the class files' "in N" / "outN" schedulers play for the weapon's sheath style (its
    // `info` byte 4); a style without a scheduler, or naming a stem that is not there, falls back to style 0 (INFERRED),
    // then to the lowest in<letter> / ot<letter> stem.
    if (!battleFiles.empty())
    {
        for (const int style : { sheathStyle, 0 })
        {
            if (m.draw.empty())
            {
                if (const auto stem = sheathStem(index, battleFiles, true, style); !stem.empty())
                {
                    m.draw     = assemble(battle, stem);
                    m.drawStem = m.draw.empty() ? std::string() : stem;
                }
            }
            if (m.sheathe.empty())
            {
                if (const auto stem = sheathStem(index, battleFiles, false, style); !stem.empty())
                {
                    m.sheathe     = assemble(battle, stem);
                    m.sheatheStem = m.sheathe.empty() ? std::string() : stem;
                }
            }
        }
        for (char c = 'a'; c <= 'z' && (m.draw.empty() || m.sheathe.empty()); ++c)
        {
            if (m.draw.empty())
            {
                m.draw = assemble(battle, std::string("in") + c);
            }
            if (m.sheathe.empty())
            {
                m.sheathe = assemble(battle, std::string("ot") + c);
            }
        }
    }
    for (const char* stem : { "at0", "at1", "at2" })
    {
        if (auto set = assemble(battle, stem); !set.empty())
        {
            m.attacks.push_back(std::move(set));
        }
    }
    return m;
}

namespace
{

void sampleSet(const MotionSet* set, float frame, std::vector<Motion::BonePose>& pose)
{
    if (!set)
    {
        return;
    }
    for (const auto& piece : set->pieces)
    {
        piece.sample(frame, pose);
    }
}

void compose(const Skeleton& sk, const std::vector<Motion::BonePose>& pose, std::vector<RowMatrix>& out);

} // namespace

void poseSkeleton(const Skeleton& sk, const std::vector<const MotionSet*>& sets, float frame, std::vector<RowMatrix>& out)
{
    std::vector<Motion::BonePose> pose(sk.bones.size());
    for (const auto* set : sets)
    {
        sampleSet(set, frame, pose);
    }
    compose(sk, pose, out);
}

void poseSkeletonBlend(const Skeleton& sk, const MotionSet* from, float fromFrame, const MotionSet* to, float toFrame, float w, std::vector<RowMatrix>& out)
{
    std::vector<Motion::BonePose> a(sk.bones.size()), b(sk.bones.size());
    sampleSet(from, fromFrame, a);
    sampleSet(to, toFrame, b);
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (!a[i].driven && !b[i].driven)
        {
            continue;
        }
        // An undriven side contributes the identity motion (bind pose): rotation (0,0,0,1), translation 0.
        const bool aDriven = a[i].driven;
        auto       qa      = aDriven ? a[i].rotation : std::array<float, 4>{ 0, 0, 0, 1 };
        auto qb = b[i].driven ? b[i].rotation : std::array<float, 4>{ 0, 0, 0, 1 };
        float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
        if (dot < 0) // shortest path
        {
            for (auto& c : qb)
            {
                c = -c;
            }
        }
        std::array<float, 4> q{};
        float                len = 0;
        for (std::size_t k = 0; k < 4; ++k)
        {
            q[k] = qa[k] + (qb[k] - qa[k]) * w;
            len += q[k] * q[k];
        }
        len = std::sqrt(std::max(len, 1e-12f));
        for (auto& c : q)
        {
            c /= len;
        }
        a[i].driven   = true;
        a[i].rotation = q;
        for (std::size_t k = 0; k < 3; ++k)
        {
            const float ta      = aDriven ? a[i].translation[k] : 0.0f;
            a[i].translation[k] = ta + (b[i].translation[k] - ta) * w;
            const float sa      = aDriven ? a[i].scale[k] : 1.0f;
            const float sb      = b[i].driven ? b[i].scale[k] : 1.0f;
            a[i].scale[k]       = sa + (sb - sa) * w; // CODE 0x1001a529: the scale is lerped like the translation
        }
    }
    compose(sk, a, out);
}

int drawnWeaponBone(const Skeleton& sk, const MotionSet& battle, int bone, int gripPoint)
{
    if (bone < 0 || battle.empty() || static_cast<std::size_t>(gripPoint) >= sk.points.size() || static_cast<std::size_t>(bone) >= sk.bones.size())
    {
        return -1;
    }
    const int grip = sk.points[static_cast<std::size_t>(gripPoint)].bone;
    if (grip < 0 || static_cast<std::size_t>(grip) >= sk.bones.size())
    {
        return -1;
    }
    std::vector<RowMatrix> posed;
    poseSkeleton(sk, { &battle }, 0.0f, posed);
    auto gap = [&](int b) {
        const auto& m = posed[static_cast<std::size_t>(b)];
        const auto& g = posed[static_cast<std::size_t>(grip)];
        return std::hypot(m[12] - g[12], m[13] - g[13], m[14] - g[14]);
    };
    constexpr float kInGrip = 0.1f; // a slot the stance holds in the hand sits on the grip bone (0 .. 0.01 apart, DATA)
    if (gap(bone) < kInGrip)
    {
        return -1;
    }
    // Children slots (points 0x5F..0x6A: 8, 19, 5, 22, 10, 24, 3, 17 ...) first, as the meshes hang from those.
    for (const auto& range : { std::pair{ 0x5F, 0x6A }, std::pair{ 0x70, 0x7D } })
    {
        int   best = -1;
        float bestGap = kInGrip;
        for (int p = range.first; p <= range.second && static_cast<std::size_t>(p) < sk.points.size(); ++p)
        {
            const int b = sk.points[static_cast<std::size_t>(p)].bone;
            if (b > 0 && static_cast<std::size_t>(b) < sk.bones.size() && b != bone && gap(b) < bestGap)
            {
                best    = b;
                bestGap = gap(b);
            }
        }
        if (best >= 0)
        {
            return best;
        }
    }
    return -1;
}

namespace
{

void compose(const Skeleton& sk, const std::vector<Motion::BonePose>& pose, std::vector<RowMatrix>& out)
{
    // CODE 0x10034700 (mode 0, then mode 2): each bone's scale accumulates down the tree (root: its own), a bone's
    // translation is scaled by its parent's accumulated scale, and the bone's matrix is built from rotation and that
    // translation times the parent's *unscaled* matrix. Only then is every bone's matrix premultiplied by
    // Scale(its accumulated scale): the scale shrinks the bone's own vertices and its children's offsets, never the
    // children's axes (no shear). Undriven bones keep scale 1 (the per-frame reset, CODE 0x1001a350).
    out.resize(sk.bones.size());
    std::vector<RowMatrix>            plain(sk.bones.size());
    std::vector<std::array<float, 3>> accum(sk.bones.size(), { 1, 1, 1 });
    for (std::size_t i = 0; i < sk.bones.size(); ++i)
    {
        const auto&          b      = sk.bones[i];
        const std::size_t    parent = b.parent < i ? b.parent : 0;
        std::array<float, 4> q      = b.rotation;
        std::array<float, 3> t      = b.translation;
        std::array<float, 3> s{ 1, 1, 1 };
        if (pose[i].driven)
        {
            q = qmul(pose[i].rotation, b.rotation);
            for (std::size_t k = 0; k < 3; ++k)
            {
                t[k] += pose[i].translation[k];
                s[k] = pose[i].scale[k];
            }
        }
        for (std::size_t k = 0; k < 3; ++k)
        {
            accum[i][k] = i == 0 ? s[k] : accum[parent][k] * s[k];
            if (i != 0)
            {
                t[k] *= accum[parent][k];
            }
        }
        const auto local = rowFromQuatTrans(q, t);
        plain[i]         = i == 0 ? local : rowMul(local, plain[parent]);
    }
    for (std::size_t i = 0; i < sk.bones.size(); ++i)
    {
        out[i] = plain[i];
        for (std::size_t r = 0; r < 3; ++r)
        {
            for (std::size_t c = 0; c < 4; ++c)
            {
                out[i][r * 4 + c] *= accum[i][r];
            }
        }
    }
}

} // namespace

} // namespace xi::dat
