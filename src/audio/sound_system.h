// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/mixer.h"
#include "client/session.h"
#include "dat/sound.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

// The client's sound (docs/sound-format.md): the mixer, decoded-wave cache, zone music selection and loading, and
// positional effect sounds. Owned by the client loop; everything runs on the main thread except music decoding.
namespace xi::audio
{

class SoundSystem
{
public:
    // `open` = false runs without an output device (XI_NO_AUDIO).
    SoundSystem(std::filesystem::path installDir, bool open);

    // 0..100 each (client.ini `sound_volume` / `music_volume`).
    void setVolumes(int sound, int music);

    // seNNNMMM.spw, decoded once (ATRAC3 included); nullptr when missing or undecodable.
    std::shared_ptr<const dat::SeWave> wave(uint32_t id);

    // Picks the zone track (S2C 0x00A / 0x05F ids): riding, battle (engaged, or a monster claimed by us or the party;
    // INFERRED trigger), else day 06:00-18:00 (INFERRED) or night. Loads it on a worker thread and cross-fades.
    void updateMusic(const client::WorldSnapshot& world, double vanaHour);
    // The zone's ambience loop (EffectSystem::ambienceSep): centred, looping, replaced when the Sep changes.
    void updateAmbience(std::span<const std::byte> sep);
    // An event's music slots (op 0x5C: slot k = track; 0 clears): they replace the zone's while set.
    void setMusicOverride(int slot, uint16_t track)
    {
        if (slot >= 0 && slot < static_cast<int>(musicOverride_.size()))
        {
            musicOverride_[static_cast<std::size_t>(slot)] = track;
        }
    }
    void clearMusicOverrides()
    {
        musicOverride_ = {};
    }
    // An event's channel levels (ops 0x69 / 0x6A, 0..1): zone effect sounds (playSep), sound effects (playUi) and
    // the ambience loop (off below 0.01, else on at the normal volume).
    void setEventLevels(float effects, float se, float ambience);

    // The listener: the camera, in render space.
    struct Listener
    {
        glm::vec3 position{ 0.0f }, right{ 1, 0, 0 }, forward{ 0, 0, 1 };
    };
    struct Played
    {
        uint32_t id     = 0;
        float    volume = 0, pan = 0;
        bool     ok     = false;
    };
    // An effect's Sep (ops 0x0A / 0x0B) at `at` (render space): the wave named by its id (+8; the PC client never runs
    // the SeSep program, CODE 0x10245b90); linear near..far falloff with the vertical distance x3 and a view-space pan
    // (CODE 0x10036ba0; the pan scale is INFERRED). nullopt when not a Sep, muted or out of range.
    // `loop` = false: a looping wave plays once (zone ambience re-triggers it per particle). `gain` scales the volume
    // (footsteps: 0.8 walking). `distanceMode` (CODE 0x10036ba0, sound particle p+0x1e9): 0 the vertical part x3, 1
    // plain, 2 across the ground only; `keepFar` (p+0x1e8): beyond `far` it still plays at volume 1 / 127.
    // `tag` != 0 lets stopTagged() end it (effect ops 0x8A / 0x8B).
    std::optional<Played> playSep(std::span<const std::byte> sep, const glm::vec3& at, float near, float far, const Listener& listener,
                                  bool loop = true, float gain = 1.0f, int distanceMode = 0, bool keepFar = false, uint32_t tag = 0);
    void stopTagged(uint32_t tag)
    {
        mixer_.stopTagged(tag);
    }
    // The music's level 0..1 over `seconds` (effect op 0xA6); the next track starts at the full level.
    void fadeMusic(float level, float seconds)
    {
        mixer_.setChannelLevel(1, level, seconds);
    }

    // An interface sound, centred (se000/se0000NN.spw: cursor 1, confirm 2, cancel 3, error 4, menus 9/10/11, tell 40).
    void playUi(uint32_t id)
    {
        if (!muteSe_)
        {
            mixer_.play(wave(id), soundVolume_ * seLevel_);
        }
    }
    // /mutebgm, /mutese, /battlebgm (on / off / toggle) and /playse ID. True when `cmd` is one of them; `reply` is the
    // line to show.
    bool command(std::string_view cmd, std::string_view arg, std::string& reply);

    [[nodiscard]] bool hasDevice() const
    {
        return mixer_.hasDevice();
    }

private:
    std::filesystem::path                                            root_;
    Mixer                                                            mixer_;
    std::unordered_map<uint32_t, std::shared_ptr<const dat::SeWave>> waves_;
    std::future<std::shared_ptr<const dat::SeWave>>                  musicLoad_;
    std::map<uint16_t, std::shared_ptr<const dat::SeWave>>           musicCache_;
    uint16_t                                                         musicWanted_ = 0, musicLoading_ = 0;
    std::array<uint16_t, 5>                                          musicOverride_{};
    uint32_t                                                         ambienceId_ = 0;
    bool                                                             muteBgm_ = false, muteSe_ = false, battleBgm_ = true;
    float                                                            soundVolume_ = 0.8f, musicVolume_ = 0.5f;
    float                                                            effectsLevel_ = 1.0f, seLevel_ = 1.0f;
    bool                                                             ambienceOn_   = true;
};

} // namespace xi::audio
