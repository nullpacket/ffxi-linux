// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The game's macro files, in USER/<content id hex>/ (ui-format-notes §7.9). Every file is a 24-byte header
// {u32 1, u32 ?, u8 md5[16] of the rest} and then its payload (CODE 0x101b9430 writes, 0x101b9ad0 reads):
// - mcr.dat, mcr1.dat .. mcr399.dat: one set each (book * 10 + set, 0-based; CODE 0x101a21c0 "%s%s%d%s"), 20 macros of
//   0x17c bytes: {u32 0, char line[6][61], char name[10]}; 0..9 the Ctrl row (keys 1..0), 10..19 the Alt row.
// - mcr.ttl / mcr_2.ttl: the names of books 1..20 / 21..40, 16 bytes each.
// - mcr.sys: u32, the palette's book * 20 + set * 2 (CODE 0x101a2440).
// Texts are the game's encoding: Shift-JIS, auto-translate phrases as FD <4 bytes> FD.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xi::dat::macro
{

constexpr int kBooks = 40, kSets = 10, kKeys = 10, kLines = 6;
constexpr std::size_t kLineBytes = 61, kNameBytes = 10, kMacroBytes = 0x17c, kHeaderBytes = 24;
constexpr std::size_t kLineMax = 60, kNameMax = 8; // CODE 0x101a1450 (< 0x3d) / 0x101a14f0 (<= 8)

struct Macro
{
    uint32_t                         flags = 0; // the record's first u32 (0 in every file seen)
    std::string                      name;
    std::array<std::string, kLines> lines;
    [[nodiscard]] bool               empty() const;
};

// One set: macros[0..9] = Ctrl 1..0, [10..19] = Alt 1..0.
struct Set
{
    std::array<Macro, 2 * kKeys> macros;
};

// "mcr.dat" for index 0, else "mcr<index>.dat" (index = book * 10 + set, both 0-based).
[[nodiscard]] std::string setFileName(int book, int set);

// A set file; nullopt when its size or MD5 is wrong (the game resets such a set, d_msg 55646 #200).
[[nodiscard]] std::optional<Set> parseSet(std::span<const std::byte> file);
[[nodiscard]] std::vector<std::byte> writeSet(const Set& set);

// mcr.ttl / mcr_2.ttl: 20 names.
[[nodiscard]] std::optional<std::array<std::string, 20>> parseTitles(std::span<const std::byte> file);
[[nodiscard]] std::vector<std::byte> writeTitles(const std::array<std::string, 20>& names);

// mcr.sys: book * 20 + set * 2.
[[nodiscard]] std::optional<uint32_t> parseSys(std::span<const std::byte> file);
[[nodiscard]] std::vector<std::byte> writeSys(uint32_t value);

// What the macro runner does with one line (CODE 0x101a1870): an empty line ends the macro; "//" lines are skipped;
// "/wait N" (N 1..60, none = 1; "%s %d" against "/wait") waits N seconds and is not run (other N: nothing); a line with
// "<wait N>" (0..60, none = 1) runs up to it and then waits; anything else runs as typed.
struct LineStep
{
    enum class Kind
    {
        End,
        Skip,
        Run,
    } kind = Kind::End;
    std::string text;    // Run: the line (or the part before "<wait N>"); empty = nothing to run
    int         wait = 0; // seconds to wait after it
};
[[nodiscard]] LineStep stepFor(std::string_view line);

} // namespace xi::dat::macro
