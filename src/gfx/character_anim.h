// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/character_model.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// Picks and advances a character's motion each frame (dat-format §10): locomotion, battle stance, draw/sheathe,
// swings, casting, weapon skills, emotes, rest/sit, flinches, death, and effect-requested motions.
namespace xi::gfx
{

// Per-entity locomotion animation: speed estimated from the displayed position.
struct AnimTrack
{
    glm::vec3             lastPos{ 0.0f };
    float                 speed = 0.0f;
    float                 frame = 0.0f;
    const dat::MotionSet* set   = nullptr;
    const dat::MotionSet* prev  = nullptr; // cross-fade source
    bool                  setOnce  = false;   // `set` plays once (a one-shot), so it holds its last frame
    bool                  prevOnce = false;   // ... and `prev` did: it stays on its last frame while fading out
    float                 prevFrame = 0.0f;
    float                 fade      = 1.0f;
    const dat::MotionSet* oneShot   = nullptr; // swing / death playing once
    const dat::MotionSet* fxMotion  = nullptr; // asked for by an effect scheduler (ops 0x05 / 0x06)
    bool                  fxFlinch  = false;   // an effect's damage flinch (ops 0x21 / 0x25)
    float                 fxHold    = 0.0f;    // effect ops 0x2E / 0x2F: seconds the current one-shot stays on its last frame
    uint64_t              drawnTick = 0;       // the client frame this track was last posed (event motion waits skip undrawn actors)
    uint32_t              seenSwings = 0;
    bool                  swingsKnown = false;
    bool                  wasDead     = false;
    bool                  wasEngaged  = false;
    bool                  wasResting  = false;
    bool                  standingUp  = false; // the rest's stand-up one-shot is playing (no walking until it ends)
    bool                  wasSitting  = false;
    uint32_t              seenWs      = 0;
    bool                  wsKnown     = false;
    // The weapon skill / TP move playing: its motions in order (wind-up, one per hit ...), the one showing, and the
    // scheduler ticks since it started (each motion starts at its own tick, MotionSet::at).
    const std::vector<dat::MotionSet>* seq         = nullptr;
    std::size_t                        seqStep     = 0;
    float                              seqTime     = 0.0f;
    float                              fadeTime    = 0.2f; // the current cross-fade's length, seconds
    uint32_t              seenEmotes  = 0;
    bool                  emotesKnown = false;
    const dat::MotionSet* chained     = nullptr; // one-shot to play after the current one (cast recover)
    // Config animation rate (cfg 64): a skipped frame holds this pose and banks its time for the next update.
    AnimState             held{};
    bool                  heldValid = false;
    float                 heldTime  = 0.0f;
    ClothSim              cloth;       // Os2 class-1 soft bodies (gfx/cloth.h)
    int                   lastSchool  = -1;
    uint32_t              seenCasts   = 0;
    bool                  castsKnown  = false;
};
struct AnimInput
{
    float    speed   = 0.0f; // ground speed, yalms/s
    bool     engaged = false;
    bool     dead    = false;
    uint32_t swings  = 0;
    int      castSchool = -1; // casting: 0 white, 1 black, 2 other; -1 = not casting
    uint32_t castsDone  = 0;
    bool     resting    = false; // status 33 (/heal)
    uint32_t weaponSkills = 0;           // weapon skills performed (per change: play `ws`)
    const std::vector<dat::MotionSet>* ws = nullptr; // the latest weapon skill's motions (wind-up, strike...)
    uint32_t emotes = 0;                   // emotes performed (per change: play `emote`)
    const dat::MotionSet* emote = nullptr; // the latest emote's motion
    int      moveDir = 0;                  // engaged movement: 0 forward, 1 back, 2 left, 3 right
    bool     sitting = false;              // status 47 (/sit) or 63-83 (/sitchair)
    bool     hitFromBehind = false;
    const dat::MotionSet* idle = nullptr; // an event script's idle motion (op 0x5E), instead of the normal one
    // Motion sets an effect chose (effect ops 0x79 battle, 0xA4 run, 0xA5 walk), instead of the model's own.
    const dat::MotionSet* stanceBattle = nullptr;
    const dat::MotionSet* stanceWalk   = nullptr;
    const dat::MotionSet* stanceRun    = nullptr;
    // Riding (chocobo_re.md §3d): the rider's chi / wlk / run instead of idle / walk / run.
    const dat::MotionSet* rideIdle = nullptr;
    const dat::MotionSet* rideWalk = nullptr;
    const dat::MotionSet* rideRun  = nullptr;
    // An effect's run / walk variant (scheduler ops 0xA4 / 0xA5: `1un` .. `3un`, `1lk` .. `3lk`) instead of run / wlk.
    const dat::MotionSet* runVariant  = nullptr;
    const dat::MotionSet* walkVariant = nullptr;
    bool                  trace    = false; // log motion changes (XI_ANIM_TRACE, our own character)
};

// Whether the weapon is in the hand: engaged, switching halfway through the draw / sheathe motion (the class files'
// draw schedulers mark their midpoint, op 0x1F at 36 of 72 ticks; INFERRED as the moment the weapon changes hands).
inline bool weaponDrawn(const AnimTrack& a, const dat::CharacterMotions& m)
{
    if (a.oneShot && a.oneShot == a.set && (a.oneShot == &m.draw || a.oneShot == &m.sheathe))
    {
        const bool past = a.frame >= 0.5f * static_cast<float>(a.oneShot->frames());
        return a.oneShot == &m.draw ? past : !past;
    }
    return a.wasEngaged && !a.wasDead;
}


AnimState animate(AnimTrack& a, const CharacterModel& cm, const AnimInput& in, float dt);

} // namespace xi::gfx
