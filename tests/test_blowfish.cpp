// SPDX-License-Identifier: GPL-3.0-or-later
#include "lobby/lobby.h"
#include "xi/blowfish.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

// Vectors produced by compiling the LSB server's own blowfish.cpp + md52.cpp (docs/protocol-map.md §2.7).
namespace
{

std::array<uint8_t, 16> encryptCounting(const xi::Blowfish& bf)
{
    std::array<std::byte, 16> d{};
    for (int i = 0; i < 16; ++i)
    {
        d[i] = std::byte(i);
    }
    bf.encryptBlocks(d);
    std::array<uint8_t, 16> out{};
    for (int i = 0; i < 16; ++i)
    {
        out[i] = std::to_integer<uint8_t>(d[i]);
    }
    return out;
}

xi::Blowfish::SessionKey keyB()
{
    xi::Blowfish::SessionKey k{};
    for (int i = 0; i < 20; ++i)
    {
        k[i] = std::byte(i + 1);
    }
    return k;
}

} // namespace

TEST_CASE("blowfish matches server: zero key")
{
    xi::Blowfish bf(xi::Blowfish::SessionKey{});
    CHECK(encryptCounting(bf) == std::array<uint8_t, 16>{ 0xf4, 0x0a, 0x34, 0xaf, 0x3e, 0x4a, 0xbf, 0x20, 0xfd, 0xda, 0x34, 0xb5, 0xbf, 0x63, 0xed, 0xf3 });
}

TEST_CASE("blowfish matches server: key 01..14")
{
    xi::Blowfish bf(keyB());
    CHECK(encryptCounting(bf) == std::array<uint8_t, 16>{ 0x82, 0xcb, 0xa3, 0x2c, 0x5d, 0xe5, 0xd9, 0xac, 0xa3, 0x07, 0x86, 0xf9, 0x5d, 0x9d, 0x82, 0xdd });
}

TEST_CASE("blowfish matches server: key 01..14 after one zone change")
{
    xi::Blowfish bf(keyB());
    bf.incrementForZone();
    CHECK(std::to_integer<int>(bf.sessionKey()[16]) == 0x13);
    CHECK(encryptCounting(bf) == std::array<uint8_t, 16>{ 0x86, 0x24, 0x28, 0x48, 0x94, 0x45, 0x60, 0x1c, 0xa6, 0x8e, 0x1a, 0xdd, 0x88, 0x38, 0x13, 0xf8 });
}

TEST_CASE("blowfish zone increment carries across bytes 16..19")
{
    xi::Blowfish::SessionKey k{};
    k[16] = std::byte{ 0xFF };
    xi::Blowfish bf(k);
    bf.incrementForZone();
    CHECK(std::to_integer<int>(bf.sessionKey()[16]) == 0x01);
    CHECK(std::to_integer<int>(bf.sessionKey()[17]) == 0x01);
}

TEST_CASE("blowfish decrypt inverts encrypt and leaves a partial block alone")
{
    xi::Blowfish              bf(keyB());
    std::array<std::byte, 21> d{};
    for (int i = 0; i < 21; ++i)
    {
        d[i] = std::byte(i * 7);
    }
    const auto orig = d;
    bf.encryptBlocks(d);
    CHECK(d[20] == orig[20]);
    bf.decryptBlocks(d);
    CHECK(d == orig);
}

TEST_CASE("map key from the session hash and the lobby key counter (LSB data_session, xiloader 2.2)")
{
    xi::lobby::SessionHash hash{};
    for (int i = 0; i < 16; ++i)
    {
        hash[i] = std::byte(i + 1);
    }
    const auto key = xi::lobby::mapKey(hash, 0xAD5DE04F); // LSB's 0x05 ResponseKey key
    // MD5 of bytes 01..10, then the counter: 0x05 key + 9 = 58 E0 5D AD, the old xiloader constant.
    const std::array<uint8_t, 20> expected = { 0x19, 0x0C, 0x4C, 0x10, 0x57, 0x86, 0xA2, 0x12, 0x1D, 0x85,
                                               0x01, 0x89, 0x39, 0x10, 0x8A, 0x6C, 0x58, 0xE0, 0x5D, 0xAD };
    for (int i = 0; i < 20; ++i)
    {
        CHECK(std::to_integer<uint8_t>(key[i]) == expected[i]);
    }
}

// The 2.1 protocol must stay byte for byte what ffxi-linux sent before LSB's 2026-10 change (dc9c9c5: lobby.cpp
// authRequest and kDefaultKey); no 2.1 server is left to test against live.
TEST_CASE("login protocol 2.1 keeps the pre-2.2 auth body and map key")
{
    xi::lobby::Config cfg;
    cfg.username = "bot2";
    cfg.password = "botpass";
    CHECK(xi::lobby::authRequestJson(cfg, 0x10, xi::lobby::LoginProtocol::V21) ==
          R"({"command":16,"password":"botpass","username":"bot2","version":[2,1,0]})");
    cfg.otp = "123456";
    CHECK(xi::lobby::authRequestJson(cfg, 0x10, xi::lobby::LoginProtocol::V21) ==
          R"({"command":16,"otp":"123456","password":"botpass","username":"bot2","version":[2,1,0]})");

    const auto key = xi::lobby::legacyMapKey();
    for (int i = 0; i < 16; ++i)
    {
        CHECK(key[i] == std::byte{ 0 });
    }
    CHECK(std::to_integer<uint8_t>(key[16]) == 0x58);
    CHECK(std::to_integer<uint8_t>(key[17]) == 0xE0);
    CHECK(std::to_integer<uint8_t>(key[18]) == 0x5D);
    CHECK(std::to_integer<uint8_t>(key[19]) == 0xAD);
}

TEST_CASE("an auth body with a non-UTF-8 password does not throw (#106)")
{
    xi::lobby::Config cfg;
    cfg.username = "bot2";
    cfg.password = "pass\x81\xA8";
    std::string body;
    REQUIRE_NOTHROW(body = xi::lobby::authRequestJson(cfg, 0x10, xi::lobby::LoginProtocol::V22));
    CHECK(body == "{\"command\":16,\"password\":\"pass\xEF\xBF\xBD\xEF\xBF\xBD\",\"username\":\"bot2\",\"version\":[2,2,0]}");
}

TEST_CASE("login protocol 2.2 auth body, option parsing and the version-error fallback")
{
    xi::lobby::Config cfg;
    cfg.username = "bot2";
    cfg.password = "botpass";
    CHECK(xi::lobby::authRequestJson(cfg, 0x10, xi::lobby::LoginProtocol::V22) ==
          R"({"command":16,"password":"botpass","username":"bot2","version":[2,2,0]})");
    CHECK(xi::lobby::authRequestJson(cfg, 0x10, xi::lobby::LoginProtocol::Auto) ==
          R"({"command":16,"password":"botpass","username":"bot2","version":[2,2,0]})");

    using xi::lobby::LoginProtocol;
    CHECK(xi::lobby::parseLoginProtocol("auto") == LoginProtocol::Auto);
    CHECK(xi::lobby::parseLoginProtocol("2.1") == LoginProtocol::V21);
    CHECK(xi::lobby::parseLoginProtocol("2.2") == LoginProtocol::V22);
    CHECK_FALSE(xi::lobby::parseLoginProtocol("2.3"));

    // LSB since 2026-10 (auth_session.cpp) refusing a 2.1 client, and LSB before it refusing a 2.2 client.
    CHECK(xi::lobby::requiredProtocol("auth: Unsupported xiloader version 2.1.0.\nThis server requires version 2.2.x.") ==
          LoginProtocol::V22);
    CHECK(xi::lobby::requiredProtocol("auth: Your xiloader is too old.\nPlease update to version '2.1.x'.\nYour client "
                                      "reported '2.2.0'.") == LoginProtocol::V21);
    CHECK_FALSE(xi::lobby::requiredProtocol("auth: login failed (result 2)"));
}
