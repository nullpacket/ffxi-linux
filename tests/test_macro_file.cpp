// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/install.h"
#include "dat/macro_file.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace m = xi::dat::macro;

TEST_CASE("macro files: set file names follow book * 10 + set")
{
    CHECK(m::setFileName(0, 0) == "mcr.dat");
    CHECK(m::setFileName(0, 1) == "mcr1.dat");
    CHECK(m::setFileName(1, 0) == "mcr10.dat");
    CHECK(m::setFileName(39, 9) == "mcr399.dat");
}

TEST_CASE("macro files: a set round-trips with its checksum")
{
    m::Set s;
    s.macros[0].name     = "Cure";
    s.macros[0].lines[0] = "/ma \"Cure\" <t>";
    s.macros[0].lines[1] = "/wait 3";
    s.macros[19].name    = "Alt0";
    s.macros[19].lines[5] = std::string(60, 'x');
    const auto bytes      = m::writeSet(s);
    REQUIRE(bytes.size() == 24 + 20 * 0x17c);
    const auto back = m::parseSet(bytes);
    REQUIRE(back);
    CHECK(back->macros[0].name == "Cure");
    CHECK(back->macros[0].lines[1] == "/wait 3");
    CHECK(back->macros[19].lines[5].size() == 60);
    CHECK(back->macros[5].empty());
    auto broken = bytes;
    broken[100] = std::byte{ 'Z' };
    CHECK_FALSE(m::parseSet(broken)); // the MD5 no longer matches
}

TEST_CASE("macro files: titles and the palette position round-trip")
{
    std::array<std::string, 20> names;
    names[0]  = "Book01";
    names[19] = "Healing";
    const auto t = m::parseTitles(m::writeTitles(names));
    REQUIRE(t);
    CHECK((*t)[19] == "Healing");
    CHECK(m::parseSys(m::writeSys(3 * 20 + 4 * 2)) == 3u * 20 + 4 * 2);
}

TEST_CASE("macro files: a bot's files from the install read and write back the same")
{
    const char* root = xi::install::gameRootOrNull();
    if (!root)
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto dir = std::filesystem::path(root) / "USER" / "2"; // Botone
    auto       read = [](const std::filesystem::path& p) {
        std::ifstream          in(p, std::ios::binary);
        std::vector<char>      c((std::istreambuf_iterator<char>(in)), {});
        std::vector<std::byte> b(c.size());
        std::memcpy(b.data(), c.data(), c.size());
        return b;
    };
    if (!std::filesystem::exists(dir / "mcr.dat"))
    {
        SKIP("no USER/2/mcr.dat");
    }
    const auto file = read(dir / "mcr.dat");
    const auto set  = m::parseSet(file);
    REQUIRE(set);
    // Everything after the header's second word is ours to reproduce.
    const auto again = m::writeSet(*set);
    REQUIRE(again.size() == file.size());
    CHECK(std::equal(again.begin() + 8, again.end(), file.begin() + 8));
    const auto titles = m::parseTitles(read(dir / "mcr.ttl"));
    REQUIRE(titles);
    CHECK((*titles)[0] == "Book01");
    CHECK(m::parseSys(read(dir / "mcr.sys")));
}

TEST_CASE("macro runner: what each line does")
{
    using K = m::LineStep::Kind;
    CHECK(m::stepFor("").kind == K::End);
    CHECK(m::stepFor("// a note").kind == K::Skip);
    const auto run = m::stepFor("/ma \"Cure\" <me>");
    CHECK((run.kind == K::Run && run.text == "/ma \"Cure\" <me>" && run.wait == 0));
    const auto w = m::stepFor("/wait 3");
    CHECK((w.kind == K::Run && w.text.empty() && w.wait == 3));
    CHECK(m::stepFor("/WAIT").wait == 1);   // no number: 1
    CHECK(m::stepFor("/wait 61").wait == 0); // out of 1..60: no wait (and nothing run)
    CHECK(m::stepFor("/wait 61").text.empty());
    const auto inl = m::stepFor("/p Ready! <wait 5>");
    CHECK((inl.kind == K::Run && inl.text == "/p Ready!" && inl.wait == 5));
    CHECK(m::stepFor("/echo hi <wait>").wait == 1);
    CHECK(m::stepFor("/echo hi <wait 99>").wait == 60);
}
