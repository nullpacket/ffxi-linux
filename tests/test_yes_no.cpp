// SPDX-License-Identifier: GPL-3.0-or-later
// The game's yes / no dialog (issue #118; ui/yes_no.h): which element it opens on, how its keys answer, and (with the
// user's install) Auto-sort's question and title strings.
#include "dat/dat_index.h"
#include "dat/messages.h"
#include "ui/yes_no.h"
#include "xi/install.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

using namespace xi::ui;

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

TEST_CASE("yes / no: the dialog opens on No unless asked to open on Yes", "[ui][yesno]")
{
    // 0x102222d0: startOnYes == 0 -> 0x10118dc0(2), the cursor on element 2 (No).
    CHECK(yesNoCursor(false) == kYesNoNo);
    CHECK(yesNoCursor(true) == kYesNoYes);
    // Auto-sort passes 0 (CODE 0x101f37a4), so "Sort items?" opens on No.
    CHECK(yesNoCursor(kSortConfirmOnYes) == kYesNoNo);
}

TEST_CASE("yes / no: Confirm answers by element, Cancel always answers no", "[ui][yesno]")
{
    // 0x10222410: event 5 on element 1 -> yes, on element 2 -> no; event 6 -> no.
    CHECK(yesNoAnswer(true, false, kYesNoYes) == std::optional<bool>(true));
    CHECK(yesNoAnswer(true, false, kYesNoNo) == std::optional<bool>(false));
    CHECK(yesNoAnswer(false, true, kYesNoYes) == std::optional<bool>(false));
    CHECK(yesNoAnswer(false, true, kYesNoNo) == std::optional<bool>(false));
    // Other keys (cursor moves) and other elements (the title tab, 3) answer nothing.
    CHECK_FALSE(yesNoAnswer(false, false, kYesNoYes).has_value());
    CHECK_FALSE(yesNoAnswer(true, false, 3).has_value());
}

TEST_CASE("yes / no: Auto-sort asks \"Sort items?\" titled \"Items\" (needs the game data)", "[ui][yesno][data]")
{
    const auto* idx = install();
    if (!idx)
    {
        SKIP("no game data");
    }
    const auto question = xi::dat::StringTable::parse(idx->read(55653)); // string table 0xc
    const auto title    = xi::dat::StringTable::parse(idx->read(55652)); // string table 0xd
    CHECK(question.get(kSortConfirmQuestion) == "Sort items?");
    CHECK(title.get(kSortConfirmTitle) == "Items");
}

TEST_CASE("yes / no: Drop asks \"Dispose of item?\" on No, and again for Rare / Ex items and linkshells", "[ui][yesno]")
{
    // CODE 0x101f2f96 passes startOnYes 0; the second prompt (0x101f2d21 -> 0x102109e0) passes 0 too (#119).
    CHECK(yesNoCursor(kDropConfirmOnYes) == kYesNoNo);
    CHECK(yesNoCursor(kDropAgainOnYes) == kYesNoNo);
    CHECK_FALSE(dropAsksAgain(0x0000, 4096));     // a plain item (a crystal)
    CHECK(dropAsksAgain(0x8000, 1));              // Rare
    CHECK(dropAsksAgain(0x4000, 1));              // Ex
    CHECK(dropAsksAgain(0x0000, kLinkshellItem)); // a linkshell
    CHECK_FALSE(dropAsksAgain(0x0400, 1));        // other flags (usable) don't
}

TEST_CASE("yes / no: Log out, Shut down, the home point and the macro prompts open on No", "[ui][yesno]")
{
    // 0x102109e0's fourth argument is 0 at 0x10160950, 0x101608c3, 0x1012dfbc, 0x101a5da0 and 0x101a6787.
    CHECK(yesNoCursor(kLogoutConfirmOnYes) == kYesNoNo);
    CHECK(yesNoCursor(kHomePointConfirmOnYes) == kYesNoNo);
    CHECK(yesNoCursor(kMacroConfirmOnYes) == kYesNoNo);
}

TEST_CASE("yes / no: Drop's strings (needs the game data)", "[ui][yesno][data]")
{
    const auto* idx = install();
    if (!idx)
    {
        SKIP("no game data");
    }
    const auto question = xi::dat::StringTable::parse(idx->read(55653));   // string table 0xc
    const auto title    = xi::dat::StringTable::parse(idx->read(55652));   // string table 0xd
    const auto again    = xi::dat::XiStringTable::parse(idx->read(55649)); // string table 0xa (XISTRING)
    CHECK(question.get(kDropConfirmQuestion) == "Dispose of item?");
    CHECK(title.get(kDropConfirmTitle) == "Items");
    CHECK(again.get(kDropAgainQuestion) == "Really dispose of the %s?");
}
