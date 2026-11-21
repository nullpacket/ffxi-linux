// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace xi
{

// The fixed 256-symbol prefix code FFXI uses for server->client packet bodies (LSB calls it "zlib";
// it is not). See docs/protocol-map.md §3. Built from compress.dat (512 LE uint32: code[256], len[256],
// indexed by the signed byte value + 0x80).
class Huffman
{
public:
    static Huffman fromCompressDat(const std::filesystem::path& path);
    static Huffman fromTable(std::span<const uint32_t, 512> table);

    // Decodes a body starting at the 0x01 flag byte. bitCount is the trailing u32 (includes the flag's 8 bits).
    [[nodiscard]] std::vector<std::byte> decode(std::span<const std::byte> body, uint32_t bitCount) const;

    // Produces [0x01][bitstream], and sets bitCount to the value to append.
    [[nodiscard]] std::vector<std::byte> encode(std::span<const std::byte> data, uint32_t& bitCount) const;

private:
    static constexpr unsigned kMaxBits = 12;

    struct Entry
    {
        uint8_t symbol = 0;
        uint8_t length = 0; // 0 = invalid
    };

    std::array<uint32_t, 256>          code_{};
    std::array<uint8_t, 256>           len_{};
    std::array<Entry, 1u << kMaxBits> lut_{}; // indexed by the next 12 stream bits (LSB-first)
};

} // namespace xi
