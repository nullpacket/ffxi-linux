// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The treasure pool's log lines (#115). LSB sends only S2C 0x0D2 (TROPHY_LIST: an item enters the pool) and 0x0D3
// (TROPHY_SOLUTION: a lot, or the outcome) and no message packet with them, so the lines come from the client: the
// standard messages (file 7031), filled from the packet's fields.
//   0x0D2: #16 "You find <a item> on <the> <target>." (#218 "... in the <target>." when IsContainer: a chest / coffer).
//          Nothing when Entry is set (the pool re-sent on zone-in: an item already announced).
//   0x0D3: JudgeFlg 0 a lot: #17 "<name>'s lot for the <Item>: <n> points." for the one who lotted (EntryPoint;
//          a pass, 0xFFFF, prints nothing); 1 won: #18 "<name> obtains <a item>."; 2 the winner could not take it:
//          #15 "<name> does not meet ... / <item> lost." (#31 "You do not meet ..." when it is us), or with no winner
//          (LSB's treasureLost sends 2 without one) #164 "<A item> was lost."; 3 lost: #164.
// INFERRED (the handlers in FFXiMain.dll were not traced): which message each case picks, nothing for a pass, and the
// capital at the start of #164 (its 7F 80 code).
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dat/messages.h"

namespace xi::client
{

struct LootNote
{
    uint16_t                   message = 0; // file 7031
    uint16_t                   itemId  = 0; // Para0
    std::string                target;      // 01 01 11 (#16) and text argument 1 (#218)
    bool                       targetNoArticle = false;
    std::array<std::string, 4> strings{}; // 0x1C n: #17 name 3, points 2; #15 / #18 name 2
};

// S2C 0x0D2. `targetName` / `targetIsPc`: the entity at TargetUniqueNo.
inline std::optional<LootNote> lootFoundNote(uint16_t itemId, uint8_t entry, bool isContainer,
                                             const std::string& targetName, bool targetIsPc)
{
    if (itemId == 0 || entry != 0)
    {
        return std::nullopt;
    }
    LootNote n;
    n.message         = isContainer ? 218 : 16;
    n.itemId          = itemId;
    n.target          = targetName;
    n.targetNoArticle = targetIsPc;
    n.strings[1]      = targetName;
    return n;
}

// S2C 0x0D3. `itemId`: the pool slot's item before the packet; `winner` (sLootName, empty when LootUniqueNo is 0),
// `lotter` / `lot` (sLootName2 / EntryPoint) for a lot; `winnerIsUs` when LootUniqueNo is our character.
inline std::optional<LootNote> lootSolutionNote(uint8_t judge, uint16_t itemId, const std::string& winner,
                                                bool winnerIsUs, const std::string& lotter, uint16_t lot)
{
    if (itemId == 0)
    {
        return std::nullopt;
    }
    LootNote n;
    n.itemId = itemId;
    switch (judge)
    {
        case 0:
            if (lotter.empty() || lot == 0xFFFF)
            {
                return std::nullopt;
            }
            n.message    = 17;
            n.strings[3] = lotter;
            n.strings[2] = std::to_string(lot);
            return n;
        case 1:
            n.message    = 18;
            n.strings[2] = winner;
            return n;
        case 2:
            if (!winner.empty())
            {
                n.message    = winnerIsUs ? 31 : 15;
                n.strings[2] = winner;
                return n;
            }
            n.message = 164;
            return n;
        case 3: n.message = 164; return n;
        default: return std::nullopt;
    }
}

// The note's lines from the standard messages; `lookup` names items (dat::BattleText::dialogName).
inline std::vector<std::string> lootLines(const dat::DialogTable& table, const LootNote& n,
                                          const std::function<std::string(uint16_t, uint32_t)>& lookup)
{
    std::vector<std::string> out;
    if (n.message >= table.size())
    {
        return out;
    }
    dat::DialogArgs args;
    args.lookup          = lookup;
    args.numbers[0]      = n.itemId;
    args.numbers[1]      = 1;
    args.target          = n.target;
    args.targetNoArticle = n.targetNoArticle;
    args.strings         = { n.strings[0], n.strings[1], n.strings[2], n.strings[3] };
    for (auto& l : dat::formatDialog(table.raw(n.message), args))
    {
        l.erase(0, l.find_first_not_of(' ')); // #164 opens with 7F 80 and an article code before its space
        if (l.empty() || l == "\x0B")
        {
            continue;
        }
        if (l[0] >= 'a' && l[0] <= 'z')
        {
            l[0] = static_cast<char>(l[0] - 'a' + 'A');
        }
        out.push_back(std::move(l));
    }
    return out;
}

} // namespace xi::client
