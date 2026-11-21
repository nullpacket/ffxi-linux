// SPDX-License-Identifier: GPL-3.0-or-later
// The chat line's auto-translate (PLANS goal 39): Tab opens the candidate list "menu fep" over the line (the candidates
// for the typed word, or with nothing typed the dictionary's categories, then a category's phrases); a chosen phrase
// goes into the line as one token (FD + 4 bytes + FD, dat::AutoTranslate) drawn between the game's gaiji brackets
// ("font usgaiji" 8 / 9: "menu markgree" "{" and "menu markred" "}"). ("menu fp_cat" / "fp_cat2" / "fp_info" /
// "fp_help" are the Alter Ego points windows, not these.)
#include "ui/game_ui.h"

#include "ui/layout_view.h"

#include <algorithm>
#include <cctype>

namespace xi::ui
{

namespace
{
constexpr UiColor kText{ 0x80, 0x80, 0x80, 0x80 };
constexpr std::size_t kLineMax       = 120;            // bytes, FD tokens counted whole (CODE 0x101ac8b9 checks the length)
constexpr UiColor     kNote{ 0x40, 0x40, 0x40, 0x80 };   // a row's note after its 0x06 (CODE 0x1018f285: 0x80404040)
constexpr int         kCandidateRows = 7;              // "menu fep": 7 candidates a page (CODE 0x1018f540: idiv 7)
constexpr std::size_t kCandidateMax  = 200;

bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && std::ranges::equal(text.substr(0, prefix.size()), prefix, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}
} // namespace

void GameUi::chatInsert(std::string_view text)
{
    cursor_ = std::min(cursor_, input_.size());
    for (const char ch : text)
    {
        if (static_cast<unsigned char>(ch) >= 0x20 && static_cast<unsigned char>(ch) < 0x7F && input_.size() < kLineMax)
        {
            input_.insert(input_.begin() + static_cast<std::ptrdiff_t>(cursor_), ch);
            ++cursor_;
        }
    }
}

void GameUi::atInsert(const std::string& token, std::size_t from)
{
    // The typed word [from, cursor) gives way to the token; a space after it keeps the next word apart.
    from = std::min(from, cursor_);
    std::string piece = token;
    if (cursor_ >= input_.size() || input_[cursor_] != ' ')
    {
        piece += ' ';
    }
    if (input_.size() - (cursor_ - from) + piece.size() > kLineMax)
    {
        return;
    }
    input_.replace(from, cursor_ - from, piece);
    cursor_ = from + piece.size();
}

void GameUi::atOpen()
{
    const auto* at = text_ ? text_->autoTranslate() : nullptr;
    if (!at)
    {
        return;
    }
    cursor_          = std::min(cursor_, input_.size());
    const auto from  = dat::chatedit::wordStart(input_, cursor_);
    const auto word  = std::string_view(input_).substr(from, cursor_ - from);
    at_              = {};
    if (word.empty())
    {
        // Nothing typed: the categories (the dictionary's groups, file order).
        for (std::size_t g = 0; g < at->groups().size(); ++g)
        {
            at_.rows.push_back({ {}, "{" + at->groups()[g].name + "}", static_cast<int>(g) });
        }
        at_.mode = AtWindow::Mode::Categories;
        return;
    }
    // Candidates: phrases starting with the word, then items and key items (INFERRED order).
    for (const auto& e : at->search(word))
    {
        at_.rows.push_back({ dat::AutoTranslate::phraseToken(e.id), e.text });
    }
    if (const auto* items = text_->items())
    {
        if (atItems_.empty())
        {
            for (uint32_t id = 1; id < 0x10000; ++id)
            {
                if (const auto info = items->find(static_cast<uint16_t>(id)); info && !info->name.empty() && info->name != ".")
                {
                    atItems_.emplace_back(info->name, static_cast<uint16_t>(id));
                }
            }
            std::ranges::sort(atItems_);
        }
        for (auto it = std::ranges::lower_bound(atItems_, std::pair<std::string, uint16_t>{ std::string(word), 0 },
                                                [](const auto& a, const auto& b) {
                                                    return std::ranges::lexicographical_compare(a.first, b.first, [](char x, char y) {
                                                        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
                                                    });
                                                });
             it != atItems_.end() && startsWith(it->first, word) && at_.rows.size() < kCandidateMax; ++it)
        {
            at_.rows.push_back({ dat::AutoTranslate::itemToken(it->second), it->first });
        }
    }
    if (const auto* keys = text_->keyItems())
    {
        std::vector<AtRow> ki;
        for (const auto& [id, k] : keys->all())
        {
            if (id > 0 && id < 0x10000 && startsWith(k.name, word))
            {
                ki.push_back({ dat::AutoTranslate::keyItemToken(static_cast<uint16_t>(id)), k.name });
            }
        }
        std::ranges::sort(ki, {}, &AtRow::text);
        for (auto& r : ki)
        {
            if (at_.rows.size() < kCandidateMax)
            {
                at_.rows.push_back(std::move(r));
            }
        }
    }
    if (at_.rows.empty())
    {
        return;
    }
    at_.wordFrom = from;
    at_.mode     = AtWindow::Mode::Candidates;
}

bool GameUi::atKey(SDL_Keycode key)
{
    if (at_.mode == AtWindow::Mode::Closed)
    {
        return false;
    }
    const int n    = static_cast<int>(at_.rows.size());
    const int page = kCandidateRows;
    auto      move = [&](int to) {
        // The list shows whole pages of 7 (CODE 0x1018f540); the game's only sound here is the cursor's se 0001 when
        // the candidate changes (0x1018f43e -> 0x10037a60).
        const int was = at_.sel;
        at_.sel       = n == 0 ? 0 : (to % n + n) % n;
        at_.top       = at_.sel / page * page;
        if (at_.sel != was)
        {
            sound_(1u);
        }
    };
    switch (key)
    {
        case SDLK_UP: move(at_.sel - 1); return true;
        case SDLK_DOWN: move(at_.sel + 1); return true;
        case SDLK_TAB: move(at_.sel + ((heldMods() & SDL_KMOD_SHIFT) ? -1 : 1)); return true;
        case SDLK_PAGEUP: move(std::max(0, at_.sel - page)); return true;
        case SDLK_PAGEDOWN: move(std::min(n - 1, at_.sel + page)); return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_RIGHT:
        {
            if (n == 0)
            {
                return true;
            }
            const auto row = at_.rows[static_cast<std::size_t>(at_.sel)];
            if (at_.mode == AtWindow::Mode::Categories)
            {
                // A category opens its phrases.
                const auto* at  = text_ ? text_->autoTranslate() : nullptr;
                const int   cat = at_.sel;
                if (!at || row.group < 0)
                {
                    return true;
                }
                at_.rows.clear();
                for (const auto& e : at->groups()[static_cast<std::size_t>(row.group)].entries)
                {
                    at_.rows.push_back({ dat::AutoTranslate::phraseToken(e.id), e.text, row.group });
                }
                at_.mode     = AtWindow::Mode::Phrases;
                at_.category = cat;
                at_.sel = at_.top = 0;
                return true;
            }
            if (key == SDLK_RIGHT)
            {
                return true;
            }
            atInsert(row.token, at_.mode == AtWindow::Mode::Candidates ? at_.wordFrom : cursor_);
            at_ = {};
            return true;
        }
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
        case SDLK_LEFT:
            if (at_.mode == AtWindow::Mode::Phrases)
            {
                // Back to the categories, on the one that was open.
                const int cat = at_.category;
                at_           = {};
                atOpen();
                at_.sel = cat;
                at_.top = cat / kCandidateRows * kCandidateRows;
                return true;
            }
            if (key == SDLK_LEFT)
            {
                return true;
            }
            at_ = {};
            return true;
        default: return true; // the line waits while a window is open
    }
}

float GameUi::drawChatText(UiPainter& p, std::string_view s, ImVec2 at, UiColor colour, std::size_t cursor) const
{
    // Plain text in `colour`; a token as its text between the game's gaiji brackets (EF 27 / EF 28). The text cursor
    // (when `cursor` is a position) is an underscore at its byte position.
    const auto* atr      = text_ ? text_->autoTranslate() : nullptr;
    float       x        = at.x;
    float       cursorX  = -1;
    std::size_t i        = 0;
    std::string run;
    auto flush = [&] {
        if (!run.empty())
        {
            x = p.text(run, { x, at.y }, colour);
            run.clear();
        }
    };
    while (i < s.size())
    {
        if (i == cursor)
        {
            flush();
            cursorX = x;
        }
        if (const auto t = dat::AutoTranslate::tokenAt(s, i))
        {
            const auto text = atr ? atr->tokenText(*t) : std::string();
            run += dat::AutoTranslate::kOpenMark;
            run += text.empty() ? std::string("...") : text;
            run += dat::AutoTranslate::kCloseMark;
            i += 6;
            continue;
        }
        run += s[i++];
    }
    flush();
    if (cursor != std::string::npos && cursorX < 0)
    {
        cursorX = x;
    }
    if (cursorX >= 0 && (tick_ / 30) % 2 == 0)
    {
        p.text("_", { cursorX, at.y }, colour);
    }
    return x;
}

float GameUi::drawLogText(UiPainter& p, std::string_view s, ImVec2 at, UiColor colour) const
{
    // The session keeps a received token between the gaiji brackets (AutoTranslate::expandMarked), which the text
    // renderer draws from "font usgaiji"; braces a player typed stay plain text.
    return p.text(s, at, colour);
}

void GameUi::atDraw(const UiCanvas& c, UiPainter& p, ImVec2 lineAt)
{
    if (at_.mode == AtWindow::Mode::Closed)
    {
        return;
    }
    // "menu fep" (96 x 134; CODE 0x1018f2b0 / 0x1018f460): as wide as its longest candidate, at least 90, plus 12, up
    // to 200; a top row with a mark (Shift-JIS 81 A3) and "index/count" ("%d/%d", "0/0" when empty) at the window's
    // x + 9 / + 39, then a page of 7 candidates 16 apart from y + 18 at x + 4, too-long ones cut with "...", a note
    // after a 0x06 in grey. The cursor is the layout's element (index % 7) + 2. The window stands at the line's caret
    // and is pushed left to stay 16 inside the screen (0x1018ef20). INFERRED: above the line (the anchor object was not
    // followed); the categories and their phrases use the same window.
    const int n    = static_cast<int>(at_.rows.size());
    float     wide = 0;
    for (const auto& r : at_.rows)
    {
        wide = std::max(wide, p.textWidth(r.text) / c.scale);
    }
    const float w      = std::min(std::max(wide, 90.0f) + 12, 200.0f);
    const float h      = 134;
    const auto  anchor = at_.mode == AtWindow::Mode::Candidates ? at_.wordFrom : cursor_;
    const float caretX = (lineAt.x + p.textWidth(std::string_view(input_).substr(0, std::min(anchor, input_.size())))) / c.scale;
    const float x      = std::clamp(caretX, 4.0f, std::max(4.0f, c.width / c.scale - w - 16));
    const float y      = lineAt.y / c.scale - h - 4;
    p.frame({ x * c.scale, y * c.scale }, { w, h });
    p.text("\x81\xa3", { (x + 9) * c.scale, (y + 2) * c.scale }, kText);
    p.text(n > 0 ? std::to_string(at_.sel + 1) + "/" + std::to_string(n) : std::string("0/0"), { (x + 39) * c.scale, (y + 2) * c.scale }, kText);
    // The cursor (0x10118ce0 -> 0x101180a0 / 0x10119d60): the layout's window cursor sub (type 6) placed on element
    // (index % 7) + 2. For "menu fep" that is not the menus' hand but its own shape 2, a 92 x 16 bar from "menu gauge"
    // (rgba 54545420) at the element's (3, 20 + 16 k), drawn under the row's text. INFERRED: stretched to the window's
    // width - 4 when the window grows past 96 (the layout's 92 in 96).
    if (n > 0)
    {
        const auto& layouts = p.renderer().resources().layouts;
        const auto  lit     = layouts.find("menu fep");
        const auto* row     = lit != layouts.end() ? lit->second.element(static_cast<uint16_t>(at_.sel % kCandidateRows + 2)) : nullptr;
        if (row)
        {
            for (const auto& sub : lit->second.window.subs)
            {
                const auto* bar = sub.type == 6 ? p.renderer().resources().sprite(sub.sheet, sub.shape) : nullptr;
                if (!bar || bar->empty())
                {
                    continue;
                }
                const float bw = static_cast<float>((*bar)[0].xy[2] - (*bar)[0].xy[0]);
                const float sx = bw > 0 ? (w - 4) / bw : 1.0f;
                p.sprite(sub.sheet, sub.shape, { (x + static_cast<float>(row->x)) * c.scale, (y + static_cast<float>(row->y)) * c.scale }, {}, 0, sx);
            }
        }
    }
    const float room = w - 8;
    for (int r = at_.top; r < std::min(n, at_.top + kCandidateRows); ++r)
    {
        const float ry   = y + 18 + 16 * static_cast<float>(r - at_.top);
        std::string text = at_.rows[static_cast<std::size_t>(r)].text;
        std::string note;
        if (const auto cut = text.find('\x06'); cut != std::string::npos)
        {
            note = text.substr(cut + 1);
            text.resize(cut);
        }
        if (p.textWidth(text) / c.scale > room)
        {
            while (!text.empty() && p.textWidth(text + "...") / c.scale > room - 7)
            {
                text.pop_back();
            }
            while (!text.empty() && text.back() == ' ')
            {
                text.pop_back();
            }
            text += "...";
        }
        const float tx = p.text(text, { (x + 4) * c.scale, ry * c.scale }, kText);
        if (!note.empty())
        {
            p.text(note, { tx, ry * c.scale }, kNote);
        }
    }
}

} // namespace xi::ui
