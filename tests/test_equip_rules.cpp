// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/equip_rules.h"

#include <catch2/catch_test_macros.hpp>

// The game's equipment rules (goal #45; docs/ui-format-notes.md §10.12).
using namespace xi::ui;
using xi::dat::ItemInfo;

namespace
{
ItemInfo weapon(uint8_t skill, uint16_t slots = 0x03)
{
    ItemInfo it;
    it.type  = 4;
    it.slots = slots;
    it.races = 0x1FE;
    it.jobs  = 0x007FFFFE;
    it.level = 1;
    it.skill = skill;
    return it;
}
} // namespace

TEST_CASE("wearing needs the race, the main job, the level and no blocked slot", "[equip]")
{
    ItemInfo mail;
    mail.type  = 5;
    mail.slots = 0x20; // body
    mail.races = 0x1FE;
    mail.jobs  = 1u << 1; // WAR
    mail.level = 10;
    EquipContext war{ 1, 10, 1, 0, 0 };
    CHECK(canEquip(mail, war));
    CHECK_FALSE(canEquip(mail, EquipContext{ 2, 10, 1, 0, 0 })); // MNK
    CHECK_FALSE(canEquip(mail, EquipContext{ 1, 9, 1, 0, 0 }));  // too low
    mail.races = 0x80;                                           // Mithra only
    CHECK_FALSE(canEquip(mail, war));
    CHECK(canEquip(mail, EquipContext{ 1, 10, 7, 0, 0 }));
    CHECK_FALSE(canEquip(mail, EquipContext{ 1, 10, 7, 0, 0x20 })); // body blocked
    mail.superiorLevel = 1;
    CHECK_FALSE(canEquip(mail, EquipContext{ 1, 10, 7, 0, 0 }));
    CHECK(canEquip(mail, EquipContext{ 1, 10, 7, 1, 0 }));
    CHECK(fitsSlot(mail, 5, 0));
    CHECK_FALSE(fitsSlot(mail, 4, 0));
}

TEST_CASE("a weapon in the sub slot: dual-wield jobs, a main weapon, a one-handed one", "[equip]")
{
    const auto sword = weapon(3);
    const auto axe   = weapon(6, 0x01); // great axe
    CHECK(subWeaponRefusal(sword, weapon(3), 1, 0) == 139);  // WAR / -
    CHECK(subWeaponRefusal(sword, weapon(3), 1, 13) == 0);   // WAR / NIN
    CHECK(subWeaponRefusal(sword, std::nullopt, 13, 0) == 152);
    CHECK(subWeaponRefusal(sword, axe, 6, 0) == 153);        // THF with a great axe
    CHECK(subWeaponRefusal(sword, weapon(1, 0x01), 19, 0) == 153); // hand-to-hand counts as two-handed
    // Grips and shields are not weapons for the main hand: never refused here.
    CHECK(subWeaponRefusal(weapon(0, 0x02), axe, 1, 0) == 0);
    ItemInfo shield;
    shield.type  = 5;
    shield.slots = 0x02;
    CHECK(subWeaponRefusal(shield, std::nullopt, 1, 0) == 0);
}

TEST_CASE("list colours and info labels", "[equip]")
{
    ItemInfo potion;
    potion.type  = 7;
    potion.flags = 0x0604;
    CHECK(itemRow(potion, false) == ItemRow::Usable);
    ItemInfo crystal;
    crystal.type = 8;
    CHECK(itemRow(crystal, false) == ItemRow::Usable);
    ItemInfo ore;
    ore.type = 1;
    CHECK(itemRow(ore, false) == ItemRow::Normal);
    CHECK(itemRow(weapon(3), true) == ItemRow::Worn);
    CHECK(itemRow(weapon(3), false) == ItemRow::Normal);

    CHECK(raceLabel(0x1FE) == 0);  // All Races
    CHECK(raceLabel(0x12A) == 1);  // all male
    CHECK(raceLabel(0x80) == 13);  // Mithra
    CHECK(raceLabel(0x100) == 12); // Galka
    CHECK(raceLabel(0x02) == 4);   // Hume male
    CHECK(slotLabel(0x10) == 14);  // [Head]
    CHECK(slotLabel(0x6000) == 18); // [Ring]
    CHECK(slotLabel(0x02) == -1);  // shields: none
    CHECK(skillLabel(1) == 24);    // (Hand-to-Hand)
    CHECK(skillLabel(12) == 35);   // (Staff)
    CHECK(skillLabel(48) == 42);   // (Fishing Tackle)

    ItemInfo rareEx;
    rareEx.flags = 0xC000;
    const auto m = infoMarks(rareEx, 3);
    CHECK(m.rare);
    CHECK(m.ex);
    CHECK(m.temporary);
    CHECK_FALSE(infoMarks(ore, 0).rare);
}
