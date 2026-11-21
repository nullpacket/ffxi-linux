// SPDX-License-Identifier: GPL-3.0-or-later
// Macros: the game's macro books and its files, the runner, the palette and the Macros windows (ui-format-notes §7.9).
#include "ui/game_ui.h"

#include "ui/layout_view.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace xi::ui
{

namespace
{

namespace mf = dat::macro;

constexpr UiColor kWhite{ 0x80, 0x80, 0x80, 0x80 };
constexpr UiColor kGrey{ 0x50, 0x50, 0x50, 0x80 };
constexpr UiColor kGold{ 0x80, 0x78, 0x50, 0x80 };

std::string lowered(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

std::optional<std::vector<std::byte>> readFile(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

// Written beside the target and renamed over it, so a crash never leaves half a file.
void writeFile(const std::filesystem::path& file, const std::vector<std::byte>& bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto tmp = std::filesystem::path(file.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            spdlog::warn("macros: cannot write {}", tmp.string());
            return;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec)
    {
        spdlog::warn("macros: cannot write {}: {}", file.string(), ec.message());
    }
}

// The game's printf-style strings ("Macro Set %s (#%d)"): %s and %d filled in order.
std::string formatGame(std::string_view f, const std::vector<std::string>& args)
{
    std::string out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < f.size(); ++i)
    {
        if (f[i] == '%' && i + 1 < f.size() && (f[i + 1] == 's' || f[i + 1] == 'd' || f[i + 1] == 'u'))
        {
            out += next < args.size() ? args[next++] : std::string();
            ++i;
            continue;
        }
        out += f[i];
    }
    return out;
}

// A line cut to the file's 60 bytes without splitting an auto-translate phrase (FD <4 bytes> FD) or a Shift-JIS pair.
std::string cutLine(std::string s, std::size_t max)
{
    if (s.size() <= max)
    {
        return s;
    }
    std::size_t i = 0;
    while (i < s.size())
    {
        const auto        b    = static_cast<unsigned char>(s[i]);
        const std::size_t step = b == 0xFD ? 6 : (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC) ? 2 : 1;
        if (i + step > max)
        {
            break;
        }
        i += step;
    }
    s.resize(i);
    return s;
}

} // namespace

// --- the model and its files ---

GameUi::MacroSet& GameUi::macroSetAt(int book, int set) const
{
    book                = std::clamp(book, 1, mf::kBooks);
    set                 = std::clamp(set, 1, mf::kSets);
    auto [it, fresh]    = macroBooks_.try_emplace({ book, set });
    if (fresh && !macroDir_.empty())
    {
        const auto file = macroDir_ / mf::setFileName(book - 1, set - 1);
        if (const auto bytes = readFile(file))
        {
            if (const auto s = mf::parseSet(*bytes))
            {
                for (std::size_t i = 0; i < s->macros.size(); ++i)
                {
                    it->second[i / 10][i % 10] = s->macros[i];
                }
            }
            else
            {
                // The game resets such a set (d_msg 55646 #200 "Macro Book %u Set %u contains invalid data and has been
                // reset."); here it starts empty and the file is left alone until the set is changed.
                spdlog::warn("macros: Macro Book {} Set {} contains invalid data and has been reset ({})", book, set, file.string());
            }
        }
    }
    return it->second;
}

void GameUi::saveMacroSet(int book, int set)
{
    if (macroDir_.empty())
    {
        return;
    }
    const auto& rows = macroSetAt(book, set);
    mf::Set     s;
    for (std::size_t i = 0; i < s.macros.size(); ++i)
    {
        s.macros[i] = rows[i / 10][i % 10];
    }
    writeFile(macroDir_ / mf::setFileName(book - 1, set - 1), mf::writeSet(s));
}

void GameUi::saveMacroTitles()
{
    if (macroDir_.empty())
    {
        return;
    }
    std::array<std::string, 20> a{}, b{};
    for (std::size_t i = 0; i < 20; ++i)
    {
        a[i] = macroBookNames_[i];
        b[i] = macroBookNames_[i + 20];
    }
    writeFile(macroDir_ / "mcr.ttl", mf::writeTitles(a));
    writeFile(macroDir_ / "mcr_2.ttl", mf::writeTitles(b));
}

void GameUi::saveMacroPosition()
{
    if (!macroDir_.empty())
    {
        // mcr.sys: book * 20 + set * 2 (CODE 0x101a2440; the manager's +0x1dbc).
        writeFile(macroDir_ / "mcr.sys", mf::writeSys(static_cast<uint32_t>((macroBook_ - 1) * 20 + (macroSet_ - 1) * 2)));
    }
}

std::string GameUi::macroShown(std::string_view raw) const
{
    return text_ ? text_->expandAutoTranslate(raw) : std::string(raw);
}

std::string GameUi::macroEncoded(std::string_view shown) const
{
    return text_ ? text_->encodeAutoTranslate(shown) : std::string(shown);
}

void GameUi::loadMacroDir(const std::filesystem::path& dir)
{
    macroDir_ = dir;
    macroBooks_.clear();
    macroRun_.next = -1;
    // Book names: mcr.ttl (1..20) and mcr_2.ttl (21..40); "Book01".. when missing (INFERRED: the names the game wrote
    // into the bots' fresh files).
    for (std::size_t i = 0; i < macroBookNames_.size(); ++i)
    {
        macroBookNames_[i] = fmt::format("Book{:02}", i + 1);
    }
    for (int half = 0; half < 2; ++half)
    {
        if (const auto bytes = readFile(dir / (half == 0 ? "mcr.ttl" : "mcr_2.ttl")))
        {
            if (const auto names = mf::parseTitles(*bytes))
            {
                for (std::size_t i = 0; i < 20; ++i)
                {
                    macroBookNames_[static_cast<std::size_t>(half) * 20 + i] = (*names)[i];
                }
            }
        }
    }
    macroBook_ = 1;
    macroSet_  = 1;
    if (const auto bytes = readFile(dir / "mcr.sys"))
    {
        if (const auto v = mf::parseSys(*bytes))
        {
            macroBook_ = std::clamp(static_cast<int>(*v / 20) + 1, 1, mf::kBooks);
            macroSet_  = std::clamp(static_cast<int>(*v % 20 / 2) + 1, 1, mf::kSets);
        }
    }
    spdlog::info("macros: {} (book {}, set {})", dir.string(), macroBook_, macroSet_);
}

void GameUi::loadMacros(const std::filesystem::path& file)
{
    // Our older text format, one block per macro: "[book B set S ctrl|alt K] Name" (book / set optional, default 1 1)
    // then up to six lines. Read into the books and written out as the game's files.
    std::ifstream in(file);
    if (!in)
    {
        return;
    }
    std::string                    line;
    Macro*                         cur = nullptr;
    std::set<std::pair<int, int>>  touched;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        {
            line.pop_back();
        }
        if (line.empty() || line[0] == ';' || line[0] == '#')
        {
            continue;
        }
        if (line[0] == '[')
        {
            const auto close = line.find(']');
            auto       head  = lowered(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            int        book = 1, set = 1;
            if (head.starts_with("book "))
            {
                std::istringstream hs(head);
                std::string        w1, w2;
                hs >> w1 >> book >> w2 >> set;
                std::string rest;
                std::getline(hs, rest);
                const auto first = rest.find_first_not_of(' ');
                head             = first == std::string::npos ? std::string() : rest.substr(first);
            }
            const int  row = head.starts_with("alt") ? 1 : head.starts_with("ctrl") ? 0 : -1;
            const auto sp  = head.find(' ');
            const int  key = sp == std::string::npos ? -1 : std::atoi(head.c_str() + sp + 1);
            cur            = nullptr;
            if (row >= 0 && key >= 0 && key <= 10)
            {
                book = std::clamp(book, 1, mf::kBooks);
                set  = std::clamp(set, 1, mf::kSets);
                touched.insert({ book, set });
                cur       = &macroSetAt(book, set)[static_cast<std::size_t>(row)][static_cast<std::size_t>(key == 0 ? 9 : key - 1)];
                *cur      = Macro{};
                auto name = close == std::string::npos ? std::string() : line.substr(close + 1);
                name.erase(0, std::min(name.find_first_not_of(' '), name.size()));
                cur->name = cutLine(macroEncoded(name), mf::kNameMax);
            }
            continue;
        }
        if (cur)
        {
            if (const auto it = std::find(cur->lines.begin(), cur->lines.end(), std::string()); it != cur->lines.end())
            {
                *it = cutLine(macroEncoded(line), mf::kLineMax);
            }
        }
    }
    for (const auto& [book, set] : touched)
    {
        saveMacroSet(book, set);
    }
    spdlog::info("macros: {} sets taken from {}", touched.size(), file.string());
}

std::string GameUi::macroName(int row, int key) const
{
    if (row < 0 || row > 1 || key < 1 || key > 10)
    {
        return {};
    }
    return macroShown(macros()[static_cast<std::size_t>(row)][static_cast<std::size_t>(key - 1)].name);
}

std::string GameUi::macroLine(int row, int key, int line) const
{
    if (row < 0 || row > 1 || key < 1 || key > 10 || line < 0 || line >= mf::kLines)
    {
        return {};
    }
    return macroShown(macros()[static_cast<std::size_t>(row)][static_cast<std::size_t>(key - 1)].lines[static_cast<std::size_t>(line)]);
}

void GameUi::setMacro(int row, int key, std::string name, std::vector<std::string> lines)
{
    if (row < 0 || row > 1 || key < 1 || key > 10)
    {
        return;
    }
    auto& m = macroSetAt(macroBook_, macroSet_)[static_cast<std::size_t>(row)][static_cast<std::size_t>(key - 1)];
    m       = Macro{};
    m.name  = cutLine(macroEncoded(name), mf::kNameMax);
    for (std::size_t i = 0; i < lines.size() && i < m.lines.size(); ++i)
    {
        m.lines[i] = cutLine(macroEncoded(lines[i]), mf::kLineMax);
    }
    saveMacroSet(macroBook_, macroSet_);
}

bool GameUi::setMacroBook(int book)
{
    if (book < 1 || book > mf::kBooks)
    {
        return false;
    }
    macroBook_ = book;
    saveMacroPosition();
    return true;
}

bool GameUi::setMacroSet(int set)
{
    if (set < 1 || set > mf::kSets)
    {
        return false;
    }
    macroSet_ = set;
    saveMacroPosition();
    return true;
}

// --- running ---

void GameUi::runMacro(int set, int key)
{
    if (set < 0 || set > 1 || key < 1 || key > 10)
    {
        return;
    }
    // CODE 0x101a1810 -> 0x101a12f0: the macro's lines are copied and the runner starts at line 0, so a macro started
    // while another runs replaces it.
    const auto& m = macros()[static_cast<std::size_t>(set)][static_cast<std::size_t>(key - 1)];
    for (std::size_t i = 0; i < m.lines.size(); ++i)
    {
        macroRun_.lines[i] = m.lines[i]; // tokens stay FD codes: chat sends them, commands read their text (submitLine)
    }
    macroRun_.next   = 0;
    macroRun_.resume = {};
}

void GameUi::stepMacro(const GameUiData& d, const GameUiActions& act)
{
    (void)act;
    auto&      r   = macroRun_;
    const auto now = d.now;
    // The wait clock is held while the chat line is open (CODE 0x101a1870 skips while DAT_1057876c +0x59 / +0xc4 are
    // set; INFERRED that those are the chat line).
    if (r.next >= 0 && typing_ && r.last.time_since_epoch().count() != 0 && r.resume > now)
    {
        r.resume += now - r.last;
    }
    r.last = now;
    // One line a frame (CODE 0x101a1870), after the line before it has gone (sub-target cursors included); the rules
    // are dat::macro::stepFor.
    while (r.next >= 0 && now >= r.resume && macroQueue_.empty() && !sub_.active)
    {
        const auto step = r.next < mf::kLines ? mf::stepFor(r.lines[static_cast<std::size_t>(r.next)]) : mf::LineStep{};
        ++r.next;
        if (step.kind == mf::LineStep::Kind::End)
        {
            r.next = -1;
            break;
        }
        if (step.kind == mf::LineStep::Kind::Skip)
        {
            continue;
        }
        spdlog::debug("macro: line {} \"{}\", then wait {} s", r.next, step.text, step.wait);
        if (!step.text.empty())
        {
            macroQueue_.push_back(step.text); // run as typed, mode 2 (macro) by the queue
        }
        r.resume = now + std::chrono::seconds(step.wait);
        break;
    }
}

// --- the palette ---

void GameUi::drawMacroPalette(UiPainter& p, const UiCanvas& c, int set)
{
    // Config Misc. 2 (config_ui2_re.md §3): macro palette Full (157 = 0, the "long" 862-wide layouts; forced compact
    // under a 1024-wide UI) or Compact ("pall", 480); placement Dynamic (158 = 0: shifted 130 right while the target
    // window shows; INFERRED which windows) or Static (always shifted; never under a 640-wide UI).
    const GameUiData* d     = data_;
    const bool        full  = (!d || cfgValue(*d, 157) == 0) && c.width >= 1024;
    const bool        shift = c.width >= 640 && d && (cfgValue(*d, 158) == 1 || d->target != nullptr);
    const auto&       ls    = r_.resources().layouts;
    const auto it = ls.find(set == 0 ? (full ? "menu mcr1long" : "menu mcr1pall") : (full ? "menu mcr2long" : "menu mcr2pall"));
    if (it == ls.end())
    {
        return;
    }
    dat::UiLayout l = it->second;
    if (shift)
    {
        l.window.x = static_cast<int16_t>(l.window.x + 130);
    }
    const ImVec2 o = drawLayout(p, c, l, {});
    drawMacroTitle(p, c); // string table 10 #39 "Macro Set %s (#%d)" with the book's name and the set (CODE 0x101a38b0)
    // Confirm on a button (here a click) runs it (CODE 0x101a3510).
    if (paletteClick_)
    {
        paletteClick_ = false;
        if (const int id = hitTest(c, l, mouse_); id >= 1 && id <= 10)
        {
            runMacro(set, id);
        }
    }
    for (const auto& e : l.elements)
    {
        const auto& m = macros()[static_cast<std::size_t>(set)][static_cast<std::size_t>(std::clamp<int>(e.id, 1, 10) - 1)];
        if (m.name.empty())
        {
            continue;
        }
        // The name above the key label ("Ctrl1" sits at the bottom of the box), centred and cut to fit.
        std::string n    = macroShown(m.name);
        const auto  boxW = static_cast<float>(e.w);
        while (n.size() > 1 && p.textShpWidth(n) > boxW - 2)
        {
            n.pop_back();
        }
        const float x = static_cast<float>(e.x) + (boxW - p.textShpWidth(n)) * 0.5f;
        p.textShp(n, ImVec2(o.x + x * c.scale, o.y + (static_cast<float>(e.y) + 4) * c.scale), kWhite);
    }
}

// --- the Macros windows ---

bool GameUi::isMacroKind(Kind k)
{
    return k == Kind::MacroMenu || k == Kind::MacroBooks || k == Kind::MacroBookOp || k == Kind::MacroEdit || k == Kind::MacroButtonOp ||
           k == Kind::MacroEditor || k == Kind::MacroLineOp || k == Kind::MacroConfirm || k == Kind::EquipSets || k == Kind::EquipSetOp ||
           k == Kind::EquipSetEdit;
}

const char* GameUi::macroLayoutName(const Menu& m) const
{
    // The edit palettes follow the palette's Full / Compact choice (cfg 157; "mcr1edlo" / "mcr2edlo" are the long ones).
    const bool full = data_ && cfgValue(*data_, 157) == 0 && lastCanvas_.width >= 1024;
    switch (m.kind)
    {
        case Kind::MacroMenu: return "menu mcrmenu";
        case Kind::MacroBooks: return "menu mcrselec";
        case Kind::MacroBookOp: return "menu mcrselop";
        case Kind::MacroEdit: return m.param == 0 ? (full ? "menu mcr1edlo" : "menu mcr1edit") : (full ? "menu mcr2edlo" : "menu mcr2edit");
        case Kind::MacroButtonOp: return "menu mcrbedit";
        case Kind::MacroEditor: return "menu macro";
        case Kind::MacroLineOp: return "menu mcrledit";
        case Kind::MacroConfirm: return "menu comyn";
        case Kind::EquipSets: return "menu mcres20";
        case Kind::EquipSetOp: return "menu mcresmn";
        case Kind::EquipSetEdit: return "menu mcresed";
        default: return "";
    }
}

const dat::UiLayout* GameUi::macroLayout(const Menu& m) const
{
    const auto& ls = r_.resources().layouts;
    const auto  it = ls.find(macroLayoutName(m));
    if (it == ls.end())
    {
        return nullptr;
    }
    if (m.kind == Kind::EquipSetOp)
    {
        // As the book options: authored at 0,0, put left of the set list's chosen row (INFERRED from "menu mcrselop").
        equipSetOpLayout_ = it->second;
        const auto list   = std::find_if(stack_.rbegin(), stack_.rend(), [](const Menu& s) { return s.kind == Kind::EquipSets; });
        const auto bl     = ls.find("menu mcres20");
        if (list != stack_.rend() && bl != ls.end())
        {
            const auto& lw = bl->second.window;
            if (const auto* row = bl->second.element(static_cast<uint16_t>(list->cursor)))
            {
                auto& w  = equipSetOpLayout_.window;
                w.anchor = lw.anchor;
                w.x      = static_cast<int16_t>(lw.x + row->x - w.w);
                w.y      = static_cast<int16_t>(std::min<int>(lw.y + row->y, lw.y + lw.h - w.h));
            }
        }
        return &equipSetOpLayout_;
    }
    if (m.kind != Kind::MacroBookOp)
    {
        return &it->second;
    }
    // "menu mcrselop" is authored at 0,0: CODE 0x101a65d0 puts it left of the book list's selected row (its x minus
    // the window's width) at the row's y, kept above the list's bottom edge.
    macroOpLayout_ = it->second;
    const auto list = std::find_if(stack_.rbegin(), stack_.rend(), [](const Menu& s) { return s.kind == Kind::MacroBooks; });
    const auto bl   = ls.find("menu mcrselec");
    if (list != stack_.rend() && bl != ls.end())
    {
        const auto& lw = bl->second.window;
        if (const auto* row = bl->second.element(static_cast<uint16_t>(list->cursor)))
        {
            auto& w  = macroOpLayout_.window;
            w.anchor = lw.anchor;
            w.x      = static_cast<int16_t>(lw.x + row->x - w.w);
            w.y      = static_cast<int16_t>(std::min<int>(lw.y + row->y, lw.y + lw.h - w.h));
        }
    }
    return &macroOpLayout_;
}

bool GameUi::macroImplemented(const Menu& m, const dat::UiRecord& e) const
{
    switch (m.kind)
    {
        case Kind::MacroMenu: return e.id == 1 || e.id == 2;
        case Kind::EquipSetOp:
        {
            // 4 Paste needs a copy; Edit / Name always; Copy, Delete, Equip, Lock style need a set there.
            const bool there = equipSets_.contains(equipSetOp_);
            return e.id == 4 ? equipSetClip_.has_value() : (e.id == 3 || e.id == 5 || e.id == 7 || e.id == 8) ? there : true;
        }
        case Kind::MacroBookOp: return e.id != 4 || macroBookClip_.has_value();
        case Kind::MacroButtonOp: return e.id != 2 || (m.param == 1 ? macroClip_.has_value() : macroSetClip_.has_value());
        case Kind::MacroLineOp: return e.id != 2 || macroLineClip_.has_value();
        default: return true;
    }
}

void GameUi::macroEditField(std::string initial, std::function<void(std::string)> done)
{
    // The game's input line for a field (CODE 0x101c64d0; event 17 hands the text back, 0x101c5a00).
    typing_     = true;
    input_      = std::move(initial);
    inputDone_  = std::move(done);
    cursor_     = input_.size();
}

void GameUi::macroConfirm(Menu& m, const dat::UiRecord& e)
{
    auto& rows = macroSetAt(macroBook_, macroSet_);
    switch (m.kind)
    {
        case Kind::MacroMenu:
            if (e.id == 1)
            {
                open(Kind::MacroBooks, (macroBook_ - 1) / 20);
                stack_.back().cursor = (macroBook_ - 1) % 20 + 1;
            }
            else if (e.id == 2)
            {
                open(Kind::EquipSets, 0); // Edit Equip. Set (help 830)
            }
            break;
        case Kind::EquipSets:
            if (e.id >= 1 && e.id <= 20)
            {
                equipSetOp_ = m.param * 20 + e.id;
                open(Kind::EquipSetOp);
            }
            else if (e.id == 21 || e.id == 22)
            {
                m.param = (m.param + (e.id == 22 ? 1 : 4)) % 5; // the page arrows (5 pages of 20)
            }
            break;
        case Kind::EquipSetOp:
        {
            // Help 831..837, 921: Edit, Name, Copy, Paste, Delete, Save what is worn, Equip (/equipset N), Lock style.
            const int n = equipSetOp_;
            if (e.id == 1)
            {
                stack_.pop_back();
                equipSetEditing_ = n;
                open(Kind::EquipSetEdit, n);
            }
            else if (e.id == 2)
            {
                macroEditField(equipSets_.contains(n) ? equipSets_[n].name : std::string(), [this, n](const std::string& name) {
                    equipSets_[n].name = cutLine(name, 15);
                    saveEquipSets();
                });
            }
            else if (e.id == 3)
            {
                equipSetClip_ = equipSets_[n];
                stack_.pop_back();
            }
            else if (e.id == 4 && equipSetClip_)
            {
                equipSets_[n] = *equipSetClip_;
                saveEquipSets();
                stack_.pop_back();
            }
            else if (e.id == 5)
            {
                equipSets_.erase(n);
                saveEquipSets();
                stack_.pop_back();
            }
            else if (e.id == 6 || e.id == 7 || e.id == 8)
            {
                macroQueue_.push_back(e.id == 6 ? fmt::format("/equipset save {}", n) : fmt::format("{} {}", e.id == 7 ? "/equipset" : "/lockstyleset", n));
                stack_.pop_back();
            }
            break;
        }
        case Kind::EquipSetEdit:
            // A slot box (framesus 40..55 as the Equipment grid): its list, choosing stores the item in the set.
            if (const auto* sub = e.subFor(0); sub && sub->shape >= 40 && sub->shape < 56)
            {
                static constexpr uint8_t kCellSlot[16] = { 0, 1, 2, 3, 4, 9, 11, 12, 5, 6, 13, 14, 15, 10, 7, 8 }; // as "menu equip"
                open(Kind::EquipList, kCellSlot[sub->shape - 40]);
            }
            break;
        case Kind::MacroBooks:
            if (e.id >= 1 && e.id <= 20)
            {
                macroOpBook_ = m.param * 20 + e.id - 1;
                open(Kind::MacroBookOp);
            }
            else if (e.id == 21 || e.id == 22)
            {
                m.param = 1 - m.param; // the page arrows (2 pages of 20)
            }
            break;
        case Kind::MacroBookOp:
        {
            const int book = macroOpBook_ + 1;
            if (e.id == 1)
            {
                // Edit: the book becomes the palette's (0x101a24d0) and its palettes open (0x101a4080).
                setMacroBook(book);
                stack_.pop_back(); // the options close (INFERRED); the book list stays under the palettes
                open(Kind::MacroEdit, 0);
            }
            else if (e.id == 2)
            {
                macroEditField(macroBookNames_[static_cast<std::size_t>(macroOpBook_)], [this, b = macroOpBook_](std::string name) {
                    macroBookNames_[static_cast<std::size_t>(b)] = cutLine(name, 15);
                    saveMacroTitles();
                });
            }
            else if (e.id == 3)
            {
                std::array<MacroSet, 10> all;
                for (int s = 1; s <= mf::kSets; ++s)
                {
                    all[static_cast<std::size_t>(s - 1)] = macroSetAt(book, s);
                }
                macroBookClip_ = all;
                stack_.pop_back();
            }
            else if (e.id == 4 || e.id == 5)
            {
                open(Kind::MacroConfirm, e.id == 4 ? 0 : 1); // 55648 #239 / #240
                stack_.back().cursor = yesNoCursor(kMacroConfirmOnYes); // opens on No (CODE 0x101a6787)
            }
            break;
        }
        case Kind::MacroConfirm:
        {
            if (m.cursor == 1)
            {
                const int book = macroOpBook_ + 1;
                if (m.param == 0 || m.param == 1)
                {
                    for (int s = 1; s <= mf::kSets; ++s)
                    {
                        macroSetAt(book, s) = m.param == 0 && macroBookClip_ ? (*macroBookClip_)[static_cast<std::size_t>(s - 1)] : MacroSet{};
                        saveMacroSet(book, s);
                    }
                }
                else
                {
                    rows = m.param == 2 && macroSetClip_ ? *macroSetClip_ : MacroSet{};
                    saveMacroSet(macroBook_, macroSet_);
                }
            }
            stack_.pop_back(); // the question
            stack_.pop_back(); // and the window that asked it
            break;
        }
        case Kind::MacroEdit:
            // A key: "menu macro" for it (CODE 0x101a4170).
            macroEditRow_ = m.param;
            macroEditKey_ = std::clamp(m.cursor, 1, 10);
            open(Kind::MacroEditor);
            stack_.back().cursor = 3;
            break;
        case Kind::MacroButtonOp:
        {
            auto& mac = rows[static_cast<std::size_t>(macroEditRow_)][static_cast<std::size_t>(macroEditKey_ - 1)];
            if (m.param == 1)
            {
                // The button: Copy / Paste / Delete macro (help 659 / 318 / 317).
                if (e.id == 1)
                {
                    macroClip_ = mac;
                }
                else if (e.id == 2 && macroClip_)
                {
                    mac = *macroClip_;
                    saveMacroSet(macroBook_, macroSet_);
                }
                else if (e.id == 3)
                {
                    mac = Macro{};
                    saveMacroSet(macroBook_, macroSet_);
                }
                stack_.pop_back();
            }
            else if (e.id == 1)
            {
                macroSetClip_ = rows; // the set: Copy / Paste / Delete macro set (help 668..670)
                stack_.pop_back();
            }
            else
            {
                open(Kind::MacroConfirm, e.id == 2 ? 2 : 3); // 55648 #237 / #238
                stack_.back().cursor = yesNoCursor(kMacroConfirmOnYes); // opens on No (#238: CODE 0x101a5da0)
            }
            break;
        }
        case Kind::MacroEditor:
        {
            auto& mac = rows[static_cast<std::size_t>(macroEditRow_)][static_cast<std::size_t>(macroEditKey_ - 1)];
            const int book = macroBook_, set = macroSet_, row = macroEditRow_, key = macroEditKey_;
            if (e.id == 1 || e.id == 2)
            {
                // Prev / Next: the macro before / after, Ctrl 1..0 then Alt 1..0, wrapping (INFERRED order).
                const int i   = (macroEditRow_ * 10 + macroEditKey_ - 1 + (e.id == 1 ? 19 : 1)) % 20;
                macroEditRow_ = i / 10;
                macroEditKey_ = i % 10 + 1;
            }
            else if (e.id == 3)
            {
                // The name: up to 8 characters (CODE 0x101a14f0).
                macroEditField(mac.name, [this, book, set, row, key](std::string text) {
                    macroSetAt(book, set)[static_cast<std::size_t>(row)][static_cast<std::size_t>(key - 1)].name = cutLine(macroEncoded(text), mf::kNameMax);
                    saveMacroSet(book, set);
                });
            }
            else if (e.id >= 4 && e.id <= 9)
            {
                // A line: up to 60 bytes (CODE 0x101a1450).
                const auto li = static_cast<std::size_t>(e.id - 4);
                macroEditField(mac.lines[li], [this, book, set, row, key, li](std::string text) {
                    macroSetAt(book, set)[static_cast<std::size_t>(row)][static_cast<std::size_t>(key - 1)].lines[li] = cutLine(macroEncoded(text), mf::kLineMax);
                    saveMacroSet(book, set);
                });
            }
            break;
        }
        case Kind::MacroLineOp:
        {
            auto&      mac = rows[static_cast<std::size_t>(macroEditRow_)][static_cast<std::size_t>(macroEditKey_ - 1)];
            const auto li  = static_cast<std::size_t>(std::clamp(m.param, 0, mf::kLines - 1));
            if (e.id == 1)
            {
                macroLineClip_ = mac.lines[li]; // Copy / Paste / Delete line (help 660 / 320 / 319)
            }
            else if (e.id == 2 && macroLineClip_)
            {
                mac.lines[li] = *macroLineClip_;
                saveMacroSet(macroBook_, macroSet_);
            }
            else if (e.id == 3)
            {
                mac.lines[li].clear();
                saveMacroSet(macroBook_, macroSet_);
            }
            stack_.pop_back();
            break;
        }
        default: break;
    }
}

bool GameUi::macroApply(Menu& m, Cmd c)
{
    if (m.kind == Kind::MacroBooks && (c == Cmd::Left || c == Cmd::Right) && m.cursor <= 20)
    {
        m.param = 1 - m.param; // the other page of 20 books
        return true;
    }
    if (m.kind == Kind::MacroEdit && (c == Cmd::Up || c == Cmd::Down))
    {
        // Down walks Ctrl(s) -> Alt(s) -> Ctrl(s + 1), Up back (CODE 0x101a4500 / 0x101a4bc0), the set wrapping 1..10.
        if (c == Cmd::Down)
        {
            if (m.param == 0)
            {
                m.param = 1;
            }
            else
            {
                m.param = 0;
                setMacroSet(macroSet_ % 10 + 1);
            }
        }
        else if (m.param == 1)
        {
            m.param = 0;
        }
        else
        {
            m.param = 1;
            setMacroSet((macroSet_ + 8) % 10 + 1);
        }
        return true;
    }
    return false;
}

void GameUi::drawMacroWindow(UiPainter& p, const UiCanvas& c, const Menu& m, ImVec2 o)
{
    const auto* l = layout(m);
    if (!l)
    {
        return;
    }
    auto at = [&](float x, float y) { return ImVec2(o.x + x * c.scale, o.y + y * c.scale); };
    if (m.kind == Kind::MacroBooks)
    {
        // Book names on the 20 rows of this page; the palette's book in gold.
        for (const auto& e : l->elements)
        {
            if (e.id >= 1 && e.id <= 20)
            {
                const int book = m.param * 20 + e.id;
                p.text(macroBookNames_[static_cast<std::size_t>(book - 1)], at(static_cast<float>(e.x) + 4, static_cast<float>(e.y) - 2),
                       book == macroBook_ ? kGold : kWhite);
            }
        }
    }
    else if (m.kind == Kind::EquipSets)
    {
        // Each row: the set's number and name ("---" when empty; INFERRED, the game's row text was not traced).
        for (const auto& e : l->elements)
        {
            if (e.id >= 1 && e.id <= 20)
            {
                const int  n   = m.param * 20 + e.id;
                const auto it  = equipSets_.find(n);
                std::string txt = fmt::format("{:3} {}", n, it == equipSets_.end() ? std::string("---") : it->second.name.empty() ? std::string("Set") : it->second.name);
                while (txt.size() > 4 && p.textWidth(txt) / c.scale > static_cast<float>(e.w) - 6)
                {
                    txt.pop_back(); // cut to the row
                }
                p.text(txt, at(static_cast<float>(e.x) + 4, static_cast<float>(e.y) - 2), it == equipSets_.end() ? kGrey : kWhite);
            }
        }
    }
    else if (m.kind == Kind::EquipSetEdit && data_ && data_->items)
    {
        // The set's items in its slot boxes (icons of the items held by that name).
        static constexpr uint8_t kCellSlot[16] = { 0, 1, 2, 3, 4, 9, 11, 12, 5, 6, 13, 14, 15, 10, 7, 8 };
        const auto set = equipSets_.find(m.param);
        for (const auto& e : l->elements)
        {
            const auto* sub = e.subFor(0);
            if (set == equipSets_.end() || !sub || sub->shape < 40 || sub->shape >= 56)
            {
                continue;
            }
            const uint8_t slot = kCellSlot[sub->shape - 40];
            for (const auto& [s, name] : set->second.items)
            {
                if (s != slot)
                {
                    continue;
                }
                for (const auto& it : data_->world->inventory)
                {
                    const auto info = data_->items->find(it.itemId);
                    if (info && lowered(info->name) == name)
                    {
                        if (const auto* icon = itemIcon(*data_, it.itemId))
                        {
                            p.icon(fmt::format("item {}", it.itemId), *icon, at(e.x, e.y));
                        }
                        break;
                    }
                }
            }
        }
    }
    else if (m.kind == Kind::MacroEdit)
    {
        const auto& row = macros()[static_cast<std::size_t>(m.param)];
        for (const auto& e : l->elements)
        {
            if (e.id < 1 || e.id > 10 || row[static_cast<std::size_t>(e.id - 1)].name.empty())
            {
                continue;
            }
            std::string n    = macroShown(row[static_cast<std::size_t>(e.id - 1)].name);
            const auto  boxW = static_cast<float>(e.w);
            while (n.size() > 1 && p.textShpWidth(n) > boxW - 2)
            {
                n.pop_back();
            }
            p.textShp(n, at(static_cast<float>(e.x) + (boxW - p.textShpWidth(n)) * 0.5f, static_cast<float>(e.y) + 4), kWhite);
        }
    }
    else if (m.kind == Kind::MacroEditor)
    {
        // The key ("Ctrl1".."Alt0") over its label, the name in its field, the six lines in theirs (fields drawn by the
        // layout; a line's box starts at x + frame x).
        const auto& mac = macros()[static_cast<std::size_t>(macroEditRow_)][static_cast<std::size_t>(macroEditKey_ - 1)];
        // Elements 1 / 2 are the Prev / Next buttons; the key's label goes in the box left of the name (INFERRED).
        if (const auto* e = l->element(3))
        {
            p.text(fmt::format("{}{}", macroEditRow_ == 0 ? "Ctrl" : "Alt", macroEditKey_ % 10), at(24, static_cast<float>(e->y) + 4), kGold);
        }
        if (const auto* e = l->element(3))
        {
            p.text(macroShown(mac.name), at(static_cast<float>(e->x + e->frameX) + 8, static_cast<float>(e->y) + 4), kWhite);
        }
        for (uint16_t id = 4; id <= 9; ++id)
        {
            if (const auto* e = l->element(id))
            {
                p.text(macroShown(mac.lines[static_cast<std::size_t>(id - 4)]), at(static_cast<float>(e->x + e->frameX) + 8, static_cast<float>(e->y) + 4), kWhite);
            }
        }
    }
}

std::string GameUi::macroTitle() const
{
    // String table 10 (file 50): #39 "Macro Set %s (#%d)" for the palettes (CODE 0x101a38b0 / 0x101a4390), #109 "Macro
    // Book (#%d)" for the book list.
    const auto* f = data_ ? data_->formats : nullptr;
    if (!stack_.empty() && stack_.back().kind == Kind::MacroBooks)
    {
        const auto& m    = stack_.back();
        const int   book = m.param * 20 + std::clamp(m.cursor, 1, 20);
        const auto  fmtS = f && !f->get(109).empty() ? f->get(109) : std::string_view("Macro Book (#%d)");
        return formatGame(fmtS, { std::to_string(book) });
    }
    const auto fmtS = f && !f->get(39).empty() ? f->get(39) : std::string_view("Macro Set %s (#%d)");
    return formatGame(fmtS, { macroBookNames_[static_cast<std::size_t>(macroBook_ - 1)], std::to_string(macroSet_) });
}

void GameUi::drawMacroTitle(UiPainter& p, const UiCanvas& c)
{
    const auto& ls = r_.resources().layouts;
    const auto  tw = ls.find("menu titlewin");
    if (tw == ls.end())
    {
        return;
    }
    const ImVec2      to    = drawLayout(p, c, tw->second, {});
    const std::string title = macroTitle();
    // The title window is 112 wide: a longer title ("Macro Set Book01 (#1)") is drawn in the narrower outlined font so
    // it stays inside (our menu font runs wider than the game's; INFERRED).
    if (p.textWidth(title) / c.scale > static_cast<float>(tw->second.window.w) - 8)
    {
        p.textShp(title, ImVec2(to.x + 4 * c.scale, to.y + 10 * c.scale), kWhite);
    }
    else
    {
        p.text(title, ImVec2(to.x + 4 * c.scale, to.y + 8 * c.scale), kWhite);
    }
}

std::string GameUi::macroHelp(const Menu& m, const GameUiData& d) const
{
    switch (m.kind)
    {
        case Kind::MacroBooks:
            if (m.cursor >= 1 && m.cursor <= 20)
            {
                return macroBookNames_[static_cast<std::size_t>(m.param * 20 + m.cursor - 1)]; // the hovered book's name (0x101a60c0)
            }
            return {};
        case Kind::MacroEdit:
        {
            // A blank help line, so the help and title windows stay up with the title (INFERRED).
            return " ";
        }
        case Kind::MacroEditor: return " ";
        case Kind::MacroButtonOp:
            // Set mode: "Copy / Paste / Delete macro set." (55651 #668..670).
            if (m.param == 0 && d.help && m.cursor >= 1 && m.cursor <= 3)
            {
                return std::string(d.help->get(static_cast<std::size_t>(667 + m.cursor)));
            }
            return {};
        case Kind::MacroConfirm:
        {
            // 55648 #239 paste book, #240 delete book, #237 paste set, #238 delete set.
            static constexpr int kText[] = { 239, 240, 237, 238 };
            const auto            id      = static_cast<std::size_t>(kText[std::clamp(m.param, 0, 3)]);
            return d.clientText ? std::string(d.clientText->get(id)) : std::string();
        }
        default: return {};
    }
}

} // namespace xi::ui
