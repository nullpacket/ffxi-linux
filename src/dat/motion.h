// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/character.h"
#include "dat/dat_index.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xi::dat
{

// Mo2 motion (docs/dat-format.md §10.4): per-bone tracks; every component is a constant or an F-frame curve.
class Motion
{
public:
    static Motion parse(std::span<const std::byte> payload);

    [[nodiscard]] uint16_t frames() const
    {
        return frames_;
    }
    // Frames advanced per animation tick (client: 30 ticks/s, INFERRED).
    [[nodiscard]] float speed() const
    {
        return speed_;
    }

    struct BonePose
    {
        bool                 driven = false;
        std::array<float, 4> rotation{ 0, 0, 0, 1 }; // quaternion x, y, z, w
        std::array<float, 3> translation{};
        std::array<float, 3> scale{ 1, 1, 1 }; // dat-format §10.4: applied by poseSkeleton (CODE 0x10034700)
    };
    // Writes the motion's contribution for every bone it drives (looping, linear between frames).
    void sample(float frame, std::vector<BonePose>& out) const;

private:
    struct Track
    {
        uint32_t                bone = 0;
        bool                    driven = true;
        std::array<uint32_t, 10> index{}; // qx qy qz qw tx ty tz sx sy sz
        std::array<float, 10>    value{}; // defaults
    };
    uint16_t           frames_ = 1;
    float              speed_  = 0.25f;
    std::vector<Track> tracks_;
    std::vector<float> keys_; // payload floats from +0x0A (key index k -> keys_[k])
};

// One named motion (e.g. "idl") assembled from its pieces <name>0/1/2 across the given files (§10.4).
struct MotionSet
{
    std::vector<Motion> pieces;
    // Timing within a weapon skill / TP move (its "main" scheduler's op 0x05, effect-format §S.3), in scheduler ticks
    // (60 a second): the tick it starts at, the duration it is stretched to fill, and its blend-in. 0 = untimed.
    float              at       = 0.0f;
    float              duration = 0.0f;
    float              blendIn  = 0.0f;
    [[nodiscard]] bool empty() const
    {
        return pieces.empty();
    }
    [[nodiscard]] float speed() const
    {
        return pieces.empty() ? 0.25f : pieces.front().speed();
    }
    [[nodiscard]] uint16_t frames() const
    {
        return pieces.empty() ? 1 : pieces.front().frames();
    }
};

MotionSet loadMotionSet(const DatIndex& index, const std::vector<uint32_t>& files, const std::string& stem);

// The motions a character uses: locomotion (idl / wlk / run), battle stance (btl), death (ded, played once) and
// corpse (cor), and melee swings (at0..at2). Mob model files hold all of them; PCs take btl/at* from their
// weapon class files.
struct CharacterMotions
{
    MotionSet              idle, walk, run, battle, death, corpse;
    // Moving while engaged: the race's walk / run with the weapon class file's pieces (run1 / wlk1, the upper body
    // holding the weapon) in place of the base ones (DATA: class files 9673.. hold run1 wlk1 amf1 amb1 aml1 amr1).
    MotionSet              battleWalk, battleRun;
    // Engaged and moving back / left / right while facing the target: the base mvb / mvl / mvr (legs) with the class
    // file's amb1 / aml1 / amr1 upper body (INFERRED from the names).
    MotionSet              battleBack, battleLeft, battleRight;
    MotionSet              draw, sheathe; // PCs: in? / ot? from the weapon class files (the letter is the sheath style)
    std::string            drawStem, sheatheStem; // which ("inc", "otd" ...)
    // Casting by school (0 white mw?, 1 black mb?, 2 other mi?; mobs use ma? for all): <x>0 loops while casting,
    // <x>1 releases the spell and <x>2 recovers (§10.6, from renders).
    struct Cast
    {
        MotionSet loop, release, recover;
    };
    std::array<Cast, 3> casts;
    // Resting (/heal, status 33): sh0 kneels down, sh1 loops, sh2 stands up (INFERRED from the names and renders).
    MotionSet restDown, rest, restUp;
    // Sitting (/sit status 47, /sitchair 63+): si0 sits down, si1 loops, si2 stands (INFERRED, the same pattern).
    MotionSet sitDown, sit, sitUp;
    // Damage flinch (effect-format §S.3 op 0x25): front / back, engaged (dfm / dbm) or not (dfi / dbi).
    MotionSet flinchFront, flinchBack, flinchFrontIdle, flinchBackIdle;
    std::vector<MotionSet> attacks;
};
// A PC weapon skill's motions (§10.8): the animation id from the 0x028 result picks two per-race files (fileA = race base
// + id for pieces 0/1, fileB = fileA + 256 for piece 2); the "main" scheduler in fileA plays its stems in order, each
// at its own tick (wind-up, then one motion per hit: Combo = a20 a22 a02 a52). Empty when the race or files are unknown.
std::vector<MotionSet> loadWeaponSkillMotions(const DatIndex& index, uint8_t race, uint16_t animation);

// An emote's motion (dat-format §10.9): race base file + 2984 + group holds pieces 0/1, + 64 more piece 2. The group
// follows LSB's emote ids (DATA); the stem within the group is INFERRED from its name (bow, kne, wav, ...). Empty for
// emotes without a known stem (face-only emotes such as /smile, and the unmatched ones).
MotionSet loadEmoteMotion(const DatIndex& index, uint8_t race, uint8_t emote);

// A monster TP move's motions (§10.8): effect file 3900 + animation id (49647 / 59193 / 84743 + the remainder from
// 0x200 / 0x600 / 0x800), whose "main" scheduler names stems of the monster's own motions ("ma0?") or full motion
// names stored in the effect file.
std::vector<MotionSet> loadMonsterSkillMotions(const DatIndex& index, uint16_t animation, const std::vector<uint32_t>& modelMotionFiles);

// `battleFiles` (PCs: the weapon class files, §10.6) supply btl / at* when present; `files` supply the rest.
// `sheathStyle`: the main weapon's `info` byte 4, which picks the draw / sheathe motions (§10.6).
CharacterMotions loadCharacterMotions(const DatIndex& index, const std::vector<uint32_t>& files, const std::vector<uint32_t>& battleFiles = {},
                                      int sheathStyle = 0);

// Bone -> model matrices (row-vector D3D form) for a skeleton posed by motions at `frame`.
// Per bone: rotation = motion (x) bind (Hamilton, bind first), translation = bind + motion (client 0x10034560).
void poseSkeleton(const Skeleton& skeleton, const std::vector<const MotionSet*>& sets, float frame, std::vector<RowMatrix>& out);

// Cross-fade between two motions: weight 0 = `from` at fromFrame, 1 = `to` at toFrame (per-bone nlerp / lerp).
void poseSkeletonBlend(const Skeleton& skeleton, const MotionSet* from, float fromFrame, const MotionSet* to, float toFrame, float weight,
                       std::vector<RowMatrix>& out);

// The attachment bone a drawn weapon follows instead of its sheath bone `bone` (dat-format §10.6). The weapon meshes
// hang from their sheath slot (5 for Hume swords, 10 for daggers, 17 for great swords ...), and a weapon class's battle
// stance puts only some slots in the grip: Hume swords' class moves bones 3..11 there, so a sword stays in hand, but
// the dagger class holds bones 16 / 17 in the grip and leaves 9 / 10 at the hip (DATA). Returns the attachment bone
// (Sk2 points 0x5F..0x6A, then 0x70..0x7D) that `battle` frame 0 puts at the grip (point `gripPoint`, 0x7F = the
// main hand), or -1 when `bone` is already there or nothing is. INFERRED: the game's own switch is not traced; this
// reproduces where the battle motions hold a weapon.
int drawnWeaponBone(const Skeleton& skeleton, const MotionSet& battle, int bone, int gripPoint = 0x7F);

} // namespace xi::dat
