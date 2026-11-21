// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/name_icons.h"

#include <catch2/catch_test_macros.hpp>

using xi::client::NameFlags;
using xi::client::nameIcons;

TEST_CASE("name icons: the 0x00D bits", "[name_icons]")
{
    // LinkShellFlag (Flags1 bit 17) alone: the pearl.
    auto f = NameFlags::fromPcUpdate(1u << 17, 0, 0, 0, 0);
    CHECK(f.linkshell);
    CHECK(nameIcons(f) == "\x92");
    // Seeking a party (LfgFlag, bit 11) replaces the pearl.
    f = NameFlags::fromPcUpdate(1u << 17 | 1u << 11, 0, 0, 0, 0);
    CHECK(nameIcons(f) == "\x91");
    // Away outranks a GM level; GmIconFlag (Flags2 bit 28) hides the GM icon.
    CHECK(nameIcons(NameFlags::fromPcUpdate(1u << 14 | 3u << 24, 0, 0, 0, 0)) == "\x90");
    CHECK(nameIcons(NameFlags::fromPcUpdate(3u << 24, 0, 0, 0, 0)) == "\x95");
    CHECK(nameIcons(NameFlags::fromPcUpdate(3u << 24, 1u << 28, 0, 0, 0)).empty());
    // Recruiting (LfgMasterFlag, Flags3 bit 1): two glyphs.
    CHECK(nameIcons(NameFlags::fromPcUpdate(0, 0, 1u << 1, 0, 0)) == "\xAC\xAD");
    // New adventurer (Flags3 bit 23), mentor (bit 24) outranks it.
    CHECK(nameIcons(NameFlags::fromPcUpdate(0, 0, 1u << 23, 0, 0)) == "\xA1");
    CHECK(nameIcons(NameFlags::fromPcUpdate(0, 0, 1u << 23 | 1u << 24, 0, 0)) == "\xA2");
    // A normal player's allegiance (BallistaTeam 1) shows nothing; invisible players show no icons.
    CHECK(nameIcons(NameFlags::fromPcUpdate(0, 0, 1u << 8, 0, 0)).empty());
    CHECK(nameIcons(NameFlags::fromPcUpdate(1u << 17 | 1u << 29, 0, 0, 0, 0)).empty());
    // Job Master (Flags4 bit 6): the stars line, not an icon.
    f = NameFlags::fromPcUpdate(0, 0, 0, 0, 1u << 6);
    CHECK(f.jobMaster);
    CHECK(nameIcons(f).empty());
}

TEST_CASE("name icons: our own 0x037 bits", "[name_icons]")
{
    auto f = NameFlags::fromServerStatus(1u << 25, 0, 0, 0, 0);
    CHECK(nameIcons(f) == "\x92");
    f = NameFlags::fromServerStatus(1u << 25 | 1u << 4, 0, 0, 0, 0); // LfgFlag
    CHECK(nameIcons(f) == "\x91");
    f = NameFlags::fromServerStatus(0, 1u << 29, 0, 0, 0x80); // bazaar, Job Master
    CHECK(nameIcons(f) == "\x9C");
    CHECK(f.jobMaster);
}
