// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/mixer.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace xi::audio
{

Mixer::Mixer(bool open)
{
    if (!open)
    {
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        spdlog::warn("audio: SDL_InitSubSystem: {}", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{ SDL_AUDIO_F32, 2, kRate };
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Mixer::callback, this);
    if (!stream_)
    {
        spdlog::warn("audio: no output device: {}", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    spdlog::info("audio: {} Hz stereo on {}", kRate, SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
}

Mixer::~Mixer()
{
    if (stream_)
    {
        SDL_DestroyAudioStream(stream_);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

bool Mixer::play(std::shared_ptr<const dat::SeWave> wave, float volume, float pan, float rate, bool loop, uint32_t tag)
{
    if (!wave || wave->channels == 0 || wave->pcm.size() / wave->channels < 2 || volume <= 0.0f)
    {
        return false;
    }
    const float src = rate > 0 ? rate : wave->sampleRate > 0 ? static_cast<float>(wave->sampleRate) : kDefaultWaveRate;
    Voice       v;
    v.wave  = std::move(wave);
    v.step  = static_cast<double>(src) / kRate;
    v.loop  = loop;
    v.tag   = tag;
    pan     = std::clamp(pan, -1.0f, 1.0f);
    v.left  = volume * std::min(1.0f, 1.0f - pan);
    v.right = volume * std::min(1.0f, 1.0f + pan);
    if (v.wave->fadeIn)
    {
        v.gain = 0.0f;
        v.fade = 1.0f / (2.5f * kRate); // 150 ticks
    }
    std::lock_guard lock(mutex_);
    // At most 20 sound effects at once, as the game (CODE: the cap variable 0x10456a94); drop the oldest.
    if (std::count_if(voices_.begin(), voices_.end(), [](const Voice& o) { return !o.music; }) >= 20)
    {
        // Drop the oldest sound effect (never the music).
        if (const auto it = std::find_if(voices_.begin(), voices_.end(), [](const Voice& o) { return !o.music; }); it != voices_.end())
        {
            voices_.erase(it);
        }
    }
    voices_.push_back(std::move(v));
    return true;
}

void Mixer::setChannel(uint8_t channel, std::shared_ptr<const dat::SeWave> track, float volume, float seconds)
{
    std::lock_guard lock(mutex_);
    const float fade = 1.0f / (kRate * std::max(seconds, 0.01f));
    for (auto& v : voices_)
    {
        if (v.music && v.channel == channel)
        {
            v.fade = -fade; // fade the old one out
        }
    }
    if (!track || track->channels == 0 || track->pcm.size() / track->channels < 2)
    {
        return;
    }
    Voice v;
    v.wave  = std::move(track);
    v.step  = static_cast<double>(v.wave->sampleRate > 0 ? v.wave->sampleRate : 44100) / kRate;
    v.left  = v.right = volume;
    v.gain  = 0.0f;
    v.fade  = fade;
    v.music = true;
    v.channel = channel;
    voices_.push_back(std::move(v));
}

void Mixer::rescale(float sfxRatio, float music, float ambience)
{
    std::lock_guard lock(mutex_);
    for (auto& v : voices_)
    {
        if (v.music)
        {
            v.left = v.right = v.channel == 1 ? music : ambience;
        }
        else
        {
            v.left *= sfxRatio;
            v.right *= sfxRatio;
        }
    }
}

void Mixer::stopTagged(uint32_t tag)
{
    std::lock_guard lock(mutex_);
    for (auto& v : voices_)
    {
        if (tag != 0 && v.tag == tag)
        {
            v.fade = -1.0f / (0.05f * kRate);
        }
    }
}

void Mixer::setChannelLevel(uint8_t channel, float level, float seconds)
{
    std::lock_guard lock(mutex_);
    if (channel >= channelLevel_.size())
    {
        return;
    }
    auto& l  = channelLevel_[channel];
    l.target = std::clamp(level, 0.0f, 1.0f);
    l.step   = seconds > 0.0f ? std::abs(l.target - l.now) / (seconds * kRate) : 1.0f;
}

std::size_t Mixer::voices() const
{
    std::lock_guard lock(mutex_);
    return voices_.size();
}

void Mixer::mix(float* out, std::size_t frames)
{
    std::lock_guard lock(mutex_);
    for (auto& l : channelLevel_) // channel levels move once per block (effect op 0xA6)
    {
        const float d = l.step * static_cast<float>(frames);
        l.now         = l.now < l.target ? std::min(l.now + d, l.target) : std::max(l.now - d, l.target);
    }
    for (auto& v : voices_)
    {
        const float level = v.music && v.channel < channelLevel_.size() ? channelLevel_[v.channel].now : 1.0f;
        const auto&       pcm    = v.wave->pcm;
        const std::size_t ch     = v.wave->channels;
        const std::size_t total  = pcm.size() / ch;
        const bool        loops  = v.loop && v.wave->loopStart >= 0 && static_cast<std::size_t>(v.wave->loopStart) < total;
        for (std::size_t f = 0; f < frames; ++f)
        {
            if (v.pos >= static_cast<double>(total - 1))
            {
                if (!loops)
                {
                    v.pos = static_cast<double>(total); // done
                    break;
                }
                v.pos -= static_cast<double>(total - 1 - static_cast<std::size_t>(v.wave->loopStart));
            }
            const auto   i = static_cast<std::size_t>(v.pos);
            const double t = v.pos - static_cast<double>(i);
            auto sample    = [&](std::size_t c) {
                const double a = pcm[i * ch + c], b = pcm[(i + 1) * ch + c];
                return static_cast<float>((a + (b - a) * t) / 32768.0);
            };
            const float l = sample(0), r = ch > 1 ? sample(1) : l;
            if (v.fade != 0.0f)
            {
                v.gain = std::clamp(v.gain + v.fade, 0.0f, 1.0f);
                if (v.gain <= 0.0f && v.fade < 0)
                {
                    v.pos = static_cast<double>(total); // faded out
                    break;
                }
            }
            out[2 * f] += l * v.left * v.gain * level * master_;
            out[2 * f + 1] += r * v.right * v.gain * level * master_;
            v.pos += v.step;
        }
    }
    std::erase_if(voices_, [](const Voice& v) {
        const std::size_t total = v.wave->pcm.size() / v.wave->channels; // whole frames
        return v.pos >= static_cast<double>(total);
    });
}

void Mixer::callback(void* self, SDL_AudioStream* stream, int additional, int /*total*/)
{
    auto*             m      = static_cast<Mixer*>(self);
    const std::size_t frames = static_cast<std::size_t>(std::max(additional, 0)) / (2 * sizeof(float));
    if (frames == 0)
    {
        return;
    }
    m->scratch_.assign(frames * 2, 0.0f);
    m->mix(m->scratch_.data(), frames);
    for (auto& s : m->scratch_)
    {
        s = std::clamp(s, -1.0f, 1.0f);
    }
    SDL_PutAudioStreamData(stream, m->scratch_.data(), static_cast<int>(frames * 2 * sizeof(float)));
}

} // namespace xi::audio
