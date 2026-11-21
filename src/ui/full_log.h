// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The log window's line wrapping and the full-screen log's channel tabs (docs/ui-format-notes.md §10.2).
namespace xi::ui
{

// One wrapped row of a log line; `stamp` is how many leading characters are the timestamp.
struct LogRow
{
    std::string text;
    std::size_t stamp = 0;
};

// Wraps a log line to `maxW` (authored units). Lines break at the last space that fits, after the timestamp and the
// continuation indent; a word wider than the window is cut where it stops fitting. Continuation rows start with two
// spaces. `width` measures a whole string, `advance` one byte (the prefix search).
// Every row takes at least one character past its indent, so a word wider than the window can no longer loop forever
// on the indent's own spaces (issue #92: the full-screen log is 352 wide, and a long addon path hung the client).
std::vector<LogRow> wrapLogLine(std::string_view text, std::size_t stamp, float maxW, const std::function<float(std::string_view)>& width,
                                const std::function<float(unsigned char)>& advance);

// The full-screen log's tabs ("menu fulllog", object 0x105781e0; CODE 0x10138630 / 0x10138730): the filter (log
// object 0x10578158, predicate 0x10129920) and the label (its sprites in "menu fep") of each tab, left to right, in
// the English client's order. Filter 8 (log window 2's lines) is only listed with Config "multiple log windows" on
// (0x10193760(0xb4)).
struct FullLogTab
{
    uint8_t filter = 0;
    uint8_t label  = 0;
};
std::vector<FullLogTab> fullLogTabs(bool secondWindow);

// The tab after `filter` (Select Window in the full log, CODE 0x10138840), wrapping to the first.
uint8_t nextFullLogFilter(const std::vector<FullLogTab>& tabs, uint8_t filter);

// "menu fep" sprites of a tab's label: {unselected, selected} (table 0x10333b4c, read at 0x101387be).
struct FullLogTabSprites
{
    int normal   = 0;
    int selected = 0;
};
FullLogTabSprites fullLogTabSprites(uint8_t label);

// Whether a line of chat kind `kind` (client::ChatLine::kind) belongs to channel filter `filter` (1..7, 9, 10; CODE
// 0x10129920 by log mode). Filters 0 and 8 route by the log windows' masks instead (the caller's inWindow).
bool fullLogFilterMatches(uint8_t filter, uint8_t kind);

// The main log window's scroll (rows up from the newest) after a frame. The game's log window never stays scrolled
// back: its frame (CODE 0x101625f0) walks the window's start line (+0x2c) toward the newest line every frame (one line
// per step, jumping when 0x41 or more behind; +0x2c is otherwise only set by the constructor 0x10161de0 and the reset
// 0x10162c70), and no input moves it back; only the full-screen log scrolls. ffxi-linux lets the mouse wheel look
// back over the log (not the game's), so a new line returns the window to the newest line, as the game shows it
// (issue #104: a wheel notch left the newest rows, the area line among them, hidden below the window). Opening the
// chat line does the same (issue #105: a look-back left the newest rows hidden while typing). The full log keeps its
// place.
int logScrollAfter(int scroll, bool newLine, bool fullLog, bool lineOpened = false);

// The main log window's text rows while the chat line ("menu inline", 22 tall, bottom-aligned with the log: the
// layouts' 298 + 134 = 410 + 22) is open. The game keeps the window's height and moves its text up a row instead
// (CODE 0x101625f0: with the input line open, 0x1011b970 on 0x10630278, the drawn line count +0x30 drops by one and
// the text rect's bottom +0x46 and the text offset +0x2e move up 0x10), so the input line covers the window's bottom
// padding and the freed row, never text. `rows` is the window's line count (its height is rows * 16 + 8).
int logTextRows(int rows, bool typing);

// The top of the input line inside a log window of `rows` lines (authored units from the window's top), and the
// bottom of the last text row's cell (text at window top + 4, 16 a line, drawn 2 above the cell; CODE 0x101621f0).
// While typing the text ends at or above the input line.
float logInputTop(int rows);
float logTextBottom(int rows, bool typing);

// A log window's text column (Config "column width", ids 156 / 197): 352 + (UI width - 512) * setting / 100, at most
// UI width - 160 and at least 352 (CODE 0x10161a20, stored at the window's +0x52 and copied to +0x32 by its reset
// 0x10162ca0).
float logColumnWidth(float uiWidth, int setting);

// A log window's width for a text column: the column + 14 (the reset 0x10162ca0 resizes the window to +0x32 + 0xe).
// The chat line ("menu inline") takes the same width when it opens (issue #108): its open handler (0x101b7ab0, vtable
// 0x10337a40 slot 1) resizes it (0x1011a4d0) to log window 1's +0x32 + 0xe, keeping its own position and height, so
// both share their left and right edges (both sit at x 16).
float logWindowWidth(float column);

} // namespace xi::ui
