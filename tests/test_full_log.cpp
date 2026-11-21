// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/full_log.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

// The log's line wrap and the full-screen log's channel tabs (issue #92; docs/ui-format-notes.md §10.2).
using namespace xi::ui;

namespace
{
// A fixed 10-unit advance per character.
std::vector<LogRow> wrap10(const std::string& s, std::size_t stamp, float maxW)
{
    return wrapLogLine(s, stamp, maxW, [](std::string_view t) { return 10.0f * static_cast<float>(t.size()); }, [](unsigned char) { return 10.0f; });
}
} // namespace

TEST_CASE("a word wider than the log after a wrapped first row does not hang", "[full-log]")
{
    // Issue #92: the full-screen log is 352 wide, and an addon's error line ("...l/share/ffxi-linux/game/addons/
    // filterless/filterless.lua:45: in function") wrapped to a continuation row whose only spaces were the indent's,
    // so the cut fell inside the indent and the row came back unchanged forever.
    const std::string path = "ab " + std::string(40, 'x') + " tail";
    const auto        rows = wrap10(path, 0, 100.0f);
    REQUIRE(rows.size() < 20);
    std::string joined;
    for (const auto& r : rows)
    {
        CHECK(r.text.size() * 10 <= 100);
        CHECK(r.text.find_first_not_of(' ') != std::string::npos); // no blank rows
        joined += r.text;
    }
    // Every character arrives once (the continuation indent and the break spaces aside).
    std::string expect = path;
    std::erase(expect, ' ');
    std::erase(joined, ' ');
    CHECK(joined == expect);
}

TEST_CASE("lines break at the last space that fits, after the timestamp", "[full-log]")
{
    const auto rows = wrap10("[1] aaa bbb ccc", 4, 80.0f);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].text == "[1] aaa");
    CHECK(rows[0].stamp == 4);
    CHECK(rows[1].text == "  bbb"); // continuation rows are indented two, which counts toward the width
    CHECK(rows[1].stamp == 0);
    CHECK(rows[2].text == "  ccc");
    // A line that fits is one row, unchanged.
    const auto one = wrap10("short", 0, 80.0f);
    REQUIRE(one.size() == 1);
    CHECK(one[0].text == "short");
}

TEST_CASE("a window narrower than one character still makes progress", "[full-log]")
{
    const auto rows = wrap10("abcdef", 0, 5.0f);
    CHECK(rows.size() <= 6);
    CHECK_FALSE(rows.empty());
}

TEST_CASE("the full log's tabs and their order", "[full-log]")
{
    const auto one = fullLogTabs(false);
    REQUIRE(one.size() == 10);
    CHECK(one.front().filter == 0);
    CHECK(one.front().label == 0);
    for (const auto& t : one)
    {
        CHECK(t.filter != 8); // window 2's tab only with two log windows
    }
    const auto two = fullLogTabs(true);
    REQUIRE(two.size() == 11);
    CHECK(two[1].filter == 8);
    CHECK(two[1].label == 1);
    // Select Window walks them in order and wraps: Window1, Say, Tell, Party, Linkshell, Linkshell2, AssistE, AssistJ,
    // Unity, Shout.
    uint8_t                 f = 0;
    std::vector<uint8_t>    seen;
    for (int i = 0; i < 10; ++i)
    {
        seen.push_back(f);
        f = nextFullLogFilter(one, f);
    }
    CHECK(seen == std::vector<uint8_t>{ 0, 3, 4, 1, 2, 7, 10, 9, 6, 5 });
    CHECK(f == 0);
    CHECK(nextFullLogFilter(one, 8) == 0); // window 2's tab gone: back to the first
    // Every label has its own pair of sprites.
    std::set<int> sprites;
    for (const auto& t : two)
    {
        const auto s = fullLogTabSprites(t.label);
        sprites.insert(s.normal);
        sprites.insert(s.selected);
    }
    CHECK(sprites.size() == 22);
}

TEST_CASE("channel tabs pick their chat kinds", "[full-log]")
{
    CHECK(fullLogFilterMatches(3, 0));  // say
    CHECK(fullLogFilterMatches(4, 3));  // tell
    CHECK(fullLogFilterMatches(1, 4));  // party
    CHECK(fullLogFilterMatches(2, 5));  // linkshell
    CHECK(fullLogFilterMatches(7, 27)); // linkshell 2
    CHECK(fullLogFilterMatches(5, 1));  // shout
    CHECK(fullLogFilterMatches(5, 26)); // yell
    CHECK(fullLogFilterMatches(6, 33)); // unity
    CHECK(fullLogFilterMatches(9, 0x22));
    CHECK(fullLogFilterMatches(10, 0x23));
    CHECK_FALSE(fullLogFilterMatches(3, 3));
    CHECK_FALSE(fullLogFilterMatches(4, 0));
    CHECK_FALSE(fullLogFilterMatches(0, 0)); // tabs 0 and 8 route by the log windows instead
}

TEST_CASE("issue #104: a new line returns the main log to the newest line; the full log keeps its place", "[ui][log]")
{
    // A wheel notch left the log three rows back, and the !zone echo and the area line that followed stayed hidden.
    CHECK(logScrollAfter(3, true, false) == 0);
    CHECK(logScrollAfter(3, false, false) == 3); // no new line: looking back stays where it is
    CHECK(logScrollAfter(0, true, false) == 0);
    CHECK(logScrollAfter(5, true, true) == 5); // the full-screen log scrolls on its own
}

TEST_CASE("issue #105: the chat line never covers the log's text", "[ui][log]")
{
    // Opening the chat line returns a looked-back log to the newest line (the report's log was two rows back, with the
    // page marker showing, so the newest lines sat under the typing box).
    CHECK(logScrollAfter(2, false, false, true) == 0);
    CHECK(logScrollAfter(2, false, true, true) == 2); // the full-screen log keeps its place
    CHECK(logScrollAfter(2, false, false, false) == 2);
    // While typing the window keeps its height and shows one row fewer, moved up a row: the last text row ends exactly
    // where the 22-tall input line starts (CODE 0x101625f0), for every window size Config allows (4..20 lines).
    for (int rows = 4; rows <= 20; ++rows)
    {
        CHECK(logTextRows(rows, false) == rows);
        CHECK(logTextRows(rows, true) == rows - 1);
        CHECK(logTextBottom(rows, true) <= logInputTop(rows));
        CHECK(logTextBottom(rows, true) > logInputTop(rows) - 16); // and not a whole row higher
        CHECK(logTextBottom(rows, false) <= static_cast<float>(rows) * 16 + 8); // inside the window when not typing
    }
    CHECK(logInputTop(8) == 114.0f); // 8 lines: 136 tall, the input line from 114
    CHECK(logTextRows(0, true) == 0);
}

TEST_CASE("issue #108: the chat line is as wide as the log window above it", "[ui][log]")
{
    // Both windows sit at x 16; the log is its column + 14 wide (0x10162ca0) and the chat line takes log window 1's
    // column + 14 when it opens (0x101b7ab0), so their left and right edges match for every column width.
    for (const float ui : { 512.0f, 640.0f, 800.0f, 1024.0f, 1600.0f })
    {
        for (const int setting : { 0, 10, 25, 50, 75, 100 })
        {
            const float column = logColumnWidth(ui, setting);
            CHECK(column >= 352.0f);
            CHECK(column <= std::max(352.0f, ui - 160.0f));
            CHECK(logWindowWidth(column) == column + 14.0f);
        }
    }
    CHECK(logWindowWidth(logColumnWidth(800.0f, 0)) == 366.0f);   // the authored "menu inline" / "menu logwindo" width
    CHECK(logColumnWidth(1024.0f, 50) == 352.0f + 256.0f);        // 352 + (1024 - 512) * 50 / 100
    CHECK(logColumnWidth(800.0f, 100) == 640.0f);                 // capped at UI width - 160
    CHECK(logWindowWidth(logColumnWidth(1024.0f, 50)) == 622.0f); // a widened log: the chat line follows it
}
