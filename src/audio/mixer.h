// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/sound.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct SDL_AudioStream;

// Sound output: decoded SeWave voices mixed to stereo float at kRate through an SDL3 audio stream.
namespace xi::audio
{

class Mixer
{
public:
    static constexpr int kRate = 48000;

    // `open` = false builds a mixer without a device (tests call mix() directly).
    explicit Mixer(bool open = true);
    ~Mixer();
    Mixer(const Mixer&)            = delete;
    Mixer& operator=(const Mixer&) = delete;

    [[nodiscard]] bool hasDevice() const
    {
        return stream_ != nullptr;
    }
    // Starts a voice. volume 0..1, pan -1 (left) .. 1 (right); `rate` = the wave's sample rate (0: its own, else
    // kDefaultWaveRate). Returns false when there is nothing to play.
    // `loop` = false plays a looping wave once (to its end).
    // `tag` != 0 marks the voice for stopTagged().
    bool play(std::shared_ptr<const dat::SeWave> wave, float volume = 1.0f, float pan = 0.0f, float rate = 0.0f, bool loop = true, uint32_t tag = 0);
    // Fades out (over ~50 ms) every voice started with `tag` (effect op 0x8B).
    void stopTagged(uint32_t tag);
    // A channel's level (1 music, 2 ambience), 0..1, reached linearly over `seconds` (effect op 0xA6); it stays.
    void setChannelLevel(uint8_t channel, float level, float seconds);
    // Background music: loops from the wave's loop start; a new track cross-fades over ~1 s. nullptr fades out.
    void setMusic(std::shared_ptr<const dat::SeWave> track, float volume = 0.5f)
    {
        setChannel(1, std::move(track), volume, 1.0f);
    }
    // The zone's ambience loop (weather / time of day): one at a time, replaced with a short fade (sound-format.md).
    void setAmbience(std::shared_ptr<const dat::SeWave> loop, float volume)
    {
        setChannel(2, std::move(loop), volume, 0.5f);
    }
    // Volume changes reach the voices already playing (the Config sliders): sound effects scaled by `sfxRatio`, the
    // music channel set to `music`, the ambience channel to `ambience`.
    void rescale(float sfxRatio, float music, float ambience);
    void setMasterVolume(float v)
    {
        master_ = v;
    }
    // Adds `frames` stereo frames of every voice into out (interleaved L R); finished voices are dropped.
    void mix(float* out, std::size_t frames);
    [[nodiscard]] std::size_t voices() const;

    static constexpr float kDefaultWaveRate = 22050.0f; // when the wave does not say (INFERRED; see sound-format.md)

private:
    struct Voice
    {
        std::shared_ptr<const dat::SeWave> wave;
        double                             pos  = 0; // in source frames
        double                             step = 1; // source frames per output frame
        float                              left = 1, right = 1;
        float                              gain = 1, fade = 0; // music: per-frame gain change (cross-fade)
        bool                               music = false;   // a channel voice (music, ambience): never dropped
        uint8_t                            channel = 0;     // 1 music, 2 ambience
        bool                               loop  = true;
        uint32_t                           tag   = 0;
    };
    static void callback(void* self, SDL_AudioStream* stream, int additional, int total);
    // A long-lived looping voice on `channel`: the previous one fades out over `seconds` while this fades in.
    void setChannel(uint8_t channel, std::shared_ptr<const dat::SeWave> wave, float volume, float seconds);

    SDL_AudioStream*   stream_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<Voice> voices_;
    std::vector<float> scratch_;
    float              master_ = 0.8f;
    struct Level
    {
        float now = 1.0f, target = 1.0f, step = 0.0f; // step per output frame
    };
    std::array<Level, 3> channelLevel_{};
};

} // namespace xi::audio
