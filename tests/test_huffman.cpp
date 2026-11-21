// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/huffman.h"
#include "xi/install.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_CASE("huffman matches server encoder and round-trips")
{
    const auto path = xi::install::findCompressDat();
    if (!path)
    {
        SKIP("LSB's res/compress.dat not found (set XI_COMPRESS_DAT or LSB_DIR)");
    }
    const auto h = xi::Huffman::fromCompressDat(path->string());

    // docs/protocol-map.md §3.2: 00 01 ff 20 -> 01 95 68 f8 1e, bitCount 37.
    const std::array<std::byte, 4> in{ std::byte{ 0x00 }, std::byte{ 0x01 }, std::byte{ 0xff }, std::byte{ 0x20 } };
    uint32_t                       bits = 0;
    const auto                     enc  = h.encode(in, bits);
    CHECK(bits == 37);
    CHECK(enc == std::vector<std::byte>{ std::byte{ 0x01 }, std::byte{ 0x95 }, std::byte{ 0x68 }, std::byte{ 0xf8 }, std::byte{ 0x1e } });

    std::vector<std::byte> all(256);
    for (int i = 0; i < 256; ++i)
    {
        all[i] = std::byte(i);
    }
    const auto packed = h.encode(all, bits);
    CHECK(h.decode(packed, bits) == all);
}
