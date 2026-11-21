// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

// Coverage of the effect bytecode (docs/effect-format.md §S.3, §G.9-§G.11): for each list and op, whether xi-client
// carries it out. `xi-dat fxops` surveys the effect files against this table; the EffectSystem counts the ops it meets
// that are Unhandled (XI_FX_TRACE / the "fx: unhandled ops" summary). Header-only so tools can use it without the
// client library.
namespace xi::client
{

enum class FxList : uint8_t
{
    Scheduler, // type 7 records
    L0,        // generator, per frame
    L1,        // particle, at spawn
    L2,        // particle, per frame
    L3,        // particle, at expiry
};

enum class FxOpStatus : uint8_t
{
    Handled,   // carried out (possibly approximated; see the case's comment in effects.cpp)
    GameNoOp,  // the game has no handler either, or the op is only a label / marker (CODE jump tables)
    NoVisual,  // the game's handler changes nothing the client draws or plays (state for systems not built)
    Unhandled, // does something in the game that xi-client does not do
};

constexpr std::string_view fxListName(FxList l)
{
    switch (l)
    {
        case FxList::Scheduler: return "sched";
        case FxList::L0: return "L0";
        case FxList::L1: return "L1";
        case FxList::L2: return "L2";
        case FxList::L3: return "L3";
    }
    return "?";
}

constexpr FxOpStatus fxOpStatus(FxList list, uint8_t op)
{
    using S = FxOpStatus;
    switch (list)
    {
        case FxList::Scheduler:
            switch (op)
            {
                // Flow, generators, sub-schedulers, the expression VM (0x69 / 0x6A are its block markers).
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x09: case 0x1E: case 0x24: case 0x2D: case 0x30:
                case 0x31: case 0x32: case 0x3C: case 0x3D: case 0x52: case 0x57: case 0x5D: case 0x5F: case 0x64:
                case 0x60: // a named Sep sound (cutscenes)
                case 0x65: case 0x67: case 0x69: case 0x6A: case 0x6B: case 0x73: case 0x76: case 0x77: case 0x85:
                case 0xAF:
                // Motions, flinches, guards, tints, camera, screen colour, doors, sounds, results, trails.
                case 0x04: case 0x05: case 0x06: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0F: case 0x21:
                case 0x25: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x46: case 0x47: case 0x5A: case 0x5B:
                // Dolls, generator swap, sound at the nearest target, result commit with a kind.
                case 0x15: case 0x16: case 0x17: case 0x18: case 0x22: case 0x23: case 0x3F: case 0x53: case 0x71:
                // The stance motion again (CODE 0x1005aff8 -> 0x100c8ae0: idle / battle / rest by state): the client's own
                // motion loop already returns there.
                case 0x28:
                // A monster's weapon variants: show / hide model part wepN.
                case 0x75: case 0xA3:
                // Captured-image layers (0x0E zoom, 0x10 freeze frame), motion hold, sub-scheduler from the caster's
                // files, the caster's "0a.." sound.
                case 0x0E: case 0x10: case 0x2E: case 0x2F: case 0x3B: case 0x78:
                // Part 7B: file 2800 + n's `main`, kept screen colours (0x51 dim / 0x72 add), light scale drives, an
                // actor held at another's point, weather (group / alternate table / one record), slot sounds, the
                // weapon in the grip, footsteps, a drawn doll of the target, target colours, run / walk / death
                // variants, the music level, stopping a camera, the filtered sound, weapon parts, stance holds,
                // turning, knock-back motions.
                case 0x19: case 0x51: case 0x72: case 0x6F: case 0x70: case 0x7A: case 0x7C: case 0xAE: case 0x7E:
                case 0x8A: case 0x8B: case 0x9B: case 0xA7: case 0xA8: case 0xBC: case 0x9D: case 0xA1: case 0xB2:
                case 0xA4: case 0xA5: case 0xAC: case 0xA6: case 0xB0: case 0xB3: case 0xB6: case 0xBA: case 0xB9:
                case 0x62: case 0xBF:
                // Carried out outside the scheduler: lifts (0x1D, client::Doors), ship legs and turns (0x27, 0xA9,
                // client::Ships).
                case 0x1D: case 0x27: case 0xA9:
                    return S::Handled;
                // Part 7A (effects_actor.cpp): texture overlay and its scroll, the player-only sound, counter, knockback,
                // motion sets, draw / sheathe, head look-at off, model generators off, attach points; the look of
                // effects-only NPC model files (0x8D-0x96, 0x98, 0x99: dat::effectLook).
                case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x54: case 0x55: case 0x4A:
                case 0x5C: case 0x5E: case 0x79: case 0x84: case 0x89: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
                case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x98: case 0x99:
                case 0x9A: case 0x9E: case 0x9F:
                // The screen-refraction pass of a fading actor (SkinnedRenderer::drawRefraction).
                case 0x48: case 0x49:
                // The caster walks along its facing (part 7E; drawn with the knockback's offset).
                case 0x6E:
                    return S::Handled;
                // 0x86 / 0xBE set model flags nothing reads (+0x128 bit 7, +0x13c bit 18); 0x88 resets the caster's
                // colour only when its task is deleted early (CODE 0x1005f320).
                case 0x86: case 0x88: case 0xBE:
                // 0x7F drives actor +0x668 (vfunc 0x34 / 0x38 = 0x100a4790 / 0x100a47a0, constructor default 0x80 at
                // 0x100c6046): the only caller of the getter in the actor, scheduler and draw code is the 0x7F task itself
                // (0x10061355); the other +0x668 accesses belong to a class whose +0x668 is a pointer (0x100ac96e ..).
                case 0x7F:
                // Part 7B, state xi-client has no use for: the area environment set and the Vana'diel clock, only in the
                // title demo files 23-26 (0x7B; 0x7D is overwritten by the zone's clock sync 0x1018c1c0); entity +0x130
                // bit 4 (0xAB: re-apply the motion names on refresh, 0x1008fc52); anchor bones and height offset for
                // the camera / name / arrow (0xAD +0x8a8, 0xBD +0x8a9, 0xB4 +0x8c0); no idle fidgets (0xB5 +0x8aa,
                // 0x100cdef5); actor +0x88 bit 7 (0xB8, no reader found); entity +0x13c bit 20 (0xC2: where one op
                // looks up its scheduler, 0x1005d926; xi-client already falls back to the caster's files).
                case 0x7B: case 0x7D: case 0xAB: case 0xAD: case 0xBD: case 0xB4: case 0xB5: case 0xB8: case 0xC2:
                    return S::NoVisual;
                // Busy counters on the actor's entity (CODE 0x100a4070: +0x11c, 30 s timeout, read by 0x10083250 /
                // 0x10095780) or the actor (0x59: +0x80 via vtable +0x160 / +0x164), for the op's duration: they gate
                // state changes (server updates, idle choices), nothing is drawn.
                case 0x1F: case 0x20: case 0x56: case 0x59:
                // The same actor busy count (vtable +0x160 at the start, CODE 0x10060710) on the caster (0x07) or the
                // target (0x08) for the duration (CODE 0x1005a1b0 / 0x1005a1fd).
                case 0x07: case 0x08:
                    return S::NoVisual;
                // No handler in the game's jump table 0x1005db4c (§S.3).
                case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3E:
                case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x61: case 0x63: case 0x66:
                case 0x68: case 0x6C: case 0x6D: case 0x74: case 0xA2:
                    return S::GameNoOp;
                default: return S::Unhandled;
            }
        case FxList::L0:
            switch (op)
            {
                case 0x04: case 0x0A: case 0x11: return S::Handled; // 0x04: emission rate keyframe
                case 0x15: return S::Handled;                        // visibility box: no tick while out of view
                // Keyframes written into L1 records: radial speed, ring radius / angles, spawn position, velocity.
                case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: case 0x0C: case 0x0D: case 0x12:
                case 0x13: case 0x14:
                    return S::Handled;
                // Rotation about X / Y / Z by a keyframe (after a fresh attach matrix); emission rate by the time of day.
                case 0x0E: case 0x0F: case 0x10: case 0x17:
                    return S::Handled;
                case 0x5D: return S::GameNoOp; // Gen_Tick's default case
                // Outside the jump table's 4..0x17 (CODE 0x100496cb): the default case.
                default: return op < 0x04 || op > 0x17 ? S::GameNoOp : S::Unhandled;
            }
        case FxList::L1:
            switch (op)
            {
                case 0x01: case 0x02: case 0x03: case 0x06: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
                case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x16: case 0x17: case 0x18: case 0x19:
                case 0x1D: case 0x1E: case 0x1F: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
                case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                case 0x30: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x3B: case 0x41: case 0x54:
                case 0x58: case 0x60: case 0x61: case 0x62: case 0x63: case 0x6C:
                // Ellipsoid spawn, colour-velocity jitter, the wobble set-up, actor-size draw key, child generators.
                case 0x07: case 0x1A: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x72: case 0x3C: case 0x44: case 0x45:
                case 0x53: case 0x6A:
                // Random slot, `dist` strength, `ring` shape, projected-pass blend, parent rotation / scale, sound
                // distances, the environment overlay and its angle curves, the sound level curve, vibration, heading.
                case 0x31: case 0x32: case 0x3A: case 0x43: case 0x47: case 0x48: case 0x49: case 0x4C: case 0x55:
                case 0x59: case 0x5A: case 0x5B: case 0x68: case 0x82: case 0x87:
                // Spawn on a Vtx point (random / in turn), converge (start at the end, fly back).
                case 0x4E: case 0x4F: case 0x67:
                // Colour x the light at the actor (0x8f) or in the scene (0x90).
                case 0x8F: case 0x90:
                // The lens flare's glare colour; a sound particle placed at another generator's particle.
                case 0x4B: case 0x6B:
                // Onto the ground; from the parent particle: velocity, texture offset, rotation / scale added, a point
                // light's intensity / range, life and wobble, draw key, its generator's Vtx; attach offsets; sound
                // particle distance modes and keep-alive; footprints at the actor; a cleared resource slot; placed at
                // the parent particle (as every child is here).
                case 0x42: case 0x46: case 0x4A: case 0x79: case 0x7A: case 0x7B: case 0x7E: case 0x7F: case 0x89:
                case 0x8A: case 0x8E: case 0x92: case 0x94: case 0x98: case 0x9B: case 0x9D: case 0x9E:
                    return S::Handled;
                // Part 7C: keyframe slots of paired L2 ops (rate / time-of-day / position / overlay colour / point light
                // / `dist` curves), the colour kept for the scene-light tint, velocity curves, the drag curve.
                case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x64: case 0x65: case 0x66: case 0x6D:
                case 0x6E: case 0x6F: case 0x70: case 0x80: case 0x81: case 0x83: case 0x84: case 0x85: case 0x8B:
                case 0x8C: case 0x8D: case 0x95: case 0x96: case 0x97: case 0x39: case 0x5C: case 0x5D: case 0x5E:
                case 0x5F: case 0x7C: case 0x7D: case 0x91: case 0x50: case 0x51: case 0x52: case 0x69:
                    return S::Handled;
                // A curve slot for L2 0x6a (CODE 0x10050aa0), whose value nothing draws (see L2 0x6a).
                case 0x93:
                // p+0x160 = a light generator's name (0x88) or the parent's (0x9c), read by nothing (CODE 0x1005149d /
                // 0x100514ab); class 0x23 `vlin` fields (0x57, CODE 0x10051a4a), whose draw is empty (0x100640b0).
                case 0x88: case 0x9C: case 0x57:
                    return S::NoVisual;
                // The burst count of the line / strip particle classes 8, 9, 0xa, 0xd (CODE 0x10051871: +0x44 = the
                // generator's flags & 0x1ff); for any other class it does nothing.
                case 0x56:
                    return S::NoVisual;
                // Past the jump table (op - 1 > 0x9d, CODE 0x1004e857): the default case (DATA: file 6068's one
                // 0xff record is a misparse of a non-effect file).
                default: return op > 0x9E ? S::GameNoOp : S::Unhandled;
            }
        case FxList::L2:
            switch (op)
            {
                case 0x02: case 0x03: case 0x05: case 0x06: case 0x08: case 0x09: case 0x0B: case 0x0D: case 0x0E:
                case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
                case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20:
                case 0x21: case 0x22: case 0x27: case 0x28: case 0x2C: case 0x2E: case 0x34: case 0x3C: case 0x3D:
                case 0x3E: case 0x3F: case 0x45: case 0x49:
                // Colour-velocity acceleration, vector turn, sine wobble; child generators tick with their particle.
                case 0x0C: case 0x26: case 0x29: case 0x2A: case 0x2B: case 0x25: case 0x33: case 0x46:
                // Overlay angles, sound level, weather weight, vibration / shake, screen flash, banking.
                case 0x35: case 0x36: case 0x37: case 0x43: case 0x50: case 0x5F: case 0x60: case 0x64:
                // Day colour tables: the weekday's (0x4e) and the moon phase's (0x4f).
                case 0x4E: case 0x4F:
                // Visibility (hidden when unseen), spin and size by view depth (lamp glows).
                case 0x53: case 0x59: case 0x5A:
                // Part 7C: scene-light tint, scroll / spin rate curves, time-of-day scale / colour / spin / rotation /
                // position, `dist` strength, overlay colour, point light curves, velocity curves, the drag curve.
                case 0x69: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x61: case 0x62: case 0x63:
                case 0x66: case 0x67: case 0x68: case 0x40: case 0x41: case 0x42: case 0x4A: case 0x4B: case 0x4C:
                case 0x4D: case 0x6B: case 0x6C: case 0x6D: case 0x24: case 0x38: case 0x39: case 0x3A: case 0x3B:
                case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x30: case 0x31: case 0x32: case 0x44:
                // Part 7D: fade by a distance band (0x48, with 0x2e's handler), its weight into D3b morph weights
                // (0x6e); the velocity turned along the particle's rotation (0x2f).
                case 0x48: case 0x6E: case 0x2F:
                    return S::Handled;
                // Writes a curve value into the L2 0x65 record's +0xC (CODE 0x1004bf8c); the only other readers of 0x65
                // records are the linker (0x10048eef: +4 = scheduler named +8) and L1 0x93, so nothing uses it.
                case 0x6A:
                    return S::NoVisual;
                // The jump table's "next record" entry (CODE 0x1004b7ae), or past it (op - 1 > 0x6d).
                case 0x23: case 0x47: case 0x65:
                    return S::GameNoOp;
                default: return op > 0x6E ? S::GameNoOp : S::Unhandled;
            }
        case FxList::L3:
            switch (op)
            {
                case 0x01: case 0x05: return S::Handled;
                // Release the object in the dying particle's slot (rec bits 13..18; a `line` trail or sound: refcount
                // down, deleted at zero; CODE 0x1004e6da -> 0x10063a50). xi-client's particles own nothing to release.
                case 0x04: return S::NoVisual;
                case 0x07: return S::GameNoOp; // the default case of 0x1004e650
                default: return S::Unhandled;
            }
    }
    return S::Unhandled;
}

} // namespace xi::client
