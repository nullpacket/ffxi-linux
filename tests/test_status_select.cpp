// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/dat_index.h"
#include "dat/items.h"
#include "ui/status_select.h"
#include "xi/install.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// Select Window from the field and the status icon cursor (#122; ui/status_select.h, dat::statusInfo).
namespace
{
const xi::dat::DatIndex* install()
{
    static std::unique_ptr<xi::dat::DatIndex> idx = [] {
        const char* root = xi::install::gameRootOrNull();
        return root ? std::make_unique<xi::dat::DatIndex>(root) : nullptr;
    }();
    return idx.get();
}
} // namespace

TEST_CASE("status select: Select Window steps log window, status icons, neither")
{
    using xi::ui::FieldSelect;
    CHECK(xi::ui::nextFieldSelect(FieldSelect::None) == FieldSelect::Log);
    CHECK(xi::ui::nextFieldSelect(FieldSelect::Log) == FieldSelect::Buffs);
    CHECK(xi::ui::nextFieldSelect(FieldSelect::Buffs) == FieldSelect::None);
}

TEST_CASE("status select: the cursor walks the icons, nine to a row, clamped")
{
    using xi::ui::BuffKey;
    using xi::ui::moveBuffCursor;
    CHECK(moveBuffCursor(0, 0, BuffKey::Right) == 0);
    CHECK(moveBuffCursor(0, 3, BuffKey::Left) == 0);
    CHECK(moveBuffCursor(0, 3, BuffKey::Right) == 1);
    CHECK(moveBuffCursor(2, 3, BuffKey::Right) == 2);
    CHECK(moveBuffCursor(5, 3, BuffKey::Left) == 1); // a stale cursor is clamped first
    CHECK(moveBuffCursor(2, 12, BuffKey::Down) == 11);
    CHECK(moveBuffCursor(3, 12, BuffKey::Down) == 3); // no icon below
    CHECK(moveBuffCursor(10, 12, BuffKey::Up) == 1);
    CHECK(moveBuffCursor(1, 12, BuffKey::Up) == 1);
}

TEST_CASE("status select: decodeDataBlock undoes the rotation chosen by bytes 2, 11 and 12")
{
    // Byte 2 = 0x01, 11 and 12 = 0: n = 1, |1 % 5| = 1 -> rotate right 1. Encoded bytes are rotated left 1.
    auto rotl = [](uint8_t b, int k) {
        return static_cast<std::byte>(static_cast<uint8_t>((b << k) | (b >> (8 - k))));
    };
    std::vector<std::byte> block(0x40);
    const std::string      text = "Defense is enhanced.";
    std::vector<uint8_t>   plain(0x40, 0);
    plain[0] = 40;   // id
    plain[2] = 0x01; // can cancel (also the key byte)
    plain[4] = 1;    // one string
    plain[8] = 0x0C; // from the table at +4, header 0x1C: text at +0x2C
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        plain[0x2C + i] = static_cast<uint8_t>(text[i]);
    }
    for (std::size_t i = 0; i < plain.size(); ++i)
    {
        block[i] = i == 2 || i == 11 || i == 12 ? static_cast<std::byte>(plain[i]) : rotl(plain[i], 1);
    }
    const auto s = xi::dat::decodeStatusInfo(block);
    REQUIRE(s);
    CHECK(s->id == 40);
    CHECK(s->canCancel);
    CHECK_FALSE(s->hideTimer);
    CHECK(s->description == text);

    // n = 0 (all key bytes 0) -> rotate left 1, so encoded bytes are rotated right 1.
    std::vector<std::byte> b0(0x30);
    b0[0]         = rotl(0x02, 7); // id 2
    const auto s0 = xi::dat::decodeStatusInfo(b0);
    REQUIRE(s0);
    CHECK(s0->id == 2);
    CHECK_FALSE(s0->canCancel);
}

TEST_CASE("status select: file 87 records from the game data")
{
    const auto* idx = install();
    if (!idx)
    {
        SKIP("no game data");
    }
    const auto protect = xi::dat::statusInfo(*idx, 40);
    REQUIRE(protect);
    CHECK(protect->canCancel);
    CHECK(protect->description == "Defense is enhanced.");
    const auto hundredFists = xi::dat::statusInfo(*idx, 46); // a job ability's effect (#123)
    REQUIRE(hundredFists);
    CHECK(hundredFists->canCancel);
    const auto food = xi::dat::statusInfo(*idx, 251);
    REQUIRE(food);
    CHECK_FALSE(food->canCancel);
    CHECK(food->description == "The meal you consumed is affecting your performance.");
    const auto ko = xi::dat::statusInfo(*idx, 0);
    REQUIRE(ko);
    CHECK_FALSE(ko->canCancel);
    CHECK(ko->hideTimer);
}
