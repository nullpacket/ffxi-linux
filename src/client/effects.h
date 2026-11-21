// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"
#include "dat/effect.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The effect runtime (docs/effect-format.md): schedulers (type 7) start generators (type 5), generators spawn
// particles that draw D3m / D3a meshes. Everything here is CPU-side, in game space (Y down); the renderer turns the
// batches into draws. One tick = 1/60 s (INFERRED, §G.15).
namespace xi::client
{

// Where an actor is, for attach points (game space, Y down).
struct FxActor
{
    glm::vec3 pos{ 0.0f };
    float     yaw    = 0.0f; // a = -dir * pi / 128: facing (cos a, 0, sin a), as the character models
    float     height = 1.8f;
    // Skeleton attach point (effect-format §C.6) in game space; empty for actors without a posed skeleton.
    std::function<std::optional<glm::vec3>(uint32_t id)> point;
    // The actor's size for the generators' size scaling (effect-format §G.4): x = actor vtable +0x98 (width), y = +0x9c
    // (height). See fxActorSize.
    glm::vec2 size{ 1.0f };
};
using FxActorLookup = std::function<std::optional<FxActor>(uint32_t id)>;

// An actor's effect size from its skeleton's bounds set 0 {top, bottom, x max, x min, ...} (Y down) and its model
// scale (CODE 0x100d0810 / 0x100d08e0, actor vtable +0x98 / +0x9c): width (x max - x min) and height (bottom - top),
// each x scale / 1.7, so a 1.7-tall model is size 1. No bounds: 1 (as the game without a skeleton).
inline glm::vec2 fxActorSize(const std::array<float, 6>& bounds, float scale)
{
    const float h = bounds[1] - bounds[0], w = bounds[2] - bounds[3];
    if (!(h > 0.0f) && !(w > 0.0f))
    {
        return glm::vec2(1.0f);
    }
    constexpr float kRef = 0.58823526f; // 1 / 1.7 (DATA 0x10331394)
    return glm::vec2(w, h) * scale * kRef;
}

// Variables the expression VM reads (effect-format §S.5): 0x28-0x36 the current 0x028 result (0x28 resolution,
// 0x2A animation, 0x2B info, 0x2F added effect, 0x33 spikes ...), 0x38-0x3E actor state bits. Unset = 0.
using FxVars = std::array<int32_t, 0x40>;
// A battle action as the schedulers see it: every (target, result) pair in order. Op 0x32 steps through them
// (effect-format §S.3), op 0x30 runs once per target; the current pair gives the target and the VM variables.
struct FxAction
{
    struct Result
    {
        uint32_t target = 0;
        FxVars   vars{};
    };
    std::vector<Result> results;
    uint32_t            actor = 0; // who acts (var 0x38)
    uint64_t            tag   = 0; // action serial << 16; op 0x2B reports tag | (result + 1) (ChatLine::hold)
};

// A motion a scheduler asks an actor to play (ops 0x05 caster / 0x06 target): `stem` names the pieces
// <stem>0/1/2 (`cm0?` -> "cm0"), over `seconds`.
struct FxMotion
{
    uint32_t    actor = 0;
    std::string stem;
    float       seconds = 0;
    bool        flinch  = false; // ops 0x21 / 0x25: a damage flinch (the character picks dfi / dbi ...), no stem
    bool        guard   = false; // op 0x5A / 0x5B: guard / parry motion, gdi? idle or gdm? engaged; no stem
    bool        event   = false; // asked for by an event script (client::Cutscene): always plays
    int         file    = -1;    // event motions: the motion DAT to look in first (-1 = the actor's own files)
    float       hold    = 0;     // ops 0x2E / 0x2F: keep the current motion this many seconds (no stem)
    bool        knockback = false; // op 0x5E: `bf0` / `bb0` while sliding (plays for any action type)
};

// A door part moving (ops 0x0C translation / 0x0D rotation, CODE handlers 0x1005a0ee / 0x1005a14f, tick 0x10043a30):
// part `part` of the caster's door goes linearly from where it is to `target` over `seconds`.
struct FxPartMove
{
    uint32_t  actor    = 0;
    uint32_t  part     = 0;
    bool      rotation = true; // radians about the part's local X / Y / Z; false = offset
    glm::vec3 target{ 0.0f };
    float     seconds = 0.0f;
};

// A sound a scheduler starts (ops 0x0A at the caster, 0x0B at the target): the Sep chunk's payload (an SeSep
// program; it stays valid while the EffectSystem lives) and the audible distance (record +0x14, DATA 30 / 60).
struct FxSound
{
    uint32_t                   actor = 0;
    dat::FxName                name  = 0;
    std::span<const std::byte> sep;
    float                      near = 3.0f, far = 30.0f; // record +0x18 / +0x14; 0 = these defaults (CODE 0x10036ba0)
    std::optional<glm::vec3>   at;                       // a world position (game space) instead of an actor's
    float                      gain = 1.0f;              // sound particles: L1 0x68's level at the start
    // Sound particles (CODE 0x10036ba0, p+0x1e9): how the distance is measured: 0 the vertical part x3 (the default),
    // 1 plain (L1 0x4c / 0x92), 2 across the ground only (L1 0x9e). p+0x1e8 (L1 0x8a): out of range it keeps playing
    // at volume 1 / 127 instead of going silent.
    uint8_t                    distanceMode = 0;
    bool                       keepFar      = false;
    uint32_t                   slot = 0;                 // op 0x8A: slot + 1, until op 0x8B stops it (takeSoundStops)
};

// A zone point light (generator resource type 0x47, `mode/pl`; effect-format §Z): keyed by the generator's name,
// which the zone's Mzb light table lists. `colour` already holds colour x intensity (0 = off).
struct FxPointLight
{
    dat::FxName key = 0;
    glm::vec3   pos{ 0.0f }; // game space
    float       range = 0.0f;
    glm::vec3   colour{ 0.0f };
};

// The cutscene camera (scheduler op 0x04 running a Camera chunk; event-format §6), game space. `zoom` is the game's
// projection scale (350 = the normal view; larger = narrower; tan(fovY/2) = 192 / zoom, CODE 0x10015450).
struct FxCameraPose
{
    glm::vec3 eye{ 0.0f }, at{ 0.0f };
    float     zoom = 350.0f, roll = 0.0f;
};

class EffectSystem
{
public:
    explicit EffectSystem(const dat::DatIndex& index);

    // Starts scheduler `name` from DAT `fileId` (its own blocks first, then the common effect file 0) for
    // caster / target entity ids. False when the file or the scheduler is missing.
    // `delaySeconds` holds the scheduler back (a melee spark waits for the swing's contact).
    bool play(uint32_t fileId, std::string_view name, uint32_t caster, uint32_t target, float delaySeconds = 0.0f,
              const FxAction* action = nullptr);
    // A damage / recovery / miss pop-up (Config damage display): scheduler `name` (`damv`, `curv`, `misv`) of file
    // `fileId` (27) from caster to target, its text particles showing `value` (or "Miss!" for misv) in `colour`
    // (0xRRGGBB) unless the particle has its own colour (CODE 0x1009e1b3, 0x10041e70).
    bool playNumber(uint32_t fileId, std::string_view name, uint32_t caster, uint32_t target, int32_t value, uint32_t colour);
    // The pop-ups' text particles this frame: game-space position, the text ("123", ":" = Miss!), colour (0x80 = 1.0)
    // and scale x / y (the world quad is 0.1 x scale per sprite unit). The particle's blend mode applies (§G.12):
    // `damv`'s second particle `dam1` is the same number at 1.4 x 1.6 in dark red with the default 0x48, additive,
    // which makes the glow around the digits (DATA file 27; CODE 0x10041a20 -> 0x10041b50).
    struct TextParticle
    {
        glm::vec3              pos{ 0.0f };
        std::string            text;
        std::array<uint8_t, 4> rgba{};
        float                  scale  = 1.0f;
        float                  scaleY = 1.0f;
        bool                   additive = false; // blend 0x48 / 0x1d (SRCALPHA, ONE)
        bool                   subtract = false; // blend 0x41..0x43
    };
    [[nodiscard]] std::vector<TextParticle> textParticles() const;
    // Starts scheduler `name` found in `actor`'s own model / motion files (then file 0): the start actions' cmd_arg
    // (`cawh`, `cate`, `calg` ...; effect-format §C.5).
    bool playFromActor(uint32_t actor, dat::FxName name, uint32_t caster, uint32_t target, const FxAction* action = nullptr);
    // The DAT files an actor's resources live in (race base files, the monster model file ...).
    void setActorFiles(std::function<std::vector<uint32_t>(uint32_t actor)> f)
    {
        actorFiles_ = std::move(f);
    }
    // An actor's ranged class (byte 14 of its ranged weapon model's `info` chunk; -1 = none), for ops 0x76 / 0x77.
    void setActorRangedClass(std::function<int(uint32_t actor)> f)
    {
        rangedClass_ = std::move(f);
    }
    // The zone's own ambient effects (zone model DAT `fileId`): starts every generator with flag 0x1000 that is not under
    // a `weat` block and has a world / camera origin (modes 0, 0xe, 0xf, 0x14), with no actors, for as long as the zone
    // runs (CODE Gen_Init 0x10047c10). Their particles may draw the zone's unplaced Mmbs (`zoneMesh`, by chunk name;
    // CODE 0x100494a0): the sea, windmills, lamp glows. Returns how many started.
    std::size_t startZone(uint32_t fileId, std::function<const dat::FxMesh*(dat::FxName chunk)> zoneMesh);
    // The weather's sky effects: the flagged generators under `weat/<fourcc>` (else `weat/suny`) — sun and lens flares
    // (origin mode 0xe), moon (0xf), clouds and stars (0) — replacing the previous weather's (CODE: the weather object,
    // ctor 0x1018a6b0; change 0x1018a9d0 / 0x1018aa50). After startZone.
    void setWeather(std::string_view fourcc);
    // Where the camera is and the directions toward the sun and moon (game space): modes 0xe / 0xf place the sun and
    // moon from the camera along them (INFERRED from the matrices at 0x1003c485.., fields +0xb4 / +0xd4).
    void setSky(const glm::vec3& camera, const glm::vec3& towardSun, const glm::vec3& towardMoon)
    {
        camera_ = camera, towardSun_ = towardSun, towardMoon_ = towardMoon;
    }
    // Whether a point (game space) is seen from the camera (L2 op 0x53; the game asks its zone object, vtable +0x8c).
    void setSightTest(std::function<bool(const glm::vec3&)> test)
    {
        sightTest_ = std::move(test);
    }
    // The collision under a segment (game space): the first hit from `from` to `to`, if any (L1 op 0x42 drops a particle
    // onto the ground; the game asks its zone collision, CODE 0x101814a0).
    void setGroundRay(std::function<std::optional<glm::vec3>(const glm::vec3& from, const glm::vec3& to)> ray)
    {
        groundRay_ = std::move(ray);
    }
    // The camera's view-projection in game space (L0 op 0x15's visibility box test).
    void setViewProj(const glm::mat4& gameViewProj)
    {
        viewProj_ = gameViewProj, haveViewProj_ = true;
    }
    // Config "weather particles" (cfg 59): off freezes the weather's generators (gen+0xde bit 1 scales their dt by the
    // setting, CODE 0x1004a584) and hides their particles (bit 7, CODE 0x1003fafd / 0x10040af7 / 0x10043277). Every
    // generator under a `weat` block has both bits (CODE 0x10047d3d).
    void setWeatherParticles(bool on)
    {
        weatherParticles_ = on;
    }
    // Config "Effects" (cfg 146, the fxfilter page): whether a battle action's generator from `caster` to `target` is
    // hidden (CODE 0x100599f0 -> 0x100ade10, checked by scheduler op 2 when the scheduler carries an action).
    std::function<bool(uint32_t caster, uint32_t target)> hideBattleEffect;
    // Drops everything running (a new zone): particles, schedulers, actor colours (a teleport's fade-out) ...
    void reset()
    {
        schedulers_.clear(), started_.clear(), generators_.clear(), trails_.clear();
        tints_.clear(), tints2_.clear(), motions_.clear(), partMoves_.clear(), commits_.clear(), sounds_.clear();
        shot_.reset(), screen_ = {};
        weather_.clear(), flares_.clear(), ambience_.clear(), ambienceIndoor_.clear();
        resetActorState();
        reset7b();
    }
    // The Vana'diel day fraction 0..1 (time of day) for the time-of-day keyframes (L2 ops 0x3e / 0x3f: lamps at night,
    // the sea's tint; CODE global 0x10629ff4).
    void setDayFraction(float f)
    {
        dayFraction_ = f;
    }
    // The moon's frame 0..11 (L2 op 0x45; CODE 0x1018bcf0: (Vana'diel day / 7) % 12).
    void setMoonFrame(uint32_t f)
    {
        moonFrame_ = f % 12;
    }
    // The Vana'diel weekday 0..7 (Firesday .. Darksday; L2 op 0x4e; CODE 0x1018bca0: day % 8).
    void setWeekday(uint32_t d)
    {
        weekday_ = d % 8;
    }
    // The light at actors and in the scene (for L1 0x8f / 0x90): each is the brighter of (light 0 + ambient) and
    // (light 1 + ambient), as the game picks by squared length (CODE 0x1004f33e / 0x1004f4dc).
    void setLights(glm::vec3 actorLight, glm::vec3 sceneLight)
    {
        actorLight_ = actorLight;
        sceneLight_ = sceneLight;
    }
    // Advances everything by `seconds`.
    void update(float seconds, const FxActorLookup& actors);

    struct Vertex
    {
        glm::vec3 pos;   // game space
        glm::vec2 uv;
        glm::vec4 color; // texture stage 0's factor, already scaled (may exceed 1)
        // Texture stage 1's factor. Direct3D clamps each texture stage's result to 0..1, so the shader does
        // clamp(clamp(color x texture) x stage) (fxStages; docs/effect-format.md §M.2). 1 = a single stage.
        glm::vec4 stage{ 1.0f };
    };
    // The textured mesh combiner (§M.2, CODE 0x103524b8): stage 0 = MODULATE2X(diffuse, texture) for colour and
    // alpha, stage 1 = MODULATE2X(current, element colour) for colour and MODULATE4X for alpha. Untextured
    // (0x10352358): one stage, MODULATE2X / MODULATE4X(diffuse, element colour); with blend 0x44 (`blendD`) the alpha
    // is MODULATE2X (0x103523c4). Inputs 0..1; returns {color, stage} for a Vertex.
    [[nodiscard]] static std::pair<glm::vec4, glm::vec4> fxStages(const glm::vec4& vertex, const glm::vec4& element, bool textured, bool blendD = false)
    {
        if (!textured)
        {
            return { glm::vec4(glm::vec3(vertex) * glm::vec3(element) * 2.0f, vertex.a * element.a * (blendD ? 2.0f : 4.0f)), glm::vec4(1.0f) };
        }
        return { vertex * 2.0f, glm::vec4(glm::vec3(element) * 2.0f, element.a * 4.0f) };
    }
    // The element colour's alpha (TFACTOR, 0..1) from the particle's alpha byte times its fades: truncated to a byte
    // (CODE 0x1003e260); a D3m drawn with blend 0x44 forces 0x7F and above to 0xFF (CODE 0x10040170..0x1004018c).
    [[nodiscard]] static float fxElementAlpha(float alphaByte, bool d3mBlendD)
    {
        const float b = std::floor(std::clamp(alphaByte, 0.0f, 255.0f));
        return d3mBlendD && b >= 127.0f ? 1.0f : b / 255.0f;
    }
    // The blend record (op 0x1e, u16 → p+0x16c) picks its states by its LOW byte only (CODE 0x10046710: `switch (mode &
    // 0xff)`); the high byte is a parameter. 0x64 / 0x68 store min((mode & 0xffff) >> 7 & ~1, 255) as TFACTOR's alpha
    // byte (0x1047bc0f) after the particle's colour was written there (0x10045fc8), so it replaces the particle's alpha:
    // 0x8064 (Port Windurst's windmill sails) draws at full alpha. 0x62 stores the high byte itself (then the special
    // path 0x10046480, not traced). nullopt: the particle's own alpha stands.
    [[nodiscard]] static std::optional<float> fxBlendElementAlpha(uint16_t mode)
    {
        switch (mode & 0xFF)
        {
            case 0x64:
            case 0x68: return static_cast<float>(std::min((static_cast<uint32_t>(mode) >> 7) & ~1u, 255u)) / 255.0f;
            case 0x62: return static_cast<float>(mode >> 8) / 255.0f;
            default: return std::nullopt;
        }
    }
    // What fx.frag does with them: the texture sample, then each stage clamped (Direct3D's fixed-function stages).
    [[nodiscard]] static glm::vec4 fxCombine(const glm::vec4& color, const glm::vec4& texel, const glm::vec4& stage)
    {
        return glm::clamp(glm::clamp(color * texel, 0.0f, 1.0f) * stage, 0.0f, 1.0f);
    }
    struct Batch
    {
        // Blend 0x44 (and kin) SRCALPHA / INVSRCALPHA; 0x48 additive SRCALPHA / ONE; 0x41-0x43 reverse subtract
        // dst - src * alpha (CODE 0x10046480: BLENDOP REVSUBTRACT, the sea's darkening layer).
        enum class Blend : uint8_t
        {
            Alpha,
            Additive,
            Subtract,
            Multiply, // ZERO / INVSRCCOLOR: dst * (1 - src) (ground shadows; shadow_re.md)
        };
        const dat::Texture* texture = nullptr; // nullptr = untextured
        Blend               blend   = Blend::Additive;
        std::vector<Vertex> vertices;          // triangle list
        float               depthKey = 0.0f;   // batches come sorted by this, highest first (L1 op 0x30)
        // Samples the screen as drawn so far instead of `texture`, at each fragment's own screen position (CODE
        // 0x10042c30: the UVs are the vertices' projections): the `ring` / `dist` particle classes 0x24 / 0x22.
        bool                screen  = false;
    };
    // The batch blend for a blend record, by its low byte (CODE 0x10046710, §G.12): 0x41-0x43 the reverse-subtract
    // path, 0x03 / 0x44 / 0x64 SRCALPHA / INVSRCALPHA, 0x46 / 0x47 (ZERO / INVSRCALPHA) drawn as alpha, the rest
    // (0x48 default, 0x49, 0x68, ...) SRCALPHA / ONE.
    [[nodiscard]] static Batch::Blend fxBlendClass(uint16_t mode)
    {
        switch (mode & 0xFF)
        {
            case 0x41:
            case 0x42:
            case 0x43: return Batch::Blend::Subtract;
            case 0x03:
            case 0x44:
            case 0x46:
            case 0x47:
            case 0x64: return Batch::Blend::Alpha;
            default: return Batch::Blend::Additive;
        }
    }
    // Full-screen layers drawn over the scene from a captured image of it (scheduler ops 0x0E / 0x10): `frozen` =
    // the image captured when op 0x10 / 0x0E started (freezeSerial() changes then), else this frame's. UV rect
    // (u0, v0, u1, v1) of the image, colour in 0..1 with the 2x factors applied.
    struct ScreenLayer
    {
        bool      frozen = true;
        glm::vec4 uvRect{ 0, 0, 1, 1 };
        glm::vec4 colour{ 1.0f };
        bool      additive = false;
        bool      image    = true; // false: the colour alone (the L2 0x60 screen flash, added)
    };
    [[nodiscard]] std::vector<ScreenLayer> screenLayers() const;
    // Changes when a scheduler asks for a new captured image (ops 0x0E / 0x10): the renderer copies the scene then.
    [[nodiscard]] uint32_t freezeSerial() const
    {
        return freezeSerial_;
    }
    // A camera layer (op 0x0E or 0x10) is on screen now (test hooks; no side effects, unlike screenLayers()).
    [[nodiscard]] bool layerActive(int op) const;
    // The screen flash this frame (L2 op 0x60, CODE 0x10014f50: particle colours added into one colour, clamped),
    // rgb 0..1, added over the scene.
    [[nodiscard]] glm::vec3 screenFlash() const
    {
        return flash_;
    }
    // Camera shake amplitude this frame (L2 op 0x5f with rec+0xC, CODE 0x10021ad0), game units; 0 = none.
    [[nodiscard]] float cameraShake() const
    {
        return shake_;
    }
    // Geometry for this frame. camRight / camDown: the camera's axes in game space, for billboards.
    void build(std::vector<Batch>& out, const glm::vec3& camRight, const glm::vec3& camDown) const;

    [[nodiscard]] std::size_t particleCount() const;
    // The live particles of generators named `generator`: position (game space) and rotation (tests).
    struct ParticleView
    {
        glm::vec3 pos{ 0.0f };
        glm::vec3 rot{ 0.0f };
    };
    [[nodiscard]] std::vector<ParticleView> particles(dat::FxName generator) const;
    // Ops met at run time that xi-client does not carry out (client/effect_ops.h: Unhandled), by (list, op): how
    // often and one "file:name" where. "unhandled ops: none" when empty (the goal 7 check).
    struct UnhandledOp
    {
        std::size_t count = 0;
        std::string where;
    };
    [[nodiscard]] const std::map<std::pair<uint8_t, uint8_t>, UnhandledOp>& unhandledOps() const
    {
        return unhandled_;
    }
    [[nodiscard]] std::string unhandledSummary() const;
    // The actor's wepN model parts that ops 0x75 / 0xA3 hid (bit N; a monster's `init` picks its weapon variants).
    [[nodiscard]] uint16_t hiddenParts(uint32_t actor) const
    {
        const auto it = hiddenParts_.find(actor);
        return it == hiddenParts_.end() ? 0 : it->second;
    }
    // The stance picker's weapon parts (CODE 0x100c867c..0x100c8713): picking the idle / battle stance shows wep0 / wep1
    // (main, sub) and hides wep2 (ranged), undoing `hwmg` (magic) / `hwso` (shooting) after the action. Called each frame
    // with whether the actor's action motion (cast, ability, swing, effect motion) is still playing; applied when it
    // ends. INFERRED: the picker also runs on other stance changes; here only at an action's end, and only for actors
    // whose parts an effect changed.
    void stancePick(uint32_t actor, bool acting)
    {
        const bool was = std::exchange(acting_[actor], acting);
        if (was && !acting)
        {
            if (const auto it = hiddenParts_.find(actor); it != hiddenParts_.end())
            {
                it->second = static_cast<uint16_t>((it->second & ~3u) | 4u);
            }
        }
    }
    // Doll ids (ops 0x15 / 0x16 / 0x22 / 0x23) have this top byte; they resolve to their snapshot.
    static constexpr uint32_t kDollIdBase = 0xFD000000u;
    // The zone's ambience loop now (ambience_re.md; CODE 0x10189bb0): the Sep under `weat/<weather>` named HHMM
    // (first digit 0 / 1, third <= 5) with the latest start before the time of day, else the last in file order.
    // Outdoor ones only (the `indo` variants need the indoor flag). Empty when the zone has none.
    // `indoor`: the `indo` variant when the zone has one.
    [[nodiscard]] std::span<const std::byte> ambienceSep(bool indoor = false) const;
    // The weather's lens flares (resource type 0x39): the sun's (origin mode 0xe) and the moon's (0xf). Drawn in
    // screen space by the caller (CODE 0x10054800); the generator's own particle is not run.
    struct Flare
    {
        dat::FxFlare        flare;
        const dat::Texture* texture = nullptr;
        bool                moon    = false;
    };
    [[nodiscard]] const std::vector<Flare>& flares() const
    {
        return flares_;
    }
    // Appends the flares as additive batches on a plane 1 unit in front of the camera (game space axes; tanHalfY and
    // aspect from the projection). `sunVisible` / `moonVisible` 0..1: the caller's sight test, faded.
    void buildFlares(std::vector<Batch>& out, const glm::vec3& camPos, const glm::vec3& camForward, const glm::vec3& camRight,
                     const glm::vec3& camDown, float tanHalfY, float aspect, float sunVisible, float moonVisible) const;
    // The point lights this frame (zone lanterns: on at night, faded out 30-50 units from the camera).
    [[nodiscard]] std::vector<FxPointLight> pointLights() const;
    [[nodiscard]] std::size_t generatorCount() const
    {
        return generators_.size();
    }
    // The running generators as "name (stopped, particles, elapsed / duration ticks)", for diagnostics and tests.
    [[nodiscard]] std::string describeGenerators() const;
    // Motions requested since the last call.
    std::vector<FxMotion> takeMotions()
    {
        return std::exchange(motions_, {});
    }
    // Cutscene camera: the pose of the last camera task (it holds after the task ends), until clearCamera().
    [[nodiscard]] std::optional<FxCameraPose> camera() const;
    void clearCamera()
    {
        shot_.reset();
    }
    // The screen colour (op 0x0F fades): rgb, and a = how much it covers the frame (0 = none).
    [[nodiscard]] glm::vec4 screenColour() const;
    // Schedulers by file and name (event ops 0x45 / 0x52 / 0x55: shots, fades): start, stop, still running.
    bool playFile(uint32_t fileId, dat::FxName name, uint32_t caster, uint32_t target);
    void stopFile(uint32_t fileId, dat::FxName name, uint32_t caster);
    [[nodiscard]] bool runningFile(uint32_t fileId, dat::FxName name, uint32_t caster) const;
    // Door part moves requested since the last call.
    std::vector<FxPartMove> takePartMoves()
    {
        return std::exchange(partMoves_, {});
    }
    // Runs scheduler `name` (`open`, `clos`, `into`, `intc`) of the zone block named `door` (the door's fourcc, 0x00E
    // +0x34) with `actor` as caster and target (CODE 0x100acad0 / 0x100ad110). False when there is none.
    bool playDoor(uint32_t door, dat::FxName name, uint32_t actor);
    // Sounds started since the last call.
    std::vector<FxSound> takeSounds()
    {
        return std::exchange(sounds_, {});
    }
    // Results whose damage an effect has shown (op 0x2B) since the last call: FxAction::tag | (result + 1).
    std::vector<uint64_t> takeCommits()
    {
        return std::exchange(commits_, {});
    }
    // An actor's colour from scheduler ops 0x29 / 0x2A / 0xA1 (colour slot 0, actor +0x660; r, g, b, a; 1 = 0x80 =
    // unchanged). Alpha below 1 fades it out. Slot 1 (ops 0x46 / 0x47) is the overlay's: FxOverlay::colour.
    [[nodiscard]] glm::vec4 actorTint(uint32_t id) const;
    [[nodiscard]] bool        active() const
    {
        return !schedulers_.empty() || !started_.empty() || !generators_.empty() || !trails_.empty() || !tints_.empty() || !tints2_.empty();
    }

    // ---- Actor state the scheduler's actor ops set (effect-format "Coverage, part 7A") ----
    // Motion sets an actor's `init` / growth schedulers choose (ops 0x79 battle, 0x8C idle, 0xA4 run, 0xA5 walk; CODE
    // name tables 0x1032b804.. "btl " "bt1 " .., actor +0x7c8..+0x7d4): the stems, empty = the model's own.
    struct FxStance
    {
        std::string idle, battle, run, walk;
    };
    [[nodiscard]] const FxStance* stance(uint32_t actor) const;
    // A texture drawn over the actor's model in a second pass (ops 0x40 / 0x41 / 0x54 / 0x55, CODE 0x100616e0; draw
    // 0x100d6dc4 -> 0x1002b885): looked up by the camera-space normal, moved by `scroll` (ops 0x42-0x45, CODE
    // 0x100614e0: actor +0x73c / +0x740, wrapped).
    struct FxOverlay
    {
        const dat::Texture* texture = nullptr;
        glm::vec2           scroll{ 0.0f };
        bool                additive = true; // record blend 0x48 (the default); else alpha
        glm::vec4           colour{ 1.0f };  // colour slot 1 (ops 0x46 / 0x47, actor +0x74c; 1 = 0x80)
    };
    [[nodiscard]] std::optional<FxOverlay> actorOverlay(uint32_t actor) const;
    // The screen-refraction pass strength (ops 0x48 / 0x49, CODE 0x10061170: actor +0x744; draw 0x1002b947 samples
    // the screen copy, colour 0x80 - the actor's alpha). 0 = off.
    [[nodiscard]] float actorShimmer(uint32_t actor) const;
    // Knockback (op 0x5E, CODE 0x100aa870 / tick 0x100aaaa0): the slide so far (game space), added where the actor
    // is drawn until the caller clears it (a new server position).
    [[nodiscard]] glm::vec3 actorDisplacement(uint32_t actor) const;
    void                    clearDisplacement(uint32_t actor);
    // Op 0x89 (CODE 0x1005f380): no head look-at while set (actor +0x840 bit 1, read by 0x100d5ac0), until the
    // duration ends or the actor moves a yalm.
    [[nodiscard]] bool lookAtSuppressed(uint32_t actor) const;
    // Skeleton points an actor's `init` names (ops 0x9E -> +0x8a6, the target arrow's anchor, CODE 0x100d0620; 0x9F
    // -> +0x8a7, the other projected anchor 0x100d03f0, INFERRED the name). which 0 / 1; -1 = the default.
    [[nodiscard]] int actorPoint(uint32_t actor, int which) const;
    // Our own character (op 0x4A plays its sound only on it; CODE 0x1005b50a: actor vtable +0x304).
    void setLocalPlayer(uint32_t id)
    {
        localPlayer_ = id;
    }
    // An actor's weapon sheath styles (main, sub: actor +0x87c / +0x87f) for op 0x84's `in1N` / `ot2N`.
    void setActorSheathStyles(std::function<std::array<int, 2>(uint32_t actor)> f)
    {
        sheathStyles_ = std::move(f);
    }
    // The actor's model generators that start by themselves (CODE 0x10084f90): flag 0x1000, origin mode in 1, 2, 4,
    // 5, 7..12, 16..19, unless the actor's `init` set op 0x9A. Runs after this frame's schedulers (so `init` is first).
    void startModelGenerators(uint32_t actor)
    {
        pendingModelGenerators_.push_back(actor);
    }

private:
    struct Particle
    {
        glm::vec3             pos{ 0.0f }, rot{ 0.0f }, scale{ 1.0f };
        std::array<uint8_t, 4> rgba{ 0x80, 0x80, 0x80, 0x80 };
        float                 dayFactor = 1.0f; // L2 op 0x3f this frame: alpha x this when < 1, hidden at <= 0
        float                 depthKey  = 0.0f; // L1 op 0x30: draw order, higher first (INFERRED far-to-near)
        float                 lightRange = 0.0f, lightIntensity = 0.0f, lightRangeScale = 1.0f, lightIntensityScale = 1.0f; // L1 0x58
        float                 remaining = 0, total = 0, age = 0;
        std::array<float, 5>  morph{};          // D3b frame weights (L2 ops 0x1e-0x22; all 0 = frame 0)
        const std::vector<glm::vec3>* path = nullptr; // L1 op 0x54: a Vtx path the particle follows (L2 op 0x34)
        glm::vec3             pathPrev{ 0.0f };
        uint32_t              pathCurve = 0;    // the path's keyframe (record offset in Generator::curves), 0 = by age
        float                 frameTimer = 0, frameLength = 0;
        uint32_t              frame = 0;
        glm::vec2             uv{ 0.0f };
        uint32_t              flags = 0;
        uint16_t              blend = 0x48;
        glm::mat4             spawnMatrix{ 1.0f };
        std::vector<uint32_t> vars;
        const dat::FxMesh*    mesh = nullptr;
        uint32_t              serial = 0; // for child generators that ride this particle (L1 0x44 / 0x53 / 0x6a)
        // Class 0x24 `ring` (L1 0x3a; CODE 0x10043650): radii and D3DCOLOR colours of 2-4 rings, `segments` around.
        std::array<float, 4>    ringRadius{};
        std::array<uint32_t, 4> ringColour{};
        uint8_t                 ringSegments = 0, ringCount = 0;
        // Mesh class 0x1c0 (L1 0x55; CODE 0x100411e0): a second, environment-mapped texture over the mesh, its
        // colour (file bytes R, G, B, A) and the texture-matrix angles (L1 0x59-0x5b / L2 0x35-0x37 animate them).
        const dat::Texture*     overlay = nullptr;
        std::array<uint8_t, 4>  overlayRgba{ 0x80, 0x80, 0x80, 0x80 };
        glm::vec3               overlayAngles{ 0.0f };
        float                   distortion = 0.0f; // class 0x22 `dist` strength (L1 0x32)
        float                   volume = 1.0f;     // sound particles: L2 0x43 from the L1 0x68 curve (0x7f = 1)
        std::optional<float>    drag;              // L2 0x44: the L2 0x2c drag factor from a curve (the game writes the record)
        uint8_t                 soundMode = 0;     // L1 0x4c / 0x92 / 0x9e (FxSound::distanceMode)
        bool                    soundKeepFar = false; // L1 0x8a
    };
    struct Generator
    {
        const dat::EffectFile*  file = nullptr;
        const dat::FxResource*  res  = nullptr;
        uint32_t                caster = 0, target = 0;
        uint32_t                origin = 0, flags = 0;
        int16_t                 interval = 0, intervalRand = 0;
        std::array<uint32_t, 4> lists{};
        float                   duration = 0, elapsed = 0, countdown = 0;
        uint32_t                spawned = 0;
        bool                    follow = false, stopped = false, singleShot = false;
        bool                    zone   = false; // a zone ambient generator: world placed, runs until the zone changes
        bool                    weather = false; // ... started for the current weather (setWeather replaces them)
        glm::mat4               matrix{ 1.0f };
        bool                    placed = false;
        std::vector<Particle>   particles;
        const dat::FxMesh*      mesh = nullptr;
        const dat::FxResource*  sound = nullptr; // resource type 0x3d: each particle plays this Sep (zone ambience)
        bool                    light = false;   // resource type 0x47: each particle is a point light
        bool                    text  = false;   // resource type 0x5c: each particle shows `number` ("menu dmgnum" digits)
        int32_t                 number = 0;      // from the scheduler that started it; INT32_MIN = "Miss!"
        uint32_t                numberColour = 0;
        const dat::EffectFile*  meshFile = nullptr; // where the mesh (and so its texture) was found
        std::map<uint32_t, dat::FxCurve> curves; // by record offset
        // A child generator (§G.9 L1 0x3c / 0x44 / 0x53 / 0x6a, L3 0x01): placed at a particle of `parent` instead of
        // through its origin word. Attached children follow particle `parentSerial` and stop when it dies; bursts
        // (parent == nullptr, placed at once) run one emission tick where the particle was.
        Generator*              parent       = nullptr;
        uint32_t                parentSerial = 0;
        bool                    child        = false;
        glm::vec3               parentRot{ 0.0f }, parentScale{ 1.0f }; // the parent particle's (L1 0x47-0x49)
        // The parent particle as it was when a burst child started (attached children read the live one): L1 ops
        // 0x46, 0x48, 0x4a, 0x79, 0x7a, 0x7e, 0x7f, 0x98, 0x9d copy from it. With the parent generator's resource (L1
        // 0x46 / 0x94 / 0x98 find records in its L1 list).
        std::shared_ptr<const Particle> parentSnap;
        const dat::FxResource*  parentRes = nullptr;
        std::size_t             parentL1  = 0;
        std::optional<glm::mat4> spinBase; // L0 0x0e-0x10 on a burst child: its placement before the turn
        dat::FxName             vtxOverride = 0; // L1 0x94: the parent's L1 0x4e Vtx name
        uint8_t                 resType      = 0;     // L1 header rec+0x21: the particle class
        float                   soundNear = 0, soundFar = 0; // L1 0x4c (0 = the defaults)
        dat::FxName             soundAt   = 0;                // L1 0x6b: the sound sits where this generator's particle is
        // L0 keyframe ops rewrite L1 records (§G.11): a private copy of the payload once one does (spawn reads it).
        std::vector<std::byte>  patched;
        bool                    model = false; // an actor model's own generator (CODE 0x10084f90): runs while it exists
    };
    // A child generator to start after this frame's generator pass (starting it inside the pass would move the list).
    struct ChildStart
    {
        const dat::EffectFile* file = nullptr;
        const dat::FxResource* res  = nullptr;
        uint32_t               caster = 0, target = 0;
        Generator*             parent = nullptr; // attached (follows the particle) when set
        uint32_t               parentSerial = 0;
        glm::mat4              matrix{ 1.0f };    // the particle's world placement now
        glm::vec3              rot{ 0.0f }, scale{ 1.0f };
        std::shared_ptr<const Particle> snap;     // the whole particle (Generator::parentSnap)
        const dat::FxResource* parentRes = nullptr;
        std::size_t            parentL1  = 0;
    };
    std::vector<ChildStart> childStarts_;
    uint32_t                particleSerial_ = 0;
    // Queues the child generator `name` (from generator g's block up) for particle q: attached or a one-tick burst.
    void startChild(Generator& g, const Particle& q, dat::FxName name, bool attached);
    // Weapon trail (§M.5, scheduler op 0x2C): baked blade-edge samples in the caster's model space, revealed over
    // the record's duration and faded per tick.
    struct Trail
    {
        uint32_t                caster = 0;
        std::vector<glm::vec3>  edgeA, edgeB;
        std::vector<float>      sampleTick; // when each sample appears
        glm::vec4               colA{ 0.0f }, colB{ 0.0f }; // 0..1 (0x80 = 0.5)
        float                   fadeA = 0, fadeB = 0;       // alpha per tick
        float                   age = 0, duration = 0;
        glm::mat4               matrix{ 1.0f };
        bool                    placed = false;
    };
    // CMoActorColorDriveTask (§S.3 ops 0x29 / 0x2A): from the colour at the start to `to` over `duration` ticks.
    struct Tint
    {
        glm::vec4 from{ 1.0f }, to{ 1.0f };
        float     elapsed = 0, duration = 0;
        [[nodiscard]] glm::vec4 now() const
        {
            return duration > 0 ? glm::mix(from, to, std::min(1.0f, elapsed / duration)) : to;
        }
    };
    struct Scheduler
    {
        const dat::EffectFile* file = nullptr;
        const dat::FxResource* res  = nullptr;
        uint32_t               caster = 0, target = 0;
        std::size_t            pc   = 0;
        float                  wait = 0;
        int                    depth = 0;
        bool                   dead  = false; // killed by op 0x5F
        std::shared_ptr<const FxAction> action; // shared with sub-schedulers
        std::size_t                     result = 0;          // current (target, result) pair in `action`
        std::size_t                     loop   = SIZE_MAX;   // the last op 0x31 label
        float                           rate   = 1.0f;       // delays and durations x rate (op 0x73, §S.1)
        std::size_t                     after  = SIZE_MAX;   // op 0x3D: continue here once the chosen record ran
        std::vector<int32_t>            stack;               // expression VM (§S.5)
        std::optional<int32_t>          number;              // damage pop-ups (file 27 damv / curv / misv): sched+0xa8
        uint32_t                        numberColour = 0;    // ... and +0xa4, 0xRRGGBB (the log colour)
        uint32_t                        realCaster = 0, realTarget = 0; // before ops 0x15 / 0x16 put dolls in their place
    };
    // A doll (CXiDollActor, ops 0x15 / 0x16 / 0x22 / 0x23; CODE 0x100abdd0): a copy of an actor's placement and its 48
    // attach points at the moment of the op. Effects started on it stay where the actor was; 0x22 / 0x23 / 0x9D dolls
    // follow the actor's position instead (`follows`, CODE 0x100ac020). Nothing draws a doll.
    struct Doll
    {
        uint32_t source = 0;
        FxActor  snapshot;
        std::array<std::optional<glm::vec3>, 48> points{};
        bool     follows = false; // 0x22 / 0x23 / 0x9D: tracks the source's position
        float    age     = 0;
    };
    std::map<uint32_t, Doll> dolls_;
    std::map<uint32_t, uint16_t> hiddenParts_;
    std::map<uint32_t, bool>     acting_; // stancePick: the action motion was playing last frame
    uint32_t                 dollSerial_ = 0;
    uint32_t                 makeDoll(uint32_t source, bool follows);

public:
    // A footstep Sep (sound-format.md §footsteps): `name` in the zone file's `fses` (dry) or `fser` (rain) set, else
    // the same name with material 2, else file 0's `foot`. Empty when none.
    std::span<const std::byte> footstepSep(dat::FxName name, bool rain);
    // Footstep effects (config_fx2_re.md §2; CODE 0x100d524c / 0x100d6320): one burst of the zone set's generator
    // ('w' walking | '0') + ('0' + material) + "00" on the actor, and file 0's `fmrk` footprint on sand, snow and
    // shallow water (materials 3, 4, 8).
    void footstepEffect(uint32_t actor, uint8_t material, bool walking, bool rain);
    // The deep-water wake (config_fx2_re.md §2; CODE 0x100d6400): the zone set's `i900` on the actor while `deep`
    // (floor material 9); call every frame.
    void wadeEffect(uint32_t actor, bool deep, bool rain);

private:
    const dat::EffectFile* file(uint32_t id);
    // (file, resource) for a name: the scheduler's own file from `block` up, then file 0.
    std::pair<const dat::EffectFile*, const dat::FxResource*> lookup(const dat::EffectFile* f, int block, dat::FxType type, dat::FxName name);
    void startScheduler(const dat::EffectFile* f, const dat::FxResource* r, uint32_t caster, uint32_t target, int depth, float waitTicks = 0.0f, float rate = 1.0f);
    // Starts (or, with `reuse`, re-triggers the running instance of) a generator; the new / re-triggered one, or null.
    Generator* startGenerator(const dat::EffectFile* f, const dat::FxResource* r, uint32_t caster, uint32_t target, float duration, bool reuse = true);
    bool stepScheduler(Scheduler& s, float ticks);
    void skipBlock(Scheduler& s, bool toElse); // expression VM: skip to the else (or past the block)
    void place(Generator& g, const FxActorLookup& actors);

public:
    // The generator's size scale from its origin word and chunk+0x20 weight (effect-format §G.4; CODE 0x10053d90).
    static glm::vec3 sizeScale(uint32_t origin, float weight, const FxActor& a, const FxActor& b);

private:
    void spawn(Generator& g, uint32_t index);
    void tickParticle(Generator& g, Particle& p, float dt);
    const dat::FxCurve* curve(Generator& g, std::size_t recordOffset, dat::FxName name);

public:
    // A point along a Vtx path, u 0..1 (chord-length Catmull-Rom; CODE 0x10063ab0): the gulls, ships.
    static glm::vec3 pathPoint(const std::vector<glm::vec3>& points, float u);

private:

    const dat::DatIndex&                                 index_;
    std::map<uint32_t, std::unique_ptr<dat::EffectFile>> files_;
    std::vector<Scheduler>                               schedulers_;
    std::vector<Scheduler>                               started_; // started while stepping; joined after the step
    std::vector<std::unique_ptr<Generator>>              generators_;
    std::vector<Trail>                                   trails_;
    std::map<const dat::FxResource*, std::vector<glm::vec3>> paths_; // Vtx chunks, decoded once
    std::map<uint32_t, Tint>                             tints_;
    std::map<uint32_t, Tint>                             tints2_; // ops 0x46 / 0x47: colour slot 1, the overlay's colour
    std::vector<FxMotion>                                motions_;
    std::vector<FxPartMove>                              partMoves_;
    struct CameraKey
    {
        glm::vec3 eye{ 0.0f }, at{ 0.0f };
        float     zoom = 350.0f, roll = 0.0f, t = 0.0f;
    };
    struct CameraTask
    {
        std::vector<CameraKey> keys;
        float                  duration = 0, elapsed = 0; // ticks
        dat::FxName            name = 0;                   // the Camera chunk (op 0xB0 stops it by name)
    };
    std::optional<CameraTask> shot_;
    struct ScreenFade
    {
        glm::vec4 from{ 1, 1, 1, 0 }, to{ 1, 1, 1, 0 };
        float     duration = 0, elapsed = 0;
    } screen_;
    // Captured-image layers (ops 0x0E / 0x10, CODE 0x10060260 / 0x1005ff00): over `duration` ticks.
    struct Layer
    {
        int       op = 0x10;
        glm::vec4 colour{ 1.0f };
        float     zoom = 0, duration = 0, elapsed = 0;
    };
    std::vector<Layer>                                   layers_;
    // Op 0x0E's state in the game's global (0x104568fc +0x32c colour, +0x330 z): it stays after the task ends, so a
    // `blon` overlay holds until `blof` fades its alpha to 0 (CODE 0x10058cc4 set-up, 0x10060260 tick).
    struct ScreenColour
    {
        glm::vec4 colour{ 0.0f }, from{ 0.0f }, to{ 0.0f }; // bytes / 255
        float     z = 1.0f, zStart = 1.0f, zTarget = 1.0f;   // INFERRED: 1 (the whole image) before any task
        float     duration = 0, remaining = 0;
    } screenColour_;
    uint32_t                                             freezeSerial_ = 0;
    glm::vec3                                            flash_{ 0.0f };
    glm::vec3                                            actorLight_{ 1.0f };
    mutable glm::vec3                                    glare_{ 0.0f }; // this frame's flare glare (buildFlares -> screenLayers)
    std::array<uint8_t, 4>                               glareRgba_{ 0x80, 0x80, 0x80, 0x80 }; // L1 0x4b (0 -> 0x80808080)
    glm::vec3                                            sceneLight_{ 1.0f };
    float                                                shake_ = 0.0f;
    std::vector<uint64_t>                                commits_;
    std::vector<FxSound>                                 sounds_;
    const dat::EffectFile*                               zoneFile_ = nullptr;
    uint32_t                                             zoneFileId_ = 0; // dropped from files_ at the next zone
    std::function<const dat::FxMesh*(dat::FxName)>        zoneMesh_;
    bool                                                 startingZone_ = false;
    float                                                dayFraction_  = 0.5f;
    uint32_t                                             moonFrame_    = 0;
    uint32_t                                             weekday_      = 0;
    std::string                                          weather_;
    bool                                                 weatherParticles_ = true;
    std::vector<Flare>                                   flares_;
    std::vector<std::pair<float, const dat::FxResource*>> ambience_, ambienceIndoor_; // (start, Sep) of the weather, file order
    glm::vec3                                            camera_{ 0.0f }, towardSun_{ 0, -1, 0 }, towardMoon_{ 0, -1, 0 };
    glm::mat4                                            viewProj_{ 1.0f };
    std::function<bool(const glm::vec3&)>                sightTest_;
    std::function<std::optional<glm::vec3>(const glm::vec3&, const glm::vec3&)> groundRay_;
    bool                                                 haveViewProj_ = false;
    std::mt19937                                         rng_{ 0x1105 };
    std::map<std::pair<uint8_t, uint8_t>, UnhandledOp>   unhandled_;
    // Counts op `op` of list `list` (FxList) when effect_ops.h marks it Unhandled; logs the first of each with XI_FX_TRACE.
    void noteOp(uint8_t list, uint8_t op, const dat::EffectFile* f, dat::FxName name);
    std::function<std::vector<uint32_t>(uint32_t)>        actorFiles_;
    std::function<int(uint32_t)>                          rangedClass_;
    // (file, resource) for a name in an actor's files, else nothing.
    std::pair<const dat::EffectFile*, const dat::FxResource*> lookupActor(uint32_t actor, dat::FxType type, dat::FxName name);
    const FxActorLookup*                                 actors_ = nullptr; // during update()
    std::shared_ptr<const FxAction>                      action_;           // given to schedulers started now
    std::size_t                                          result_ = 0;

    // Part 7A actor state (effects_actor.cpp).
    struct Drive // an actor float going from `from` to `to` over `duration` ticks (CODE 0x100615a0 / 0x100611e0)
    {
        float from = 0, to = 0, elapsed = 0, duration = 0;
        [[nodiscard]] float now() const
        {
            return duration > 0 ? from + (to - from) * std::min(1.0f, elapsed / duration) : to;
        }
    };
    struct Overlay
    {
        const dat::Texture* texture  = nullptr;
        bool                additive = true;
        float               remaining = 0; // ticks
    };
    struct Knockback
    {
        glm::vec3 velocity{ 0.0f }, offset{ 0.0f }, source{ 0.0f };
        float     hold = 0, decay = 0; // ticks at full speed, then velocity x (1 - decay) per tick
        bool      near = false;        // stop short of the source (0.81) when pulled
        bool      active = false;
    };
    struct LookOff
    {
        float                    remaining = 0;
        std::optional<glm::vec3> start;
    };
    std::map<uint32_t, FxStance>                         stances_;
    std::map<uint32_t, Overlay>                          overlays_;
    std::map<uint32_t, std::array<Drive, 2>>             scrolls_;  // ops 0x42 / 0x44 (u) and 0x43 / 0x45 (v)
    std::map<uint32_t, Drive>                            shimmer_;  // ops 0x48 / 0x49
    std::map<uint32_t, Knockback>                        knockbacks_;
    std::map<uint32_t, LookOff>                          lookOff_;
    std::map<uint32_t, std::array<int, 2>>               points_;
    std::set<uint32_t>                                   noModelGenerators_; // op 0x9A 1
    std::vector<uint32_t>                                pendingModelGenerators_;
    uint32_t                                             localPlayer_ = 0;
    std::function<std::array<int, 2>(uint32_t)>          sheathStyles_;
    // Scheduler ops 0x40-0x45, 0x48-0x4A, 0x5C, 0x5E, 0x79, 0x84, 0x86, 0x88, 0x89, 0x8C-0x99, 0x9A, 0x9E, 0x9F, 0xBE.
    void actorOp(Scheduler& s, uint8_t op, int16_t dur);
    void tickActorState(float ticks);
    void resetActorState();
public:
    // --- Scheduler ops that reach the environment, sound and actor state (effect-format "Coverage, part 7B"). ---
    // The environment a scheduler asked for (ops 0x7C / 0xAE / 0x7E), held until the zone changes: a weather group
    // to show instead of the server's, the alternate (`sun1`, `rai1` ...) fourcc table for the server's weather, or
    // one Weather record (type 0x2f payload) applied as it is. `serial` changes with every request.
    struct FxEnvironment
    {
        std::string                fourcc;            // empty = the server's weather
        bool                       altTable = false;  // EnvMgr_SelectFourccTable(1)
        float                      blendSeconds = 0;  // how long the change cross-fades
        std::span<const std::byte> record;            // op 0x7E: this record, not the zone's keyframes
        uint32_t                   serial = 0;
    };
    [[nodiscard]] const FxEnvironment& environment() const
    {
        return environment_;
    }
    // Slots of sounds started by op 0x8A that op 0x8B stopped since the last call (FxSound::slot).
    std::vector<uint32_t> takeSoundStops()
    {
        return std::exchange(soundStops_, {});
    }
    // Music fades (op 0xA6, CODE 0x10035d60 -> 0x10036020): level 0..1 over seconds; level 0 stops the music.
    std::optional<std::pair<float, float>> takeMusicFade()
    {
        return std::exchange(musicFade_, std::nullopt);
    }
    // The weapon the actor holds in the grip while an op asks (0x9B for its duration, 0xA7 on / 0xA8 off; CODE vfunc
    // 0x324 -> actor +0x83e[slot], read by the weapon attachment 0x1002a070): slot 0 main, 1 sub.
    [[nodiscard]] bool weaponHeld(uint32_t actor, int slot) const;
    // Footsteps sound while standing (op 0xBC for its duration: actor +0x8ab, CODE 0x100d4d91).
    [[nodiscard]] bool stepsForced(uint32_t actor) const;
    // A locomotion motion variant (ops 0xA4 run, 0xA5 walk; CODE tables 0x1032b824 / 0x1032b834): 0 = `run` / `wlk`,
    // 1..3 = `1un` / `1lk` .. `3un` / `3lk`. Kind 2 = run, 3 = walk (0 battle, 1 idle: ops 0x79 / 0x8C).
    [[nodiscard]] int motionVariant(uint32_t actor, int kind) const;
    // Where an actor is drawn while op 0x7A holds it at another actor's attach point (game space), and its facing while
    // op 0x62 turns it toward its target (FxActor::yaw convention). Updated by update().
    [[nodiscard]] std::optional<glm::vec3> attachedPosition(uint32_t actor) const;
    [[nodiscard]] std::optional<float>     yawOverride(uint32_t actor) const;
    // Config 153 (the effect filter bits; op 0xB3 plays its sound only with bit 1 set, CODE 0x101937a0).
    void setConfig153(uint32_t bits)
    {
        config153_ = bits;
    }

private:
    FxEnvironment                          environment_;
    std::vector<uint32_t>                  soundStops_;
    std::optional<std::pair<float, float>> musicFade_;
    uint32_t                               config153_ = 0;
    // Timed actor states (ticks left; < 0 = until an op clears it).
    struct ActorHold
    {
        uint32_t actor = 0;
        int      kind  = 0; // 0 weapon held (slot in `value`), 1 footsteps
        int      value = 0;
        float    remaining = -1;
    };
    std::vector<ActorHold>                 holds_;
    std::map<std::pair<uint32_t, int>, int> motionVariants_;
    // Op 0x7A (CODE 0x10061f00, actor +0x7a4 / +0x7a8 / +0x7b4; applied by 0x100c6e7a): the caster at the target's
    // attach point `point` plus `offset` turned by the target's yaw, for `remaining` ticks.
    struct Attach
    {
        uint32_t  actor = 0, target = 0, point = 0;
        glm::vec3 offset{ 0.0f };
        float     remaining = 0;
        std::optional<glm::vec3> at; // this frame's position
    };
    std::vector<Attach> attaches_;
    // Op 0x62 (CODE 0x1005ae5c: actor +0x870 angle left, +0x874 rate per tick): turning toward the target.
    struct Turn
    {
        uint32_t actor = 0, target = 0;
        float    yaw = 0, rate = 0, left = 0, hold = 0;
        bool     started = false;
    };
    std::vector<Turn> turns_;
    // Ops 0x6F / 0x70 (CODE 0x100608c0, tick 0x10060920): a light generator's intensity (+0x1a4) or range (+0x1a8)
    // scale from its value now to `to` over `duration` ticks.
    struct LightDrive
    {
        Generator* g = nullptr;
        bool       intensity = true;
        float      from = 1, to = 1, duration = 1, elapsed = 0;
        bool       started = false;
    };
    std::vector<LightDrive> lightDrives_;
    // Ops 0x51 / 0x72: screen colours the game keeps (0x104568fc +0x334 -> +0x31c and +0x328 -> +0x324), bytes / 255.
    struct ScreenDrive
    {
        glm::vec4 colour{ 128 / 255.0f }, from{ 128 / 255.0f }, to{ 128 / 255.0f };
        float     duration = 0, elapsed = 0;
    } screenDim_, screenAdd_{ glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f) };
    // Ticks the part-7B state (holds, attaches, turns, light drives, screen colours).
    void update7b(float ticks);
    void reset7b();
    // Adds the 0x51 / 0x72 screen layers.
    void screenLayers7b(std::vector<ScreenLayer>& out) const;
};

} // namespace xi::client
