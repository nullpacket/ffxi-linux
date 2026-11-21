// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

// Sound effect waves (docs/sound-format.md): sound/win/se/seNNN/seNNNMMM.spw, an `SeWave` header and 4-bit ADPCM
// blocks of 16 samples (1 header byte: filter << 4 | shift, then 8 data bytes) per channel.
namespace xi::dat
{

struct SeWave
{
    uint32_t             id         = 0;
    uint32_t             format     = 0; // 0 = ADPCM; others are not decoded
    uint32_t             channels   = 1;
    uint32_t             sampleRate = 0; // 0 = unknown (the caller picks)
    int32_t              loopStart  = -1; // in samples per channel; -1 = plays once
    bool                 fadeIn     = false; // .spw flag 0x29 & 0x20: rises over 150 ticks (CODE 0x1024a351)
    std::vector<int16_t> pcm;             // interleaved by channel
    // Format 3: the ATRAC3 frames, unscrambled, 384 bytes each (mono frames duplicated, as the client does); pcm is
    // empty until an ATRAC3 decoder fills it (audio::decodeAtrac3). `frames` = sample frames per channel (+0x14).
    std::vector<std::byte> atrac3;
    uint32_t               frames = 0;
};

// Parses and decodes a whole .spw file; nullopt when it is not an SeWave or not ADPCM.
std::optional<SeWave> decodeSeWave(std::span<const std::byte> file);
// seNNN/seNNNMMM.spw under <install>/sound/win/se (NNN = id / 1000).
std::filesystem::path seWavePath(const std::filesystem::path& installDir, uint32_t id);
std::optional<SeWave> loadSeWave(const std::filesystem::path& installDir, uint32_t id);
// Background music: sound/win/music/data/musicNNN.bgw, a `BGMStream` header (the SeWave fields 4 bytes on) and the
// same ADPCM (44100 or 48000 Hz, stereo, 16-128 samples per block). Format 3 (ATRAC3) tracks return nullopt.
std::optional<SeWave> decodeBgm(std::span<const std::byte> file);
std::optional<SeWave> loadBgm(const std::filesystem::path& installDir, uint32_t id);

} // namespace xi::dat
