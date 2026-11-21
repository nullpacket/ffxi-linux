// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/search.h"
#include "xi/blowfish.h"
#include "xi/md5.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
constexpr uint8_t kStatic[16] = { 0x30, 0x73, 0x3D, 0x6D, 0x3C, 0x31, 0x49, 0x5A, 0x32, 0x7A, 0x42, 0x43, 0x63, 0x38, 0x7B, 0x7E };

xi::Blowfish keyed(std::span<const std::byte> key)
{
    xi::Blowfish bf(xi::Blowfish::SessionKey{});
    bf.schedule(xi::md5(key));
    return bf;
}
} // namespace

TEST_CASE("search request: MSB-first criteria, MD5 trailer, Blowfish from byte 8")
{
    xi::client::SearchQuery q;
    q.name         = "bot";
    const uint32_t key = 0x12345678;
    auto           r   = xi::client::buildSearchRequest(q, key);
    auto&          p   = r.bytes;
    REQUIRE(p.size() % 8 == 4);
    uint16_t size = 0;
    std::memcpy(&size, p.data(), 2);
    CHECK(size == p.size());

    std::array<std::byte, 20> k{};
    std::memcpy(k.data(), kStatic, 16);
    std::memcpy(k.data() + 16, &key, 4);
    keyed(k).decryptBlocks(std::span(p).subspan(8, ((p.size() - 12) / 4 & ~std::size_t{ 1 }) * 4));
    CHECK(p[0x0B] == std::byte{ 0x00 }); // no area: TCP_SEARCH_ALL
    // Name entry: type 0, ascending, present, length 3, "bot" in 7-bit chars.
    CHECK(p[0x10] == std::byte{ 5 });
    const uint8_t want[5] = { 0x02, 0x3C, 0x5B, 0xFA, 0x00 };
    CHECK(std::memcmp(p.data() + 0x11, want, 5) == 0);
    const auto hash = xi::md5(std::span<const std::byte>(p).subspan(8, p.size() - 0x14 - 8));
    CHECK(std::memcmp(p.data() + p.size() - 0x14, hash.data(), 16) == 0);
    CHECK(std::memcmp(r.responseKey.data() + 20, p.data() + p.size() - 0x18, 4) == 0);
}

TEST_CASE("search list: one entry as LSB sends it (Bottwo, Southern San d'Oria, BLM1)")
{
    // A decrypted list packet captured from LSB's search server.
    const uint8_t plain[] = { 0x4f, 0x00, 0x00, 0x00, 0x49, 0x58, 0x46, 0x46, 0x3b, 0x00, 0x80, 0x80, 0x00, 0x00, 0x01, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0x03, 0x42, 0xdf, 0xd3, 0xa7, 0x7d, 0xe1,
                              0x39, 0x84, 0x0c, 0x80, 0x20, 0x08, 0x01, 0x56, 0x00, 0x26, 0x00, 0x00, 0x40, 0x00, 0x01, 0xb8,
                              0x00, 0x00, 0x00, 0x02, 0xc0, 0x00, 0x00, 0x00, 0x17, 0x00, 0x02, 0x67, 0x68, 0xb9, 0x02, 0x0b,
                              0x85, 0x6d, 0x95, 0x2d, 0xf4, 0xa6, 0xc8, 0xa2, 0xdd, 0xaf, 0x51, 0xa8, 0x86, 0x4b, 0x4d };
    std::vector<std::byte> p(sizeof(plain));
    std::memcpy(p.data(), plain, sizeof(plain));
    std::array<std::byte, 24> key{};
    for (std::size_t i = 0; i < key.size(); ++i)
    {
        key[i] = static_cast<std::byte>(i * 7 + 1);
    }
    keyed(key).encryptBlocks(std::span(p).subspan(8, ((p.size() - 12) / 4 & ~std::size_t{ 1 }) * 4));
    xi::client::SearchResult out;
    CHECK(xi::client::readSearchList(p, key, out));
    CHECK(out.total == 1);
    REQUIRE(out.hits.size() == 1);
    const auto& h = out.hits[0];
    CHECK(h.name == "Bottwo");
    CHECK(h.zone == 230);
    CHECK(h.mainJob == 4);
    CHECK(h.mainLevel == 1);
    CHECK(h.race == 5);
    CHECK(h.id == 3);
    CHECK_FALSE(h.anonymous());
}

TEST_CASE("search request: flags 2 (Unity / Request / Mute List) is a bare type 0x16 entry with 32 bits")
{
    // Unity leader 3 (Invincible Shield): flags 2 = 3 << 22 (CODE 0x100e3130); no sort / present bits (CODE 0x10104130,
    // LSB search_handler).
    xi::client::SearchQuery q;
    q.flags2           = 3u << 22;
    const uint32_t key = 0x0BADF00D;
    auto           r   = xi::client::buildSearchRequest(q, key);
    auto&          p   = r.bytes;
    std::array<std::byte, 20> k{};
    std::memcpy(k.data(), kStatic, 16);
    std::memcpy(k.data() + 16, &key, 4);
    keyed(k).decryptBlocks(std::span(p).subspan(8, ((p.size() - 12) / 4 & ~std::size_t{ 1 }) * 4));
    CHECK(p[0x10] == std::byte{ 5 });
    const uint8_t want[5] = { 0xB0, 0x06, 0x00, 0x00, 0x00 };
    CHECK(std::memcmp(p.data() + 0x11, want, 5) == 0);
}
