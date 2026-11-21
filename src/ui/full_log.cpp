// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/full_log.h"

#include <algorithm>
#include <array>

namespace xi::ui
{

std::vector<LogRow> wrapLogLine(std::string_view text, std::size_t stamp, float maxW, const std::function<float(std::string_view)>& width,
                                const std::function<float(unsigned char)>& advance)
{
    std::vector<LogRow> rows;
    std::string         rest(text);
    std::size_t         indent = 0; // the continuation's two spaces
    while (!rest.empty())
    {
        std::size_t cut = rest.size();
        if (width(rest) > maxW)
        {
            // Longest prefix that fits, broken at a space when there is one. A break must leave something past the
            // timestamp and the indent on this row, so that each row consumes text.
            float       w     = 0;
            std::size_t fit   = 0;
            std::size_t space = 0;
            for (; fit < rest.size(); ++fit)
            {
                w += advance(static_cast<unsigned char>(rest[fit]));
                if (w > maxW)
                {
                    break;
                }
                if (rest[fit] == ' ' && fit >= stamp && fit > indent)
                {
                    space = fit;
                }
            }
            cut = space > 0 ? space : std::max(indent + 1, fit);
            cut = std::min(cut, rest.size());
        }
        rows.push_back({ rest.substr(0, cut), std::min(stamp, cut) });
        rest   = cut < rest.size() ? rest.substr(rest[cut] == ' ' ? cut + 1 : cut) : std::string();
        stamp  = 0;
        indent = 0;
        if (!rest.empty())
        {
            rest   = "  " + rest; // continuation indent
            indent = 2;
        }
    }
    return rows;
}

std::vector<FullLogTab> fullLogTabs(bool secondWindow)
{
    // The pairs at 0x1036f950 as 0x10138630 fills them for an English client ([0x10456a28]+8 != 0: filter 10 before 9,
    // label 8 before 7); 0x10138730 drops the label-1 entry without a second log window.
    static constexpr std::array<FullLogTab, 11> kTabs{ { { 0, 0 }, { 8, 1 }, { 3, 2 }, { 4, 3 }, { 1, 4 }, { 2, 5 }, { 7, 6 }, { 10, 8 }, { 9, 7 }, { 6, 9 }, { 5, 10 } } };
    std::vector<FullLogTab>                     tabs;
    for (const auto& t : kTabs)
    {
        if (t.label != 1 || secondWindow)
        {
            tabs.push_back(t);
        }
    }
    return tabs;
}

uint8_t nextFullLogFilter(const std::vector<FullLogTab>& tabs, uint8_t filter)
{
    for (std::size_t i = 0; i < tabs.size(); ++i)
    {
        if (tabs[i].filter == filter)
        {
            return tabs[(i + 1) % tabs.size()].filter;
        }
    }
    return tabs.empty() ? 0 : tabs.front().filter; // not listed (window 2 switched off): back to the first tab
}

FullLogTabSprites fullLogTabSprites(uint8_t label)
{
    // 0x10333b4c: per label the unselected and the selected sprite index. Read from the sprites' letters: 0 Window1,
    // 1 Window2, 2 Say, 3 Tell, 4 Party, 5 Linkshell, 6 Linkshell2, 7 AssistJ, 8 AssistE, 9 Unity, 10 Shout.
    static constexpr std::array<FullLogTabSprites, 11> kSprites{ { { 0x12, 0x1b },
                                                                    { 0x13, 0x1c },
                                                                    { 0x0a, 0x14 },
                                                                    { 0x0c, 0x16 },
                                                                    { 0x0d, 0x17 },
                                                                    { 0x0e, 0x18 },
                                                                    { 0x10, 0x19 },
                                                                    { 0x1d, 0x1f },
                                                                    { 0x1e, 0x20 },
                                                                    { 0x11, 0x1a },
                                                                    { 0x0b, 0x15 } } };
    return label < kSprites.size() ? kSprites[label] : FullLogTabSprites{};
}

bool fullLogFilterMatches(uint8_t filter, uint8_t kind)
{
    // 0x10129920 tests the line's log mode (own / others'): 3 say 1/9, 4 tell 4/12, 1 party 5/13/0xd2, 2 linkshell
    // 6/14, 7 linkshell 2 0xd5/0xd6, 9 0xdb/0xdc, 10 0xdd/0xde, 6 0xd3/0xd4, 5 shout and yell 2/10/3/11. The modes are
    // matched to this client's chat kinds by the tab labels (9 AssistJ, 10 AssistE, 6 Unity).
    switch (filter)
    {
        case 1: return kind == 4;
        case 2: return kind == 5;
        case 3: return kind == 0;
        case 4: return kind == 3;
        case 5: return kind == 1 || kind == 26;
        case 6: return kind == 33;
        case 7: return kind == 27;
        case 9: return kind == 0x22;
        case 10: return kind == 0x23;
        default: return false;
    }
}

int logScrollAfter(int scroll, bool newLine, bool fullLog, bool lineOpened)
{
    return (newLine || lineOpened) && !fullLog ? 0 : scroll;
}

namespace
{
constexpr float kLogLine = 16, kLogPad = 4, kInputH = 22;
}

int logTextRows(int rows, bool typing)
{
    return std::max(0, typing ? rows - 1 : rows);
}

float logInputTop(int rows)
{
    return static_cast<float>(rows) * kLogLine + 2 * kLogPad - kInputH;
}

float logTextBottom(int rows, bool typing)
{
    // The text fills the window's top rows: the last one is row (rows - 1), or (rows - 2) while typing.
    return kLogPad + static_cast<float>(logTextRows(rows, typing)) * kLogLine - 2;
}

float logColumnWidth(float uiWidth, int setting)
{
    const float column = 352.0f + (uiWidth - 512.0f) * static_cast<float>(setting) / 100.0f;
    return std::clamp(column, 352.0f, std::max(352.0f, uiWidth - 160.0f));
}

float logWindowWidth(float column)
{
    return column + 14.0f;
}

} // namespace xi::ui
