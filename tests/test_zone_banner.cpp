// SPDX-License-Identifier: GPL-3.0-or-later
// The area lines logged on zone-in (issue #81; client/zone_banner.h). The texts come from the user's own install
// (skipped when FFXI_ROOT is unset or missing); the branch logic is checked without it.
#include "client/zone_banner.h"
#include "dat/dat_index.h"
#include "dat/messages.h"
#include "xi/install.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

namespace
{

const xi::dat::DatIndex* install()
{
    static std::unique_ptr<xi::dat::DatIndex> idx = [] {
        const char* root = xi::install::gameRootOrNull();
        if (!root || !std::filesystem::exists(std::filesystem::path(root) / "VTABLE.DAT"))
        {
            return std::unique_ptr<xi::dat::DatIndex>{};
        }
        return std::make_unique<xi::dat::DatIndex>(root);
    }();
    return idx.get();
}

} // namespace

using xi::client::ZoneInInfo;
using xi::client::zoneInLines;

TEST_CASE("zone-in area line without the text tables")
{
    // No zone name: the DLL's own "=== Area: %d ===".
    const auto lines = zoneInLines(ZoneInInfo{ 118, 2, 118, 0, 0x1FF }, nullptr, nullptr, nullptr);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "=== Area: 118 ===");
    // Login states 3, 4 and 5 log nothing; a Mog House without the client text logs nothing.
    CHECK(zoneInLines(ZoneInInfo{ 230, 3, 230, 0, 0 }, nullptr, nullptr, nullptr).empty());
    CHECK(zoneInLines(ZoneInInfo{ 230, 4, 230, 0, 0 }, nullptr, nullptr, nullptr).empty());
    CHECK(zoneInLines(ZoneInInfo{ 230, 5, 230, 0, 0 }, nullptr, nullptr, nullptr).empty());
    CHECK(zoneInLines(ZoneInInfo{ 230, 1, 230, 0, 0x0100 }, nullptr, nullptr, nullptr).empty());
    // Mog Garden (MapNumber 0x118) in the Mog House state still gets the area line.
    CHECK(zoneInLines(ZoneInInfo{ 280, 1, 0x118, 0, 0 }, nullptr, nullptr, nullptr).size() == 1);
}

TEST_CASE("zone-in area lines from the game's text tables")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto formats = xi::dat::XiStringTable::parse(install()->read(50));
    const auto areas   = xi::dat::StringTable::parse(install()->read(55465));
    const auto client  = xi::dat::StringTable::parse(install()->read(55648));

    auto lines = [&](const ZoneInInfo& z) { return zoneInLines(z, &formats, &areas, &client); };

    // A field zone (the issue's Buburimu Peninsula) and a town.
    CHECK(lines(ZoneInInfo{ 118, 2, 118, 0, 0x1FF }) == std::vector<std::string>{ "=== Area: Buburimu Peninsula ===" });
    CHECK(lines(ZoneInInfo{ 230, 2, 230, 0, 0x1FF }) == std::vector<std::string>{ "=== Area: Southern San d'Oria ===" });
    // Mog House: 1F / 2F lines when it has a second floor (floor 1, or models 0x121..0x123), nothing otherwise.
    CHECK(lines(ZoneInInfo{ 230, 1, 230, 1, 0x0100 }) == std::vector<std::string>{ "=== Area: Mog House 2F ===" });
    CHECK(lines(ZoneInInfo{ 230, 1, 230, 0, 0x0122 }) == std::vector<std::string>{ "=== Area: Mog House 1F ===" });
    CHECK(lines(ZoneInInfo{ 230, 1, 230, 0, 0x0100 }).empty());
    // Mog Garden, sent with the Mog House state.
    CHECK(lines(ZoneInInfo{ 280, 1, 0x118, 0, 0 }) == std::vector<std::string>{ "=== Area: Mog Garden ===" });
}
