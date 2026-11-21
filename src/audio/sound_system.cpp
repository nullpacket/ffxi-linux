// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/sound_system.h"

#include "audio/atrac3.h"
#include "xi/bytes.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace xi::audio
{

SoundSystem::SoundSystem(std::filesystem::path installDir, bool open)
    : root_(std::move(installDir))
    , mixer_(open)
{
}

void SoundSystem::setVolumes(int sound, int music)
{
    const float oldSound = soundVolume_;
    soundVolume_         = static_cast<float>(std::clamp(sound, 0, 100)) / 100.0f;
    musicVolume_         = static_cast<float>(std::clamp(music, 0, 100)) / 100.0f;
    // At once for what is playing, not only for what starts next.
    mixer_.rescale(oldSound > 0.0f ? soundVolume_ / oldSound : 0.0f, musicVolume_, soundVolume_);
}

std::shared_ptr<const dat::SeWave> SoundSystem::wave(uint32_t id)
{
    auto it = waves_.find(id);
    if (it == waves_.end())
    {
        if (waves_.size() >= 256)
        {
            waves_.clear(); // zone ambience walks through many waves; playing voices keep their own references
        }
        auto w = dat::loadSeWave(root_, id);
        if (w && w->format == 3 && !decodeAtrac3(*w))
        {
            w.reset();
        }
        it = waves_.emplace(id, w ? std::make_shared<const dat::SeWave>(std::move(*w)) : nullptr).first;
    }
    return it->second;
}

void SoundSystem::updateMusic(const client::WorldSnapshot& world, double vanaHour)
{
    if (!mixer_.hasDevice() || musicVolume_ <= 0.0f || muteBgm_)
    {
        return;
    }
    bool battle = battleBgm_ && world.engaged();
    for (const auto& e : world.entities)
    {
        if (battle)
        {
            break;
        }
        battle = battleBgm_ && !e.isPc && e.hpp > 0 && e.claimId != 0 &&
                 (e.claimId == world.charId ||
                  std::any_of(world.party.begin(), world.party.end(), [&](const client::PartyMember& m) { return m.id == e.claimId; }));
    }
    // Riding (status 5 chocobo, 85 mount): slot 4 (LSB: 0x54 mounted, else 0xD4).
    const bool riding = world.animation == 5 || world.animation == 85;
    auto       slot   = [&](std::size_t k) { return musicOverride_[k] ? musicOverride_[k] : world.music[k]; };
    uint16_t   want   = riding ? slot(4) : battle ? slot(world.party.size() > 1 ? 3 : 2) : slot(vanaHour >= 6.0 && vanaHour < 18.0 ? 0 : 1);
    want              = want ? want : slot(0);
    if (musicLoad_.valid() && musicLoad_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        auto track = musicLoad_.get();
        if (musicCache_.size() >= 3)
        {
            musicCache_.erase(musicCache_.begin());
        }
        musicCache_[musicLoading_] = track;
        spdlog::info("music: {:03} {}", musicLoading_, track ? "loaded" : "not playable (missing or undecodable)");
    }
    if (want == musicWanted_ || musicLoad_.valid())
    {
        return;
    }
    if (const auto it = musicCache_.find(want); it != musicCache_.end())
    {
        musicWanted_ = want;
        mixer_.setMusic(it->second, musicVolume_);
        mixer_.setChannelLevel(1, 1.0f, 0.0f); // a new track comes back from an effect's fade (op 0xA6; INFERRED)
    }
    else if (want != 0)
    {
        musicLoading_ = want;
        musicLoad_    = std::async(std::launch::async, [root = root_, want] {
            auto m = dat::loadBgm(root, want);
            if (m && m->format == 3 && !decodeAtrac3(*m))
            {
                m.reset();
            }
            return m ? std::make_shared<const dat::SeWave>(std::move(*m)) : std::shared_ptr<const dat::SeWave>{};
        });
    }
}

std::optional<SoundSystem::Played> SoundSystem::playSep(std::span<const std::byte> sep, const glm::vec3& at, float near, float far,
                                                        const Listener& listener, bool loop, float gain, int distanceMode, bool keepFar, uint32_t tag)
{
    if (muteSe_ || sep.size() < 0x10 || std::memcmp(sep.data(), "SeSep", 5) != 0)
    {
        return std::nullopt;
    }
    const glm::vec3 d    = at - listener.position;
    const float     ky   = distanceMode == 1 ? 1.0f : distanceMode == 2 ? 0.0f : 9.0f;
    const float     dist = std::sqrt(d.x * d.x + d.z * d.z + ky * d.y * d.y);
    if (dist > far && !keepFar)
    {
        return std::nullopt;
    }
    Played p;
    p.volume = (dist > far ? 1.0f / 127.0f : dist > near ? 1.0f - (dist - near) / std::max(far - near, 0.01f) : 1.0f) * gain;
    const float x = glm::dot(d, listener.right), z = glm::dot(d, listener.forward);
    p.pan         = std::clamp(x / std::max(std::abs(z), 1.0f), -1.0f, 1.0f) * 0.8f;
    p.id          = load<uint32_t>(sep, 8);
    p.ok          = mixer_.play(wave(p.id), p.volume * soundVolume_ * effectsLevel_, p.pan, 0.0f, loop, tag);
    return p;
}

void SoundSystem::setEventLevels(float effects, float se, float ambience)
{
    effectsLevel_ = effects;
    seLevel_      = se;
    const bool on = ambience > 0.01f;
    if (on != ambienceOn_)
    {
        ambienceOn_ = on;
        if (mixer_.hasDevice())
        {
            mixer_.setAmbience(on && ambienceId_ ? wave(ambienceId_) : nullptr, soundVolume_);
        }
    }
}

void SoundSystem::updateAmbience(std::span<const std::byte> sep)
{
    const uint32_t id = !muteSe_ && sep.size() >= 0x10 && std::memcmp(sep.data(), "SeSep", 5) == 0 ? load<uint32_t>(sep, 8) : 0u;
    if (id == ambienceId_ || !mixer_.hasDevice())
    {
        return;
    }
    ambienceId_ = id;
    mixer_.setAmbience(id && ambienceOn_ ? wave(id) : nullptr, soundVolume_);
    spdlog::debug("ambience: se{:06}", id);
}

bool SoundSystem::command(std::string_view cmd, std::string_view arg, std::string& reply)
{
    if (cmd == "/mutebgm" || cmd == "/mutese" || cmd == "/battlebgm")
    {
        bool& flag = cmd == "/mutebgm" ? muteBgm_ : cmd == "/mutese" ? muteSe_ : battleBgm_;
        flag       = arg == "on" ? true : arg == "off" ? false : !flag;
        if (cmd == "/mutebgm")
        {
            musicWanted_ = 0; // re-pick (or fade out) on the next update
            if (muteBgm_)
            {
                mixer_.setMusic(nullptr);
            }
        }
        reply = fmt::format("{}: {}", cmd.substr(1), flag ? "on" : "off");
        return true;
    }
    if (cmd == "/playse") // debug: play sound effect wave seNNNMMM.spw
    {
        uint32_t id = 0;
        std::from_chars(arg.data(), arg.data() + arg.size(), id);
        const auto w = wave(id);
        reply        = w ? fmt::format("se{:06}: {} ch, {} samples, format {}{}", id, w->channels, w->pcm.size() / w->channels, w->format,
                                       mixer_.play(w, soundVolume_) ? "" : " (not played)")
                         : fmt::format("se{:06}: not found or not decodable", id);
        return true;
    }
    return false;
}

} // namespace xi::audio
