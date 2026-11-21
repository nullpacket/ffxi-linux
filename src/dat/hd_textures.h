// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/texture.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// HD textures (goal 41, option 4, docs/enhanced-graphics.md): AI-upscaled copies of the game's textures, built on this
// machine from the user's own DATs by `xi-dat hdtex build` into a cache outside the repository, and swapped in at load
// time by `--hd-textures[=DIR]`. Nothing of it is ever committed.
namespace xi::dat::hd
{

struct Image
{
    uint32_t             width = 0, height = 0;
    std::vector<uint8_t> rgba; // tightly packed RGBA8, the same value scale as toRgba (0x80 = full / opaque)
};

// The cache key of a texture: its name (sanitised) and a 64-bit FNV-1a hash of its size, format and pixel data, so
// the same picture in two DAT files shares one entry and a changed texture gets a new one.
[[nodiscard]] std::string key(const Texture& tex);

// $XDG_CACHE_HOME/ffxi-linux/hd-textures (else ~/.cache/ffxi-linux/hd-textures).
[[nodiscard]] std::filesystem::path defaultDir();

// The colour file of a key (`suffix` "" ), or its generated normal ("_n") / roughness ("_r") map.
[[nodiscard]] std::filesystem::path entryPath(const std::filesystem::path& dir, const std::string& key, const char* suffix = "");
// Loads a cache entry (any thread); nullopt when it is missing or unreadable.
[[nodiscard]] std::optional<Image> load(const std::filesystem::path& dir, const std::string& key, const char* suffix = "");
// Reads any PNG (8-bit RGBA result).
[[nodiscard]] std::optional<Image> readPng(const std::filesystem::path& file);

// Runtime switch (--hd-textures): the cache the client reads from, unset = off (textures exactly as before).
void                                        setRuntimeDir(std::optional<std::filesystem::path> dir, bool dumpMissing);
[[nodiscard]] const std::optional<std::filesystem::path>& runtimeDir();
// With dumping on (XI_HDTEX_DUMP=1), textures the client loads that are not in the cache are written to
// <dir>/pending/<key>.png, so `xi-dat hdtex build --pending` can upscale exactly what play has shown.
[[nodiscard]] bool dumpMissing();

// --- build helpers (pure; unit-tested) ---

// True when the alpha channel is a cut-out (at most two distinct values, e.g. 0 / 0x80): such alpha is resized with
// a threshold so the edges stay crisp; otherwise it is resized smoothly.
[[nodiscard]] bool isCutoutAlpha(const Image& img);
// Bilinear resize of one channel (`channel` 0..3) of `src` into `dst` (already sized); `threshold` >= 0 snaps each
// result to the nearer of the two source values (cut-outs).
void resizeChannel(const Image& src, Image& dst, int channel, int threshold);
// Matches each colour channel's mean and spread to the original's (the models shift brightness and flatten contrast).
void matchColour(const Image& original, Image& up);
// Box-filter downscale by an integer factor.
[[nodiscard]] Image downscale(const Image& src, uint32_t factor);
// A tangent-space normal map from the luminance as height (Sobel), and a roughness map (INFERRED heuristics, see
// docs/enhanced-graphics.md); both RGBA8.
[[nodiscard]] Image normalMap(const Image& colour, float strength);
[[nodiscard]] Image roughnessMap(const Image& colour);

struct BuildOptions
{
    std::filesystem::path dir;               // the cache
    std::filesystem::path upscaler;          // realesrgan-ncnn-vulkan
    std::string           model   = "realesr-animevideov3-x4"; // closest to the originals of the three shipped models (docs)
    uint32_t              scale   = 4;       // 4 (the model's own), 2 or 1 (x4 then box down)
    uint32_t              maxSize = 1024;    // largest side kept
    bool                  maps    = true;    // also write _n / _r
    bool                  force   = false;   // rebuild entries that exist
};
struct BuildStats
{
    std::size_t built = 0, skipped = 0, tiny = 0, failed = 0;
    uint64_t    bytes = 0;
};
// Upscales `textures` (key, original RGBA) into the cache in batches through the external upscaler.
BuildStats build(const std::vector<std::pair<std::string, Image>>& textures, const BuildOptions& opt);

} // namespace xi::dat::hd
