// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/sound.h"

#include "xi/bytes.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

#include <fmt/format.h>

namespace xi::dat
{
namespace
{

// PlayStation ADPCM prediction filters (k0, k1) / 64; the block header's high nibble picks one (DATA: always 0..4).
constexpr std::array<std::array<int, 2>, 5> kFilters{ { { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 } } };

// One block (1 header byte: filter << 4 | shift, then n/2 data bytes) -> n samples; s1 / s2 carry this channel's
// history. CODE 0x10246610: low nibble first, no clamping (the output is truncated to 16 bits, the history keeps
// the full value).
void decodeBlock(std::span<const std::byte> b, std::size_t n, int& s1, int& s2, int16_t* out, std::size_t stride)
{
    const auto h      = static_cast<uint8_t>(b[0]);
    const int  shift  = h & 15;
    const auto filter = kFilters[std::min<std::size_t>(h >> 4, kFilters.size() - 1)];
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto byte = static_cast<uint8_t>(b[1 + i / 2]);
        const int  nib  = (i % 2 == 0) ? (byte & 15) : (byte >> 4);
        const int  v    = ((s1 * filter[0] + s2 * filter[1]) >> 6) + (static_cast<int16_t>(static_cast<uint16_t>(nib << 12)) >> shift);
        out[i * stride] = static_cast<int16_t>(v);
        s2 = s1;
        s1 = v;
    }
}

} // namespace

namespace
{
// Field offsets: SeWave (.spw) and BGMStream (.bgw) share the layout, the music header 4 bytes further on (DATA).
struct Layout
{
    std::size_t format, id, length, loop, rate, header, channels, blockSamples;
};
constexpr Layout kSeWave{ 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x24, 0x2A, 0x2C };
constexpr Layout kBgm{ 0x0C, 0x14, 0x18, 0x1C, 0x20, 0x28, 0x2E, 0x2F };

std::optional<SeWave> decodeWave(std::span<const std::byte> file, const Layout& at)
{
    SeWave w;
    w.format          = load<uint32_t>(file, at.format);
    w.id              = load<uint32_t>(file, at.id);
    const auto length = load<uint32_t>(file, at.length); // ADPCM: blocks; PCM: frames
    w.loopStart       = load<int32_t>(file, at.loop);
    // The rate is split over two words (CODE 0x1024a300; 48000 in every shipped file, DATA).
    w.sampleRate      = (load<uint32_t>(file, at.rate) + load<uint32_t>(file, at.rate + 4)) & 0x7FFFFFFF;
    const auto header = load<uint32_t>(file, at.header);
    w.channels        = std::max<uint32_t>(1, static_cast<uint8_t>(file[at.channels]));
    if (header < 0x30 || header > file.size() || w.channels > 2)
    {
        return std::nullopt;
    }
    const auto data = file.subspan(header);
    if (w.format == 0)
    {
        // Blocks of 0x2C samples (16 normally; 4 and 8 occur), 0x2C / 2 + 1 bytes each; stereo = L block, R block.
        const std::size_t n     = std::max<std::size_t>(2, static_cast<uint8_t>(file[at.blockSamples]) & ~1u);
        const std::size_t bytes = n / 2 + 1;
        const std::size_t count = std::min<std::size_t>(length, data.size() / (bytes * w.channels));
        w.pcm.resize(count * n * w.channels);
        std::array<int, 2> s1{}, s2{};
        for (std::size_t k = 0; k < count; ++k)
        {
            for (uint32_t c = 0; c < w.channels; ++c)
            {
                decodeBlock(data.subspan((k * w.channels + c) * bytes, bytes), n, s1[c], s2[c], w.pcm.data() + k * n * w.channels + c, w.channels);
            }
        }
        if (w.loopStart >= 0)
        {
            w.loopStart *= static_cast<int32_t>(n);
        }
    }
    else if (w.format == 1 || w.format == 2) // PCM s16, little / big endian
    {
        const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(length) * w.channels, data.size() / 2);
        w.pcm.resize(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto lo = static_cast<uint8_t>(data[2 * i]), hi = static_cast<uint8_t>(data[2 * i + 1]);
            w.pcm[i]      = static_cast<int16_t>(w.format == 1 ? (hi << 8 | lo) : (lo << 8 | hi));
        }
    }
    else if (w.format == 3)
    {
        // ATRAC3 under an XOR key (CODE 0x10246550 / 0x10246d90): s = rotl(seed @rate+4, 11); key[i] = s, s += rotl(s, 3);
        // each frame XORs from key[0]. Stereo frames are 384 bytes; mono 192, doubled to 384 (DATA: first byte 0xA0).
        auto rotl = [](uint32_t v, int r) { return (v << r) | (v >> (32 - r)); };
        std::array<uint32_t, 100> key{};
        uint32_t                  k = rotl(load<uint32_t>(file, at.rate + 4), 11);
        for (auto& e : key)
        {
            e = k;
            k += rotl(k, 3);
        }
        const std::size_t frame = w.channels == 2 ? 384 : 192;
        const std::size_t count = data.size() / frame;
        w.frames                = length;
        w.atrac3.resize(count * 384);
        for (std::size_t f = 0; f < count; ++f)
        {
            std::byte* out = w.atrac3.data() + f * 384;
            for (std::size_t i = 0; i < frame / 4; ++i)
            {
                const uint32_t v = load<uint32_t>(data, f * frame + 4 * i) ^ key[i];
                std::memcpy(out + 4 * i, &v, 4);
            }
            if (frame == 192)
            {
                std::memcpy(out + 192, out, 192);
            }
        }
    }
    else
    {
        return std::nullopt;
    }
    return w;
}

} // namespace

std::optional<SeWave> decodeSeWave(std::span<const std::byte> file)
{
    if (file.size() < 0x30 || std::memcmp(file.data(), "SeWave", 6) != 0)
    {
        return std::nullopt;
    }
    auto w = decodeWave(file, kSeWave);
    if (w)
    {
        w->fadeIn = (static_cast<uint8_t>(file[0x29]) & 0x20) != 0; // 115 of 4,685 files (DATA)
    }
    return w;
}

std::optional<SeWave> decodeBgm(std::span<const std::byte> file)
{
    if (file.size() < 0x30 || std::memcmp(file.data(), "BGMStream", 9) != 0)
    {
        return std::nullopt;
    }
    return decodeWave(file, kBgm);
}

std::filesystem::path seWavePath(const std::filesystem::path& installDir, uint32_t id)
{
    return installDir / "sound" / "win" / "se" / fmt::format("se{:03}", id / 1000) / fmt::format("se{:06}.spw", id);
}

namespace
{
std::vector<char> readAll(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return in ? std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::vector<char>{};
}
} // namespace

std::optional<SeWave> loadBgm(const std::filesystem::path& installDir, uint32_t id)
{
    const auto raw = readAll(installDir / "sound" / "win" / "music" / "data" / fmt::format("music{:03}.bgw", id));
    return decodeBgm(std::as_bytes(std::span(raw)));
}

std::optional<SeWave> loadSeWave(const std::filesystem::path& installDir, uint32_t id)
{
    std::ifstream in(seWavePath(installDir, id), std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decodeSeWave(std::as_bytes(std::span(raw)));
}

} // namespace xi::dat
