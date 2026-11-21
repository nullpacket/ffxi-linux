// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/install.h"
#include "audio/mixer.h"
#include "audio/sound_system.h"
#include "dat/sound.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

TEST_CASE("sound: an SeWave with one ADPCM block decodes to 16 samples")
{
    std::vector<std::byte> f(0x30 + 9);
    std::memcpy(f.data(), "SeWave", 6);
    auto put = [&](std::size_t at, uint32_t v) { std::memcpy(f.data() + at, &v, 4); };
    put(0x08, static_cast<uint32_t>(f.size()));
    put(0x0C, 0);       // ADPCM
    put(0x10, 1234);    // id
    put(0x14, 1);       // one block
    put(0x18, 0xFFFFFFFF);
    put(0x1C, 47000);
    put(0x20, 1000); // rate = 0x1C + 0x20
    put(0x24, 0x30);
    f[0x2A] = std::byte{ 1 };
    f[0x2C] = std::byte{ 16 };
    f[0x30] = std::byte{ 0x0C }; // filter 0, shift 12: a nibble n decodes to n
    f[0x31] = std::byte{ 0x71 }; // samples 1 then 7 (low nibble first)
    f[0x32] = std::byte{ 0x0F }; // -1 then 0
    const auto w = xi::dat::decodeSeWave(f);
    REQUIRE(w);
    CHECK(w->id == 1234);
    REQUIRE(w->pcm.size() == 16);
    CHECK(w->pcm[0] == 1);
    CHECK(w->pcm[1] == 7);
    CHECK(w->pcm[2] == -1);
    CHECK(w->pcm[3] == 0);
    CHECK(w->loopStart == -1);
    CHECK(w->sampleRate == 48000);
}

TEST_CASE("sound: the mixer plays a voice to the end, panned")
{
    auto w        = std::make_shared<xi::dat::SeWave>();
    w->sampleRate = xi::audio::Mixer::kRate; // one source frame per output frame
    w->pcm.assign(100, 16384);               // 0.5
    xi::audio::Mixer m(false);
    m.setMasterVolume(1.0f);
    REQUIRE(m.play(w, 1.0f, 1.0f)); // hard right
    std::vector<float> out(2 * 64, 0.0f);
    m.mix(out.data(), 64);
    CHECK(out[0] == 0.0f);
    CHECK(out[1] == 0.5f);
    CHECK(m.voices() == 1);
    std::vector<float> rest(2 * 64, 0.0f);
    m.mix(rest.data(), 64);
    CHECK(m.voices() == 0);
}

TEST_CASE("sound: a volume change reaches the voices already playing")
{
    auto w        = std::make_shared<xi::dat::SeWave>();
    w->sampleRate = xi::audio::Mixer::kRate;
    w->pcm.assign(1000, 16384); // 0.5
    xi::audio::Mixer m(false);
    m.setMasterVolume(1.0f);
    REQUIRE(m.play(w, 1.0f));
    m.rescale(0.5f, 0.0f, 0.0f);
    std::vector<float> out(2 * 16, 0.0f);
    m.mix(out.data(), 16);
    CHECK(out[0] == 0.25f);
    CHECK(out[1] == 0.25f);
}

TEST_CASE("sound: real .spw files decode")
{
    const char* root = xi::install::gameRootOrNull();
    if (!root)
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto w = xi::dat::loadSeWave(root, 18051); // se018/se018051.spw: 8768 blocks, mono (DATA)
    REQUIRE(w);
    CHECK(w->channels == 1);
    CHECK(w->pcm.size() == 8768u * 16u);
    CHECK(w->sampleRate == 48000);
}

TEST_CASE("sound: background music decodes (BGMStream)")
{
    const char* root = xi::install::gameRootOrNull();
    if (!root)
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto m = xi::dat::loadBgm(root, 23); // music023.bgw: 58661 blocks of 128, stereo, 44100 Hz, loop at block 3535 (DATA)
    REQUIRE(m);
    CHECK(m->channels == 2);
    CHECK(m->sampleRate == 44100);
    CHECK(m->pcm.size() == 58661u * 128u * 2u);
    CHECK(m->loopStart == 3535 * 128);
}

#include "audio/atrac3.h"
#include <cmath>

TEST_CASE("sound: ATRAC3 waves decode (format 3)")
{
    const char* root = xi::install::gameRootOrNull();
    if (!root)
    {
        SKIP("FFXI_ROOT not set");
    }
    auto mono = xi::dat::loadSeWave(root, 41); // se000/se000041.spw: mono ATRAC3, 61440 frames (DATA)
    REQUIRE(mono);
    REQUIRE(mono->format == 3);
    REQUIRE(xi::audio::decodeAtrac3(*mono));
    CHECK(mono->pcm.size() == 61440u);
    auto music = xi::dat::loadBgm(root, 52); // music052.bgw: stereo ATRAC3, 4028416 frames
    REQUIRE(music);
    REQUIRE(xi::audio::decodeAtrac3(*music));
    CHECK(music->pcm.size() == 2u * 4028416u);
    auto joint = xi::dat::loadSeWave(root, 41);
    CHECK_FALSE(xi::audio::decodeAtrac3(*joint, true)); // joint stereo does not fit these frames
}

TEST_CASE("sound: a fade-in wave starts silent and rises")
{
    auto w        = std::make_shared<xi::dat::SeWave>();
    w->sampleRate = xi::audio::Mixer::kRate;
    w->fadeIn     = true;
    w->pcm.assign(xi::audio::Mixer::kRate * 3, 16384);
    xi::audio::Mixer m(false);
    m.setMasterVolume(1.0f);
    REQUIRE(m.play(w));
    std::vector<float> out(2 * xi::audio::Mixer::kRate, 0.0f); // the first second
    m.mix(out.data(), xi::audio::Mixer::kRate);
    CHECK(out[0] < 0.001f);
    CHECK(out[out.size() - 1] > 0.15f); // 1 s of 2.5 s: about 0.4 x 0.5
    CHECK(out[out.size() - 1] < 0.25f);
}

TEST_CASE("sound: an effect sound's distance by mode (sound particles' p+0x1e9 / p+0x1e8, CODE 0x10036ba0)")
{
    xi::audio::SoundSystem snd(std::filesystem::path("/nonexistent"), false);
    std::vector<std::byte> sep(0x10, std::byte{ 0 });
    std::memcpy(sep.data(), "SeSep", 5);
    const xi::audio::SoundSystem::Listener ear{ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
    const glm::vec3                        at(0, 10, 10); // render space: 10 up, 10 ahead
    // Mode 0: the vertical part counts 3 times (sqrt(100 + 900) = 31.6, beyond 30).
    CHECK(!snd.playSep(sep, at, 3.0f, 30.0f, ear, false, 1.0f, 0));
    // Mode 1 (plain, L1 0x92 / a world-placed 0x4c): 14.1 -> 1 - (14.1 - 3) / 27.
    const auto plain = snd.playSep(sep, at, 3.0f, 30.0f, ear, false, 1.0f, 1);
    REQUIRE(plain);
    CHECK(plain->volume == Catch::Approx(1.0f - (std::sqrt(200.0f) - 3.0f) / 27.0f));
    // Mode 2 (across the ground, L1 0x9e): 10.
    const auto ground = snd.playSep(sep, at, 3.0f, 30.0f, ear, false, 1.0f, 2);
    REQUIRE(ground);
    CHECK(ground->volume == Catch::Approx(1.0f - 7.0f / 27.0f));
    // L1 0x8a: out of range it still plays, at volume 1 / 127.
    const auto far = snd.playSep(sep, { 0, 0, 100 }, 3.0f, 30.0f, ear, false, 1.0f, 1, true);
    REQUIRE(far);
    CHECK(far->volume == Catch::Approx(1.0f / 127.0f));
    CHECK(!snd.playSep(sep, { 0, 0, 100 }, 3.0f, 30.0f, ear, false, 1.0f, 1, false));
}
