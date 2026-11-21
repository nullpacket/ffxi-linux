// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/chunk.h"
#include "xi/bytes.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace
{

void appendChunk(std::vector<std::byte>& file, const char* name, uint8_t type, std::size_t payload)
{
    const auto off  = file.size();
    const auto size = 16 + payload;
    file.resize(off + size);
    xi::storeString(file, off, 4 + 1, name);
    xi::store<uint32_t>(file, off + 4, static_cast<uint32_t>((size / 16) << 7) | type);
}

} // namespace

TEST_CASE("chunk walker parses a nested block and rejects truncation")
{
    std::vector<std::byte> f;
    appendChunk(f, "tsa", 1, 0);
    appendChunk(f, "img", 0x20, 32);
    appendChunk(f, "end", 0, 0);

    std::vector<xi::dat::Chunk> chunks;
    REQUIRE(xi::dat::parseChunks(f, chunks));
    REQUIRE(chunks.size() == 3);
    CHECK(chunks[1].name == "img");
    CHECK(chunks[1].type == 0x20);
    CHECK(chunks[1].payload.size() == 32);
    CHECK(chunks[2].offset == 64);

    f.pop_back();
    CHECK_FALSE(xi::dat::parseChunks(f, chunks));
}
