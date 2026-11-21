// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

namespace xi::ui
{
// The game's yes / no dialog object (CODE 0x102222d0 opens it, 0x10222410 is its key handler; docs/ui-format-notes.md
// §10.12). Its windows ("menu sortyn", "menu yesno", ...) have Yes as element 1 and No as element 2.
//   0x102222d0(callback, context, question, title, startOnYes, yOffset): the question goes to the help line
//   (0x1013a250), the title to the title window (0x10150bf0(title, 2)); with startOnYes == 0 the cursor is put on
//   element 2 (0x10118dc0(2)), so it opens on No; otherwise it stays on element 1 (Yes).
//   0x10222410: Confirm (event 5) on element 1 answers yes, on element 2 no; Cancel (event 6) answers no. The callback
//   gets (context, answer) and the dialog closes.
constexpr int kYesNoYes = 1;
constexpr int kYesNoNo  = 2;

constexpr int yesNoCursor(bool startOnYes)
{
    return startOnYes ? kYesNoYes : kYesNoNo;
}

// The answer for a key in the dialog: Confirm on Yes / No, or Cancel; nothing for other keys or elements.
constexpr std::optional<bool> yesNoAnswer(bool confirm, bool cancel, int element)
{
    if (cancel)
    {
        return false;
    }
    if (!confirm || (element != kYesNoYes && element != kYesNoNo))
    {
        return std::nullopt;
    }
    return element == kYesNoYes;
}

// Auto-sort in the Items sort window (CODE 0x101f37a4): 0x102222d0(0x101f3330, the list's selected item,
// 55653 #61 "Sort items?", 55652 #2 "Items", startOnYes 0, 0) on "menu sortyn" (0x1038610c). Yes (0x101f3330): C2S 0x03A
// for the list's container (0x100ebe70), its display order (0x1021c320), and the focus back on the list (0x101f32f0).
// No: the dialog closes and the sort window keeps the focus.
constexpr int  kSortConfirmQuestion = 61; // string table 0xc = file 55653
constexpr int  kSortConfirmTitle    = 2;  // string table 0xd = file 55652
constexpr bool kSortConfirmOnYes    = false;

// Drop, from an item's options (#119; CODE 0x101f2f96, the Items list's handler for the option): 0x102222d0(0x101f2c70,
// the list's selected item, 55653 #62 "Dispose of item?", 55652 #2 "Items", startOnYes 0, 0), so it opens on No.
// Yes (0x101f2c70): a Rare or Ex item (the item record's flags 0xc000) or a linkshell (item 513) first asks again with
// the "menu comyn" dialog (0x10631a00 -> 0x102109e0, startOnYes 0): 55649 #48 "Really dispose of the %s?" with the
// item's name; otherwise (and on that dialog's Yes) 0x101f2ba0 sends the drop (C2S 0x028). Two more checks on that path
// (0x101cb450 / 0x101cb540) were not traced, and the linkshell's own follow-up prompts (55648 #343 / #344, 0x101f2bf0)
// are not shown.
constexpr int      kDropConfirmQuestion = 62; // string table 0xc = file 55653
constexpr int      kDropConfirmTitle    = 2;  // string table 0xd = file 55652
constexpr bool     kDropConfirmOnYes    = false;
constexpr int      kDropAgainQuestion   = 48; // string table 0xa = file 55649, "Really dispose of the %s?"
constexpr bool     kDropAgainOnYes      = false;
constexpr uint16_t kLinkshellItem       = 513;

constexpr bool dropAsksAgain(uint16_t itemFlags, uint16_t itemId)
{
    return (itemFlags & 0xC000) != 0 || itemId == kLinkshellItem;
}

// Other yes / no prompts and what their game callers pass as startOnYes (0x102109e0's fourth argument; 0 puts the cursor
// on No through 0x10118dc0(2)): Log out 0x10160950 and Shut down 0x101608c3 pass 0; the home point prompt 0x1012dfbc
// passes 0; the macro book / set Paste and Delete prompts 0x101a5da0 (55648 #238) and 0x101a6787 (#239 / #240) pass 0.
constexpr bool kLogoutConfirmOnYes    = false;
constexpr bool kHomePointConfirmOnYes = false;
constexpr bool kMacroConfirmOnYes     = false;

} // namespace xi::ui
