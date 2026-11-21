// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/character_anim.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace xi::gfx
{

namespace
{

// Frames a second: a timed motion (a weapon skill's op 0x05) is stretched to fill its duration (effect-format §S.3,
// CODE); the others run at their own speed.
float frameRate(const dat::MotionSet& s)
{
    if (s.duration > 0.0f)
    {
        return static_cast<float>(std::max<int>(s.frames() - 1, 1)) * 60.0f / s.duration;
    }
    return s.speed() * 30.0f;
}

} // namespace

AnimState animate(AnimTrack& a, const CharacterModel& cm, const AnimInput& in, float dt)
{
    const auto& m = cm.motions;
    if (!a.swingsKnown)
    {
        a.seenSwings  = in.swings;
        a.swingsKnown = true;
    }
    if (in.dead && !a.wasDead)
    {
        a.oneShot = !m.death.empty() ? &m.death : !m.corpse.empty() ? &m.corpse : nullptr;
    }
    else if (!in.dead && a.wasDead)
    {
        a.oneShot = nullptr;
    }
    a.wasDead = in.dead;
    if (in.engaged != a.wasEngaged && !in.dead)
    {
        const auto& drawOrSheathe = in.engaged ? m.draw : m.sheathe;
        if (!drawOrSheathe.empty())
        {
            a.oneShot = &drawOrSheathe;
        }
    }
    a.wasEngaged = in.engaged;
    if (!a.castsKnown)
    {
        a.seenCasts  = in.castsDone;
        a.castsKnown = true;
    }
    if (in.castsDone != a.seenCasts && !in.dead)
    {
        // Spell landed: release, then recover (school of the cast that just ended; default black).
        const auto& c = m.casts[a.lastSchool >= 0 ? a.lastSchool : 1];
        if (!c.release.empty())
        {
            a.oneShot = &c.release;
            a.chained = c.recover.empty() ? nullptr : &c.recover;
        }
    }
    a.seenCasts = in.castsDone;
    if (in.castSchool >= 0)
    {
        a.lastSchool = in.castSchool;
    }
    if (in.swings != a.seenSwings && !in.dead && !m.attacks.empty())
    {
        a.oneShot = &m.attacks[in.swings % m.attacks.size()];
    }
    a.seenSwings = in.swings;
    if (!a.wsKnown)
    {
        a.seenWs  = in.weaponSkills;
        a.wsKnown = true;
    }
    if (in.weaponSkills != a.seenWs && !in.dead && in.ws && !in.ws->empty())
    {
        // Weapon skill: its own motions in order (wind-up, then one per hit), instead of the swing.
        a.oneShot = &in.ws->front();
        a.chained = nullptr;
        a.seq     = in.ws;
        a.seqStep = 0;
        a.seqTime = 0.0f;
    }
    a.seenWs = in.weaponSkills;
    if (!a.emotesKnown)
    {
        a.seenEmotes  = in.emotes;
        a.emotesKnown = true;
    }
    if (in.emotes != a.seenEmotes && !in.dead && in.emote)
    {
        a.oneShot = in.emote; // the emote plays once, then the idle loop resumes
        a.chained = nullptr;
    }
    a.seenEmotes = in.emotes;
    const bool flinch = std::exchange(a.fxFlinch, false);
    if (flinch && !in.dead && in.castSchool < 0 && in.speed < 0.3f && !(in.emote && a.oneShot == in.emote))
    {
        // Damage flinch (effect-format §S.3 op 0x25): dfm / dbm engaged, dfi / dbi otherwise; front or back by
        // where the attacker stands. It cuts a swing short (monsters in melee are almost always mid-swing),
        // but not a cast, a run or an emote.
        const dat::MotionSet* f = in.engaged ? (in.hitFromBehind ? &m.flinchBack : &m.flinchFront)
                                             : (in.hitFromBehind ? &m.flinchBackIdle : &m.flinchFrontIdle);
        if (f->empty())
        {
            f = !m.flinchFront.empty() ? &m.flinchFront : &m.flinchFrontIdle;
        }
        a.oneShot = f->empty() ? nullptr : f;
        spdlog::debug("anim: flinch ({} pieces, {}, {})", f->pieces.size(), in.engaged ? "engaged" : "idle", in.hitFromBehind ? "behind" : "front");
    }
    if (in.resting != a.wasResting && !in.dead)
    {
        // Kneel down / stand up; the kneeling loop holds in between.
        const auto& edge = in.resting ? m.restDown : m.restUp;
        a.oneShot        = edge.empty() ? nullptr : &edge;
        a.standingUp     = !in.resting && a.oneShot != nullptr;
    }
    a.wasResting = in.resting;
    if (a.standingUp && a.oneShot != &m.restUp)
    {
        a.standingUp = false; // the stand-up finished (or something replaced it)
    }
    if (in.sitting != a.wasSitting && !in.dead)
    {
        const auto& edge = in.sitting ? m.sitDown : m.sitUp;
        a.oneShot        = edge.empty() ? nullptr : &edge;
    }
    a.wasSitting = in.sitting;

    // Engaged: the weapon-ready walk / run (dat::CharacterMotions::battleRun / battleWalk) when the class has them.
    const dat::MotionSet* strafe = in.moveDir == 1 ? &m.battleBack : in.moveDir == 2 ? &m.battleLeft : in.moveDir == 3 ? &m.battleRight : nullptr;
    const dat::MotionSet* run  = in.engaged && strafe && !strafe->empty() ? strafe
                               : in.engaged && !m.battleRun.empty() ? &m.battleRun
                               : in.stanceRun && !in.stanceRun->empty() ? in.stanceRun : &m.run;
    const dat::MotionSet* walk = in.engaged && !m.battleWalk.empty() ? &m.battleWalk
                               : in.stanceWalk && !in.stanceWalk->empty() ? in.stanceWalk : &m.walk;
    const dat::MotionSet* battle = in.stanceBattle && !in.stanceBattle->empty() ? in.stanceBattle : &m.battle;
    if (in.runVariant && !in.runVariant->empty())
    {
        run = in.runVariant;
    }
    if (in.walkVariant && !in.walkVariant->empty())
    {
        walk = in.walkVariant;
    }
    if (in.rideIdle && !in.rideIdle->empty())
    {
        run  = in.rideRun && !in.rideRun->empty() ? in.rideRun : in.rideIdle;
        walk = in.rideWalk && !in.rideWalk->empty() ? in.rideWalk : in.rideIdle;
    }
    const dat::MotionSet* loop = in.speed >= 3.5f && !run->empty() ? run
                               : in.speed >= 0.3f && !walk->empty() ? walk
                               : in.castSchool >= 0 && !m.casts[in.castSchool].loop.empty() ? &m.casts[in.castSchool].loop
                               : in.resting && !m.rest.empty()       ? &m.rest
                               : in.sitting && !m.sit.empty()        ? &m.sit
                               : in.engaged && !battle->empty()      ? battle
                               : in.idle && !in.idle->empty()        ? in.idle
                               : in.rideIdle && !in.rideIdle->empty() ? in.rideIdle
                                                                     : &m.idle;
    if (a.fxMotion && !in.dead)
    {
        a.oneShot = a.fxMotion; // job ability / item motions (cm0? -> cm1?, mi3? ...)
        a.chained = nullptr;
    }
    a.fxMotion = nullptr;
    if (a.oneShot && !in.dead && in.speed >= 0.3f)
    {
        a.oneShot = nullptr; // moving cancels a swing
    }
    if (a.seq)
    {
        if (a.seqStep >= a.seq->size() || a.oneShot != &(*a.seq)[a.seqStep])
        {
            a.seq = nullptr; // cancelled or replaced (moving, a flinch, death, an emote ...)
        }
        else
        {
            // The scheduler's clock: the next motion takes over at its tick, whether or not this one has ended.
            a.seqTime += dt * 60.0f;
            while (a.seqStep + 1 < a.seq->size() && (*a.seq)[a.seqStep + 1].at > 0.0f &&
                   a.seqTime >= (*a.seq)[a.seqStep + 1].at)
            {
                a.oneShot = &(*a.seq)[++a.seqStep];
            }
        }
    }
    const dat::MotionSet* set = a.oneShot ? a.oneShot : loop;
    if (set != a.set && in.trace)
    {
        auto label = [&](const dat::MotionSet* p) -> std::string {
            if (!p) return "none";
            if (p == &m.idle) return "idle";
            if (p == &m.battle) return "battle stance";
            if (p == &m.draw) return "draw";
            if (p == &m.sheathe) return "sheathe";
            if (p == &m.run) return "run";
            if (p == &m.walk) return "walk";
            if (p == &m.battleRun) return "battle run";
            if (p == &m.battleWalk) return "battle walk";
            if (p == &m.flinchFront || p == &m.flinchBack) return "flinch";
            if (p == &m.flinchFrontIdle || p == &m.flinchBackIdle) return "idle flinch";
            if (p == &m.rest) return "rest";
            if (p == &m.restDown) return "rest down";
            if (p == &m.restUp) return "rest up";
            if (p == &m.sit) return "sit";
            if (p == &m.sitDown) return "sit down";
            if (p == &m.sitUp) return "sit up";
            if (p == &m.death) return "death";
            if (p == &m.corpse) return "corpse";
            for (std::size_t i = 0; i < m.attacks.size(); ++i)
                if (p == &m.attacks[i]) return "attack " + std::to_string(i);
            if (p == in.idle) return "event idle";
            if (in.ws)
                for (std::size_t i = 0; i < in.ws->size(); ++i)
                    if (p == &(*in.ws)[i])
                        return "weapon skill " + std::to_string(i + 1) + "/" + std::to_string(in.ws->size());
            return p == a.fxMotion ? "effect motion" : "other";
        };
        spdlog::info("anim: {} -> {} (engaged {}, resting {}, sitting {}, oneShot {})", label(a.set), label(set), in.engaged, in.resting, in.sitting, a.oneShot != nullptr);
    }
    if (set != a.set)
    {
        // Cross-fade over 0.2 s from wherever the old motion was.
        a.prev      = a.set;
        a.prevFrame = a.frame;
        a.prevOnce  = a.setOnce;
        a.setOnce   = a.oneShot != nullptr;
        a.fade      = a.set ? 0.0f : 1.0f;
        a.fadeTime  = set->blendIn > 0.0f ? set->blendIn / 60.0f : 0.2f; // op 0x05 blend-in, ticks (INFERRED unit)
        a.set       = set;
        a.frame     = 0.0f;
    }
    a.frame += frameRate(*set) * dt;
    a.fxHold = std::max(0.0f, a.fxHold - dt);
    if (a.oneShot)
    {
        const float last = static_cast<float>(std::max<int>(set->frames() - 1, 0));
        if (a.frame >= last)
        {
            a.frame = last;
            if (a.seq && a.seqStep + 1 < a.seq->size())
            {
                // A weapon skill's motion ended before the next one's tick: hold this pose until then; an untimed
                // sequence (no scheduler ticks) goes straight on.
                if ((*a.seq)[a.seqStep + 1].at <= 0.0f)
                {
                    a.oneShot = &(*a.seq)[++a.seqStep];
                }
            }
            else if (!in.dead && a.fxHold <= 0.0f) // ops 0x2E / 0x2F keep it until their duration ends
            {
                a.oneShot = a.chained; // swing done (or release -> recover); the loop takes over after
                a.chained = nullptr;
                a.seq     = nullptr;
            }
        }
    }
    if (a.prev)
    {
        // A finished one-shot (a kneel, a swing) holds its last frame while it fades out; advancing it would wrap
        // it back to its first frame (standing) for the 0.2 s fade.
        a.prevFrame += frameRate(*a.prev) * dt;
        if (a.prevOnce)
        {
            a.prevFrame = std::min(a.prevFrame, static_cast<float>(std::max<int>(a.prev->frames() - 1, 0)));
        }
    }
    a.fade = std::min(1.0f, a.fade + dt / a.fadeTime);
    return { set->empty() ? nullptr : set, a.frame, a.prev && !a.prev->empty() ? a.prev : nullptr, a.prevFrame, a.fade };
}

} // namespace xi::gfx
