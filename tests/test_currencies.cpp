// SPDX-License-Identifier: GPL-3.0-or-later
// The Currencies window (issue #89; ui/currencies.h, ui-format-notes §10.7): S2C 0x113 field selection and the row
// labels / help lines from the user's own install (skipped when FFXI_ROOT is unset or missing).
#include "ui/currencies.h"
#include "dat/dat_index.h"
#include "dat/messages.h"
#include "xi/install.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>

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

template <typename T> void put(std::vector<std::byte>& p, std::size_t off, T v)
{
    std::memcpy(p.data() + off, &v, sizeof(T));
}

} // namespace

using xi::ui::currencyRows;
using xi::ui::currencyValue;

TEST_CASE("currencies: every field key 0..98 once, in retail's order")
{
    const auto rows = currencyRows();
    REQUIRE(rows.size() == 99);
    std::set<int> keys;
    for (const auto& r : rows)
    {
        keys.insert(r.key);
    }
    CHECK(keys.size() == 99);
    CHECK(*keys.begin() == 0);
    CHECK(*keys.rbegin() == 98);
    // Table 0x103862c8: the conquest points first, Sparks of Eminence (key 90) right after Scylds (key 10).
    CHECK(rows[0].key == 0);
    CHECK(rows[10].key == 10);
    CHECK(rows[11].key == 90);
    CHECK(rows[98].key == 89);
}

TEST_CASE("currencies: field selection from S2C 0x113 (LSB GP_SERV_COMMAND_CURRENCIES_1 layout)")
{
    std::vector<std::byte> p(0xFC);
    put<int32_t>(p, 0x04, 1234);     // conquest points San d'Oria
    put<int32_t>(p, 0x0C, 56);       // conquest points Windurst
    put<uint16_t>(p, 0x10, 77);      // beastmen's seals
    put<uint16_t>(p, 0x1E, 9);       // scylds
    put<int32_t>(p, 0x20, 300);      // guild points, fishing
    put<int32_t>(p, 0x40, 900);      // guild points, cooking
    put<uint8_t>(p, 0x48, 3);        // fewell: fire
    put<uint16_t>(p, 0x58, 11);      // chocobucks, San d'Oria team
    put<uint16_t>(p, 0x5E, 4);       // daily tally
    put<uint16_t>(p, 0x6C, 21);      // infamy
    put<int32_t>(p, 0x74, 4321);     // sparks of eminence
    put<int32_t>(p, 0x80, 15);       // assault points, Leujaoam
    put<int32_t>(p, 0xA0, 1000);     // therion ichor
    put<uint8_t>(p, 0xB8, 2);        // 5th echelon battle trophies
    put<uint32_t>(p, 0xD8, 5u | (6u << 9) | (7u << 18) | (0x1Fu << 27)); // plans: bloodshed, umbrage, ritualistic, tutelary (low 5)
    put<uint8_t>(p, 0xDC, 0x23);     // tutelary high 4 bits = 3; primacy low 4 bits = 2
    put<uint16_t>(p, 0xE8, 500);     // fire crystals stored
    put<uint16_t>(p, 0xF8, 8);       // deeds

    CHECK(currencyValue(0, p) == 1234);
    CHECK(currencyValue(2, p) == 56);
    CHECK(currencyValue(3, p) == 77);
    CHECK(currencyValue(10, p) == 9);
    CHECK(currencyValue(0x0B, p) == 300);
    CHECK(currencyValue(0x13, p) == 900);
    CHECK(currencyValue(0x15, p) == 3);
    CHECK(currencyValue(0x1F, p) == 11);
    CHECK(currencyValue(0x46, p) == 4);
    CHECK(currencyValue(0x27, p) == 21);
    CHECK(currencyValue(0x2A, p) == 4321);
    CHECK(currencyValue(0x2D, p) == 15);
    CHECK(currencyValue(0x35, p) == 1000);
    CHECK(currencyValue(0x3A, p) == 2);
    CHECK(currencyValue(0x48, p) == 5);
    CHECK(currencyValue(0x49, p) == 6);
    CHECK(currencyValue(0x4A, p) == 7);
    CHECK(currencyValue(0x4B, p) == ((3u << 5) | 0x1Fu));
    CHECK(currencyValue(0x4C, p) == 2);
    CHECK(currencyValue(0x5A, p) == 500);
    CHECK(currencyValue(0x62, p) == 8);
    // A short packet reads 0, as does an unknown key.
    CHECK(currencyValue(0x62, std::span(p).first(0x20)) == 0);
    CHECK(currencyValue(200, p) == 0);
}

TEST_CASE("currencies: labels (55653) and help lines (55651) from the game's text tables")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto labels = xi::dat::StringTable::parse(install()->read(55653));
    const auto help   = xi::dat::StringTable::parse(install()->read(55651));
    const auto rows   = currencyRows();
    CHECK(labels.get(rows[0].label) == "Conquest Points (San d'Oria)");
    CHECK(labels.get(rows[10].label) == "Scylds");
    CHECK(help.get(721) == "Currencies");
    CHECK(std::string(help.get(rows[0].help)).starts_with("Currency earned from participating in Conquest"));
    for (const auto& r : rows)
    {
        CHECK_FALSE(labels.get(r.label).empty());
    }
}
