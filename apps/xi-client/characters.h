// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/footsteps.h"
#include "client/session.h"
#include "dat/dat_index.h"
#include "dat/motion.h"
#include "gfx/character_model.h"
#include "gfx/skinned_renderer.h"
#include "gfx/texture_set.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Character models by appearance, and the motions loaded for them on demand (weapon skills, monster TP moves, emotes,
// effect and event motions). A new appearance is read on a worker thread (the DATs, the motion sets) and uploaded on the
// render thread a few per frame (docs/performance.md, goal 25); until then it draws nothing. XI_SYNC_LOADS=1 loads in
// the frame as before.
namespace xi::app
{

// A client-made look: a model DAT by id (ids[0] low, ids[1] high): the Mog House statues.
constexpr uint16_t kLookFile = 0xFF01;
// S2C 0x00E look kind 7: a chocobo (race 32..36 in word 0's low byte, then head, body, tail, legs, feet).
constexpr uint16_t kLookChocobo = 7;

// Appearance cache key; empty when the look has no character model (doors, ships, unknown kinds).
std::string lookKey(const client::Look& l);

class Characters
{
public:
    struct Entry
    {
        std::optional<gfx::CharacterModel> model;       // nullopt after a failed load
        std::vector<uint32_t>              motionFiles; // for monster TP move motions
        std::vector<uint32_t>              battleMotionFiles;
        std::vector<uint32_t>              weaponFiles;
        std::vector<uint32_t>              gearFiles; // face + armour: voices, the body's impact sound
        std::vector<uint32_t>              effectFiles; // an effects-only NPC model file whose `init` built this look
        int                                rangedClass = -1; // effect-format §C.5 `lcNN` / `lsNN`
        int                                sheathStyle = 0;  // the main weapon's (effect op 0x84)
        audio::Footsteps::Feet             feet;
        uint8_t                            bodyVariant = 0; // event motions' piece-2 file (dat-format §10.8)
    };

    Characters(const dat::DatIndex& index, gfx::TextureSet& textures, gfx::SkinnedRenderer& renderer);
    ~Characters();
    Characters(const Characters&)            = delete;
    Characters& operator=(const Characters&) = delete;

    // Start of the actors stage: uploads the looks the worker has read, within a small time budget.
    void newFrame();
    // The model for a look, requesting it when unseen (nullptr: none, failed, or still loading).
    const gfx::CharacterModel* model(const client::Look& look);
    // A look already loaded (or failed), else nullptr.
    [[nodiscard]] const Entry* find(const client::Look& look) const;

    // Weapon skill motions by (race, animation id) for PCs, TP moves from the model's files for monsters (dat-format §10.8).
    const std::vector<dat::MotionSet>* weaponSkill(const client::Look& look, uint16_t anim);
    // Emote motions by (race, emote id) (dat-format §10.9); PCs and equipped-look NPCs only.
    const dat::MotionSet* emote(const client::Look& look, uint8_t emote);
    // A motion by stem from `extraFiles` first (those present), then the actor's battle and base motion files; nullptr
    // when the look is not loaded or nothing has the stem.
    const dat::MotionSet* motion(const client::Look& look, const std::string& stem, const std::vector<uint32_t>& extraFiles = {});

    // Riding (chocobo_re.md §3d): the rider's motions for a PC look (file A = the race's riding file, + 3 in the [S]
    // zones, + 5 on a mount; piece 2 by the body variant). nullptr when the look is not loaded.
    struct Ride
    {
        dat::MotionSet idle, walk, run; // chi / wlk / run
        dat::MotionSet mount;           // chh: the hop into the saddle (scheduler `chor`)
        dat::MotionSet dismount1, dismount2; // ch1 / ch2: getting down (scheduler `coff`)
    };
    const Ride* ride(const client::Look& look, uint16_t zone, bool mount);
    // Fishing (CODE 0x100d2940 / 0x100d2af0 with offset 1): the race's fishing file (riding file + 1: schedulers
    // fsh0..fsh9, hits / hitl, motions fh0..fhd, the float) and, by the body variant, its piece-2 file. Empty when the
    // look is not a loaded PC.
    std::vector<uint32_t> fishingFiles(const client::Look& look);
    // The fishing rod, a sub-actor of its own ("サブアクター扱い(釣り竿)", CODE 0x100d339a): file u16 0x1035f5a8[race] +
    // the ranged look's index (the rods' MId 0..15: 39307.. for Hume male), holding the rod skeleton `sao `, its mesh,
    // the fsh schedulers and the rod's own fh?0 / idl0 motions, which are posed in the owner's model space. The look of
    // that file for a PC look; nullopt when the look is not a PC.
    [[nodiscard]] static std::optional<client::Look> rodLook(const client::Look& look);
    // The stem a MotionSet from motion() was loaded by ("fh4"), or empty.
    [[nodiscard]] std::string stemOf(const dat::MotionSet* set) const;
    // What a rider sits on (chocobo_re.md §3c): a rented chocobo (file 39275, 39276 in the [S] zones), an own / Noble
    // chocobo from its looks (race 32..36), or a mount (102705 + index - 1).
    [[nodiscard]] static client::Look mountLook(uint8_t animation, uint8_t chocoboIndex, const std::array<uint32_t, 2>& looks, uint8_t mountIndex,
                                                uint16_t zone);

private:
    // A look read on the worker, ready for its GPU upload.
    struct Prepared
    {
        std::string                         key;
        std::optional<dat::CharacterAsset>  asset;
        gfx::CharacterUpload                upload; // decoded textures, vertices
        dat::CharacterMotions               motions;
        int                                 drawnWeaponBone = -1;
        double                              datMs = 0.0, motionMs = 0.0, cpuMs = 0.0;
        std::string                         error;
    };
    // A look whose upload is on the GPU: installed into cache_ once the ticket is done.
    struct Uploading
    {
        std::string key;
        Entry       entry;
        uint64_t    ticket = 0;
    };
    Prepared prepare(const std::string& key, const client::Look& look); // worker side (or in the frame when synchronous)
    void     upload(Prepared&& p);                                       // render thread: start the GPU upload
    void     work();
    std::vector<Uploading> uploading_;

    const dat::DatIndex&                                         index_;
    gfx::TextureSet&                                             textures_;
    gfx::SkinnedRenderer&                                        renderer_;
    std::unordered_map<std::string, Entry>                       cache_;
    std::unordered_map<std::string, dat::MotionSet>              motions_;   // lookKey#stem@files
    std::unordered_map<uint32_t, std::vector<dat::MotionSet>>    wsMotions_; // race << 16 | anim
    std::unordered_map<std::string, std::vector<dat::MotionSet>> mobSkills_; // lookKey#anim
    std::unordered_map<uint32_t, dat::MotionSet>                 emotes_;    // race << 8 | emote
    std::unordered_map<std::string, Ride>                        rides_;     // lookKey#offset
    std::unordered_map<std::string, std::shared_ptr<const dat::CharacterMotions>> characterMotions_; // worker only: files|battle|sheath
    // Loader: requests (render -> worker) and prepared looks (worker -> render), both under mutex_.
    bool                                                         sync_ = false;
    std::unordered_set<std::string>                              pending_; // render thread: requested, not yet installed
    std::mutex                                                   mutex_;
    std::condition_variable                                      wake_;
    std::deque<std::pair<std::string, client::Look>>             requests_;
    std::deque<Prepared>                                         ready_;
    bool                                                         stop_ = false;
    std::thread                                                  worker_;
};

} // namespace xi::app
