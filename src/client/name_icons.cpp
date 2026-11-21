// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/name_icons.h"

namespace xi::client
{

namespace
{

constexpr bool bit(uint32_t v, unsigned n)
{
    return ((v >> n) & 1u) != 0;
}

} // namespace

NameFlags NameFlags::fromPcUpdate(uint32_t flags1, uint32_t flags2, uint32_t flags3, uint8_t ballistaInfo, uint8_t flags4)
{
    // The 0x00D handler copies these bit by bit into the actor (CODE 0x10099a10 .. 0x1009a920).
    NameFlags f;
    f.lfg          = bit(flags1, 11);
    f.away         = bit(flags1, 14);
    f.pol          = bit(flags1, 16);
    f.linkshell    = bit(flags1, 17);
    f.linkDead     = bit(flags1, 18);
    f.gmLevel      = static_cast<uint8_t>((flags1 >> 24) & 7);
    f.invisible    = bit(flags1, 29);
    f.bazaar       = bit(flags1, 31);
    f.gmIcon       = bit(flags2, 28);
    f.autoParty    = bit(flags2, 31);
    f.lfgMaster    = bit(flags3, 1);
    f.ballistaTeam = static_cast<uint8_t>((flags3 >> 8) & 0xFF);
    f.newChar      = bit(flags3, 23);
    f.mentor       = bit(flags3, 24);
    f.ballistaInfo = ballistaInfo;
    f.trial        = bit(flags4, 1);
    f.jobMaster    = bit(flags4, 6);
    return f;
}

NameFlags NameFlags::fromServerStatus(uint32_t flags0, uint32_t flags1, uint32_t flags2, uint32_t flags3, uint8_t flags4)
{
    // Field names as LSB's GP_SERV_SERVERSTATUS (char_status.cpp).
    NameFlags f;
    f.lfg          = bit(flags0, 4);
    f.away         = bit(flags0, 7);
    f.pol          = bit(flags0, 24);
    f.linkshell    = bit(flags0, 25);
    f.linkDead     = bit(flags0, 26);
    f.gmLevel      = static_cast<uint8_t>((flags0 >> 29) & 7);
    f.invisible    = bit(flags1, 15);
    f.bazaar       = bit(flags1, 29);
    f.gmIcon       = bit(flags1, 31);
    f.autoParty    = bit(flags2, 2);
    f.lfgMaster    = bit(flags3, 0);
    f.trial        = bit(flags3, 1);
    f.newChar      = bit(flags3, 3);
    f.mentor       = bit(flags3, 4);
    f.ballistaTeam = static_cast<uint8_t>((flags3 >> 8) & 0xFF);
    f.jobMaster    = bit(flags4, 7);
    return f;
}

std::string nameIcons(const NameFlags& f)
{
    // 0x10097720 / 0x10097320: no icons on an invisible player (INFERRED: the gate's other bits, +0x120 bit 9 and
    // +0x130 bit 7, were not traced).
    if (f.invisible)
    {
        return {};
    }
    const uint8_t prev = f.linkshell ? 0x92 : 0; // 0x10097770: the pearl first, replaced by any icon below
    uint8_t       icon = prev;
    const uint8_t t    = f.ballistaTeam;
    if (f.pol)
    {
        icon = 0x8E;
    }
    else if (f.linkDead)
    {
        icon = 0x8F;
    }
    else if (t >= 2 && t <= 0x63 && !(t >= 0x28 && t <= 0x2B))
    {
        // Ballista / Brenner teams (CODE 0x10097487, tables 0x10097624 / 0x10097640). The game's +0x29c (set by another
        // packet) picks 0xA5 for two of them: taken as unset here.
        int i = f.ballistaInfo != 0 ? f.ballistaInfo : (t <= 6 ? t - 1 : 0);
        --i;
        static constexpr uint8_t kTeam[] = { 0x9E, 0x9F, 0xA0, 0xA3, 0xA4, 0xA3, 0xA4 };
        icon = i >= 0 && i <= 6 ? kTeam[i] : prev;
        if (t >= 0x20 && t <= 0x27)
        {
            icon = (t & 1) ? 0xA7 : 0xA6;
        }
    }
    else if (f.away)
    {
        icon = 0x90;
    }
    else if (!f.gmIcon && f.gmLevel != 0)
    {
        static constexpr uint8_t kGm[] = { 0x93, 0x93, 0x93, 0x95, 0x96, 0x97, 0x98, 0x99 }; // 0x1035b62c
        icon = kGm[f.gmLevel & 7];
    }
    else if (f.lfgMaster)
    {
        icon = 0xAC;
    }
    else if (f.autoParty)
    {
        icon = 0x9D;
    }
    else if (f.lfg)
    {
        icon = 0x91;
    }
    else if (f.trial)
    {
        icon = 0x93;
    }
    else if (f.mentor)
    {
        icon = 0xA2;
    }
    else if (f.newChar)
    {
        icon = 0xA1;
    }
    else if (f.bazaar)
    {
        icon = 0x9C;
    }
    std::string s;
    if (icon != 0)
    {
        s.push_back(static_cast<char>(icon));
        if (icon == 0xAC)
        {
            s.push_back(static_cast<char>(0xAD));
        }
    }
    return s;
}

} // namespace xi::client
