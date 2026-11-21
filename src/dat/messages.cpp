// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/messages.h"

#include <algorithm>

#include "xi/bytes.h"

#include <fmt/format.h>

#include <cctype>
#include <cstring>
#include <stdexcept>

namespace xi::dat
{

namespace
{

uint32_t u32(std::span<const uint8_t> b, std::size_t off)
{
    if (off + 4 > b.size())
    {
        return 0;
    }
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

} // namespace

KeyItemTable KeyItemTable::parse(std::span<const std::byte> file)
{
    const auto   t = StringTable::parse(file);
    KeyItemTable out;
    for (std::size_t e = 1; e < t.size(); ++e)
    {
        const uint32_t id = t.number(e, 0);
        if (id == 0 || t.get(e, 4).empty())
        {
            continue;
        }
        std::string desc(t.get(e, 6));
        for (auto& ch : desc)
        {
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                ch = ' '; // line breaks: the help window shows one line
            }
        }
        out.items_[id] = { std::string(t.get(e, 4)), std::move(desc), t.number(e, 1) };
    }
    return out;
}

LogNameTable LogNameTable::parse(std::span<const std::byte> file)
{
    const auto   t = StringTable::parse(file);
    LogNameTable out;
    for (std::size_t e = 0; e < t.size(); ++e)
    {
        const std::string name(t.get(e, 1));
        if (name.empty() || std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; }))
        {
            continue;
        }
        std::string desc(t.get(e, 2));
        for (auto& ch : desc)
        {
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                ch = ' ';
            }
        }
        out.entries_[t.number(e, 0)] = { name, std::move(desc) };
    }
    return out;
}

XiStringTable XiStringTable::parse(std::span<const std::byte> file)
{
    XiStringTable out;
    if (file.size() < 0x38 || std::memcmp(file.data(), "XISTRING", 8) != 0)
    {
        return out;
    }
    uint32_t count = 0;
    std::memcpy(&count, file.data() + 0x24, 4);
    const std::size_t data = 0x38 + std::size_t{ count } * 12;
    if (data > file.size())
    {
        return out;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t off = 0, len = 0;
        std::memcpy(&off, file.data() + 0x38 + i * 12, 4);
        std::memcpy(&len, file.data() + 0x38 + i * 12 + 4, 4);
        if (data + off > file.size())
        {
            out.entries_.emplace_back();
            continue;
        }
        const auto* p = reinterpret_cast<const char*>(file.data() + data + off);
        out.entries_.emplace_back(p, strnlen(p, std::min<std::size_t>(len, file.size() - data - off)));
    }
    return out;
}

StringTable StringTable::parse(std::span<const std::byte> file)
{
    // Header: "d_msg\0\0\0", u16 @+0x0A = 1 when the body is bit-inverted, then @+0x14 file size, header size,
    // index size (0 = fixed-size entries), entry size, data size, entry count.
    if (file.size() < 0x40 || std::memcmp(file.data(), "d_msg", 5) != 0)
    {
        throw std::runtime_error("not a d_msg file");
    }
    const bool     inverted = load<uint16_t>(file, 0x0A) == 1;
    const uint32_t header   = load<uint32_t>(file, 0x18);
    const uint32_t indexSz  = load<uint32_t>(file, 0x1C);
    const uint32_t entrySz  = load<uint32_t>(file, 0x20);
    const uint32_t count    = load<uint32_t>(file, 0x28);
    if (header > file.size())
    {
        throw std::runtime_error("d_msg header past end");
    }
    std::vector<uint8_t> body(file.size() - header);
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        const auto b = static_cast<uint8_t>(file[header + i]);
        body[i]      = inverted ? static_cast<uint8_t>(~b) : b;
    }
    const std::span<const uint8_t> all(body);

    StringTable t;
    t.entries_.resize(count);
    t.numbers_.resize(count);
    for (uint32_t e = 0; e < count; ++e)
    {
        std::size_t begin = 0, length = 0;
        if (indexSz)
        {
            begin  = indexSz + u32(all, 8u * e);
            length = u32(all, 8u * e + 4);
        }
        else
        {
            begin  = std::size_t{ entrySz } * e;
            length = entrySz;
        }
        if (begin + length > all.size() || length < 4)
        {
            continue;
        }
        const auto     entry  = all.subspan(begin, length);
        const uint32_t fields = std::min<uint32_t>(u32(entry, 0), 16);
        auto&          out    = t.entries_[e];
        for (uint32_t f = 0; f < fields; ++f)
        {
            // Field: u32 offset, u32 type. Type 0 = string, stored 28 bytes after its offset.
            const uint32_t off  = u32(entry, 4 + 8 * f);
            const uint32_t type = u32(entry, 8 + 8 * f);
            std::string    s;
            if (type == 0 && off + 28 < entry.size())
            {
                const auto* p = reinterpret_cast<const char*>(entry.data() + off + 28);
                s.assign(p, strnlen(p, entry.size() - off - 28));
            }
            out.push_back(std::move(s));
            t.numbers_[e].push_back(type == 1 && off + 4 <= entry.size() ? u32(entry, off) : 0u); // type 1: u32 at the offset
        }
    }
    return t;
}

std::string_view StringTable::get(std::size_t index, std::size_t field) const
{
    if (index >= entries_.size() || field >= entries_[index].size())
    {
        return {};
    }
    return entries_[index][field];
}

DialogTable DialogTable::parse(std::span<const std::byte> file)
{
    // u32 size (| 0x10000000), then everything XOR 0x80: u32 offsets (relative to after the size word), the first of
    // which is also the table's byte length, then the messages back to back.
    if (file.size() < 8)
    {
        throw std::runtime_error("dialog file too small");
    }
    std::vector<uint8_t> x(file.size() - 4);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        x[i] = static_cast<uint8_t>(static_cast<uint8_t>(file[4 + i]) ^ 0x80);
    }
    const std::span<const uint8_t> all(x);
    const uint32_t                 n = u32(all, 0) / 4;
    if (n == 0 || n * 4 > x.size())
    {
        throw std::runtime_error("dialog offset table out of range");
    }
    DialogTable t;
    t.messages_.resize(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const std::size_t b = u32(all, 4 * i);
        const std::size_t e = i + 1 < n ? u32(all, 4 * (i + 1)) : x.size();
        if (b <= e && e <= x.size())
        {
            t.messages_[i].assign(reinterpret_cast<const char*>(x.data() + b), e - b);
        }
    }
    return t;
}

std::string skillName(uint32_t id)
{
    // Standard FFXI skill ids (combat 1-31, magic 32-45, crafts 48-57); the client reads these from a DAT too.
    static constexpr const char* kNames[] = {
        nullptr, "hand-to-hand", "dagger", "sword", "great sword", "axe", "great axe", "scythe", "polearm", "katana",
        "great katana", "club", "staff", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, "automaton melee", "automaton archery", "automaton magic", "archery", "marksmanship", "throwing", "guarding", "evasion",
        "shield", "parrying", "divine magic", "healing magic", "enhancing magic", "enfeebling magic", "elemental magic", "dark magic", "summoning magic", "ninjutsu",
        "singing", "string instrument", "wind instrument", "blue magic", "geomancy", "handbell", nullptr, nullptr, "fishing", "woodworking",
        "smithing", "goldsmithing", "clothcraft", "leathercraft", "bonecraft", "alchemy", "cooking", "synergy",
    };
    if (id < std::size(kNames) && kNames[id])
    {
        return kNames[id];
    }
    return fmt::format("#{}", id);
}

std::optional<uint32_t> zoneDialogFile(uint16_t zone)
{
    // English: 6420 + zone below 256; 85335 + zone from 256 (DATA: 85591 is zone 256's English dialog; the runs at
    // 85035 / 85635 / 85935 + zone hold the Japanese, German and French text).
    if (zone < 256)
    {
        return 6420u + zone;
    }
    if (zone < 304)
    {
        return 85335u + zone;
    }
    return std::nullopt;
}

std::vector<std::string> formatDialog(std::string_view s, const DialogArgs& args)
{
    std::vector<std::string> lines;
    std::string              line;
    auto at  = [&](std::size_t i) -> uint8_t { return i < s.size() ? static_cast<uint8_t>(s[i]) : 0; };
    auto num = [&](uint8_t slot) -> int32_t { return slot < args.numbers.size() ? args.numbers[slot] : 0; };
    // "[a/b/c]" at i: the option `pick` (clamped), moving i past the ']'.
    auto pickOption = [&](std::size_t& i, int32_t pick) -> std::string_view {
        if (at(i) != '[')
        {
            return {};
        }
        const auto close = s.find(']', i);
        if (close == std::string_view::npos)
        {
            return {};
        }
        std::string_view body = s.substr(i + 1, close - i - 1);
        i                     = close + 1;
        for (int32_t k = 0;; ++k)
        {
            const auto slash = body.find('/');
            if (k >= pick || slash == std::string_view::npos)
            {
                return body.substr(0, slash);
            }
            body.remove_prefix(slash + 1);
        }
    };
    std::size_t i        = 0;
    int32_t     nextPick = -1; // 01 05 85: the next "[a/b]" picks this option (the 0x6EC / 0x6ED flag, CODE 0x100bdd92)
    while (i < s.size())
    {
        const uint8_t c = at(i);
        if (c == '[' && nextPick >= 0)
        {
            line += pickOption(i, nextPick);
            nextPick = -1;
            continue;
        }
        if (c == 0x7F)
        {
            const uint8_t op = at(i + 1);
            if (op == 0x31) // end of message
            {
                break;
            }
            if (op == 0x85) // [sir/ma'am]
            {
                i += 2;
                line += pickOption(i, args.female ? 1 : 0);
            }
            else if (op == 0x92) // [singular/plural] by number slot
            {
                const uint8_t slot = at(i + 2);
                i += 3;
                line += pickOption(i, num(slot) == 1 ? 0 : 1);
            }
            else if (op == 0xFC || op == 0xFB) // around a name (DATA, file 7025): no visible text
            {
                i += 2;
            }
            else if (op == 0x90) // [his/her] by the actor's gender (DATA, file 7025)
            {
                i += 2;
                line += pickOption(i, args.female ? 1 : 0);
            }
            else if (op == 0x88) // [the /]: the target's article (DATA, file 7025)
            {
                i += 3;
                line += pickOption(i, args.targetNoArticle ? 1 : 0);
            }
            else // pauses, prompts, dates: not shown
            {
                i += 3;
            }
            continue;
        }
        switch (c)
        {
            case 0x07:
                lines.push_back(std::move(line));
                line.clear();
                ++i;
                break;
            case 0x08: // our own name (CODE 0x100c22cd: the player actor's name)
                line += args.player;
                ++i;
                break;
            case 0x0A: // number
            case 0x12: // number, as in the standard messages (7031: "Executing logout in 12 00 seconds")
                line += fmt::format("{}", num(at(i + 1)));
                i += 2;
                break;
            case 0x0B: // a menu's options follow, one per line: marked by a line holding just "\x0B"
                if (!line.empty())
                {
                    lines.push_back(std::move(line));
                    line.clear();
                }
                lines.emplace_back("\x0B");
                ++i;
                break;
            case 0x0C: // option chosen by a number
            {
                const uint8_t slot = at(i + 1);
                i += 2;
                line += pickOption(i, num(slot));
                break;
            }
            case 0x01:
                if (at(i + 1) == 0x01 && (at(i + 2) == 0x10 || at(i + 2) == 0x11)) // the actor's / target's name
                {
                    line += at(i + 2) == 0x10 ? args.actor : args.target;
                    i += 3;
                }
                else if (at(i + 1) == 0x05 && (at(i + 2) == 0x84 || at(i + 2) == 0x85)) // event name records (op 0xC3)
                {
                    const auto n = static_cast<uint32_t>(num(static_cast<uint8_t>(at(i + 4) - 0x80)));
                    if (n < args.eventNames.size())
                    {
                        if (at(i + 2) == 0x84)
                        {
                            line += args.eventNames[n].first;
                        }
                        else
                        {
                            nextPick = static_cast<int32_t>(args.eventNames[n].second & 0xFF);
                        }
                    }
                    i += 7;
                }
                else if (at(i + 1) == 0x05) // 01 05 <table> 82 <0x80 + slot> 80 80: a name from another table
                {
                    const uint8_t table = at(i + 2);
                    std::string   n     = args.lookup ? args.lookup(table, static_cast<uint32_t>(num(static_cast<uint8_t>(at(i + 4) - 0x80)))) : "";
                    i += 7;
                    if (table == 0x03 && n.empty())
                    {
                        // A count word for the item that follows (01 09 29 carries the article itself): nothing.
                        if (at(i) == ' ')
                        {
                            ++i;
                        }
                    }
                    else
                    {
                        line += n.empty() ? "(...)" : n;
                    }
                }
                else if (at(i + 1) == 0x09) // 01 09 <table> 82 <count slot> 80 80 82 <value slot> 80 80: count-aware name
                {
                    const uint8_t  table = at(i + 2);
                    const uint32_t count = static_cast<uint32_t>(num(static_cast<uint8_t>(at(i + 4) - 0x80)));
                    const uint32_t value = static_cast<uint32_t>(num(static_cast<uint8_t>(at(i + 8) - 0x80)));
                    std::string    n = args.lookup ? args.lookup(static_cast<uint16_t>(0x100 | table), (value & 0xFFFF) | (std::min<uint32_t>(count, 0xFFFF) << 16)) : "";
                    line += n.empty() ? "(...)" : n;
                    i += 11;
                }
                else if (at(i + 1) == 0x01 && at(i + 2) == 0x01) // 01 01 01: no visible text (before item names)
                {
                    i += 3;
                    if (at(i) == ' ' && !line.empty() && line.back() == ' ')
                    {
                        ++i;
                    }
                }
                else
                {
                    ++i;
                }
                break;
            case 0x1C: // text argument
            {
                const uint8_t slot = at(i + 1);
                if (slot < args.strings.size())
                {
                    line += args.strings[slot];
                }
                i += 2;
                break;
            }
            case 0x1F: // colour
                i += 2;
                break;
            case 0x02: // cutscene narration window: 02 50 00 03 54 01 (DATA, zone 230 #7667; INFERRED: placement / style
            case 0x03: // codes with two parameter bytes each)
                i += 3;
                break;
            case 0xEF: // icon glyph
                i += 2;
                break;
            default:
                if (c >= 0x20 && c < 0x7F)
                {
                    line += static_cast<char>(c);
                    ++i;
                }
                else if (c >= 0x81) // Shift-JIS symbol
                {
                    i += 2;
                }
                else
                {
                    ++i;
                }
                break;
        }
    }
    if (!line.empty())
    {
        lines.push_back(std::move(line));
    }
    return lines;
}

AutoTranslate AutoTranslate::parse(std::span<const std::byte> file)
{
    AutoTranslate t;
    auto          at = [&](std::size_t i) { return static_cast<uint8_t>(file[i]); };
    auto          id = [&](std::size_t i) { return uint32_t{ at(i) } << 24 | uint32_t{ at(i + 1) } << 16 | uint32_t{ at(i + 2) } << 8 | at(i + 3); };
    auto          str = [&](std::size_t i, std::size_t n) {
        const auto* p = reinterpret_cast<const char*>(file.data() + i);
        return std::string(p, strnlen(p, n));
    };
    std::size_t off = 0;
    while (off + 0x4C <= file.size())
    {
        uint32_t count = 0, size = 0;
        std::memcpy(&count, file.data() + off + 0x44, 4);
        std::memcpy(&size, file.data() + off + 0x48, 4);
        const uint32_t gid = id(off);
        t.phrases_[gid]    = str(off + 0x24, 32);
        Group g;
        g.id            = gid >> 8 & 0xFF;
        g.name          = t.phrases_[gid];
        std::size_t p   = off + 0x4C;
        const auto  end = std::min<std::size_t>(file.size(), p + size);
        for (uint32_t k = 0; k < count && p + 5 <= end; ++k)
        {
            const uint8_t len = at(p + 4);
            if (p + 5 + len > end)
            {
                break;
            }
            auto text = str(p + 5, len);
            if (text.find_first_not_of(' ') != std::string::npos)
            {
                g.entries.push_back({ id(p), text });
            }
            t.phrases_[id(p)] = std::move(text);
            p += 5 + len;
        }
        t.groups_.push_back(std::move(g));
        off += 0x4C + size;
    }
    return t;
}

void AutoTranslate::resolve(const std::function<std::string(char kind, uint32_t id)>& name)
{
    auto fix = [&](std::string& s) {
        if (s.size() < 3 || s[0] != '@')
        {
            return;
        }
        char*          endp = nullptr;
        const uint32_t n    = static_cast<uint32_t>(std::strtoul(s.c_str() + 2, &endp, 16));
        if (endp && *endp == 0)
        {
            if (auto r = name(s[1], n); !r.empty())
            {
                s = std::move(r);
            }
        }
    };
    for (auto& [id, text] : phrases_)
    {
        fix(text);
    }
    for (auto& g : groups_)
    {
        for (auto& e : g.entries)
        {
            fix(e.text);
        }
    }
}

namespace
{
std::string token4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return std::string{ '\xFD', static_cast<char>(a), static_cast<char>(b), static_cast<char>(c), static_cast<char>(d), '\xFD' };
}

// Items (07) and key items (13): a zero id byte is sent as FF with the type moved up (+2 high, +3 low).
std::string idToken(uint8_t type, uint16_t id)
{
    uint8_t hi = static_cast<uint8_t>(id >> 8), lo = static_cast<uint8_t>(id);
    if (hi == 0)
    {
        type = static_cast<uint8_t>(type + 2);
        hi   = 0xFF;
    }
    else if (lo == 0)
    {
        type = static_cast<uint8_t>(type + 3);
        lo   = 0xFF;
    }
    return token4(type, 0x02, hi, lo);
}
} // namespace

std::string AutoTranslate::phraseToken(uint32_t id)
{
    return token4(static_cast<uint8_t>(id >> 24), static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id));
}

std::string AutoTranslate::itemToken(uint16_t item)
{
    return idToken(0x07, item);
}

std::string AutoTranslate::keyItemToken(uint16_t keyItem)
{
    return idToken(0x13, keyItem);
}

std::optional<AutoTranslate::Token> AutoTranslate::tokenAt(std::string_view s, std::size_t i)
{
    if (i + 5 >= s.size() || static_cast<uint8_t>(s[i]) != 0xFD || static_cast<uint8_t>(s[i + 5]) != 0xFD)
    {
        return std::nullopt;
    }
    const auto b = [&](std::size_t k) { return static_cast<uint8_t>(s[i + k]); };
    Token      t;
    const auto idOf = [&](uint8_t base) {
        const uint8_t d  = static_cast<uint8_t>(b(1) - base);
        const uint8_t hi = d == 2 ? 0 : b(3), lo = d == 3 ? 0 : b(4);
        return static_cast<uint32_t>(hi << 8 | lo);
    };
    if (b(1) >= 0x07 && b(1) <= 0x0A && b(1) != 0x08)
    {
        t = { Kind::Item, idOf(0x07) };
    }
    else if (b(1) >= 0x13 && b(1) <= 0x16 && b(1) != 0x14)
    {
        t = { Kind::KeyItem, idOf(0x13) };
    }
    else
    {
        t = { b(1) == 0x02 ? Kind::Phrase : Kind::Unknown, static_cast<uint32_t>(b(1)) << 24 | static_cast<uint32_t>(b(2)) << 16 | b(3) << 8 | b(4) };
    }
    return t;
}

std::string AutoTranslate::tokenText(const Token& t) const
{
    switch (t.kind)
    {
        case Kind::Phrase: return std::string(find(t.id));
        case Kind::Item: return itemName_ ? itemName_(static_cast<uint16_t>(t.id)) : std::string();
        case Kind::KeyItem: return keyItemName_ ? keyItemName_(static_cast<uint16_t>(t.id)) : std::string();
        default: return {};
    }
}

std::string AutoTranslate::expand(std::string_view s) const
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (const auto t = tokenAt(s, i))
        {
            const auto text = tokenText(*t);
            out += '{';
            out += text.empty() ? std::string("...") : text;
            out += '}';
            i += 5;
            continue;
        }
        out += s[i];
    }
    return out;
}

std::string AutoTranslate::expandMarked(std::string_view s) const
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (const auto t = tokenAt(s, i))
        {
            const auto text = tokenText(*t);
            out += kOpenMark;
            out += text.empty() ? std::string("...") : text;
            out += kCloseMark;
            i += 5;
            continue;
        }
        out += s[i];
    }
    return out;
}

std::string AutoTranslate::plainMarks(std::string_view s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s.substr(i, 2) == kOpenMark || s.substr(i, 2) == kCloseMark)
        {
            out += s[i + 1] == '\x27' ? '{' : '}';
            ++i;
            continue;
        }
        out += s[i];
    }
    return out;
}

std::vector<AutoTranslate::Entry> AutoTranslate::search(std::string_view prefix) const
{
    auto starts = [&](std::string_view text) {
        return text.size() >= prefix.size() &&
               std::ranges::equal(text.substr(0, prefix.size()), prefix,
                                  [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    };
    std::vector<Entry> out;
    for (const auto& g : groups_)
    {
        for (const auto& e : g.entries)
        {
            if (starts(e.text) && std::ranges::none_of(out, [&](const Entry& o) { return o.text == e.text; }))
            {
                out.push_back(e);
            }
        }
    }
    return out;
}

namespace chatedit
{
namespace
{
bool tokenEndsAt(std::string_view s, std::size_t pos)
{
    return pos >= 6 && AutoTranslate::tokenAt(s, pos - 6).has_value();
}
} // namespace

std::size_t stepLeft(std::string_view s, std::size_t pos)
{
    pos = std::min(pos, s.size());
    if (pos == 0)
    {
        return 0;
    }
    if (tokenEndsAt(s, pos))
    {
        return pos - 6;
    }
    --pos;
    while (pos > 0 && (static_cast<uint8_t>(s[pos]) & 0xC0) == 0x80)
    {
        --pos;
    }
    return pos;
}

std::size_t stepRight(std::string_view s, std::size_t pos)
{
    if (pos >= s.size())
    {
        return s.size();
    }
    if (AutoTranslate::tokenAt(s, pos))
    {
        return pos + 6;
    }
    ++pos;
    while (pos < s.size() && (static_cast<uint8_t>(s[pos]) & 0xC0) == 0x80)
    {
        ++pos;
    }
    return pos;
}

std::size_t eraseLeft(std::string& s, std::size_t pos)
{
    const auto from = stepLeft(s, pos);
    s.erase(from, std::min(pos, s.size()) - from);
    return from;
}

std::size_t eraseRight(std::string& s, std::size_t pos)
{
    const auto to = stepRight(s, pos);
    s.erase(pos, to - pos);
    return pos;
}

std::size_t wordStart(std::string_view s, std::size_t pos)
{
    pos = std::min(pos, s.size());
    while (pos > 0 && s[pos - 1] != ' ' && !tokenEndsAt(s, pos))
    {
        --pos;
    }
    return pos;
}
} // namespace chatedit

std::vector<std::string_view> AutoTranslate::complete(std::string_view prefix) const
{
    std::vector<std::string_view> out;
    for (const auto& [id, text] : phrases_)
    {
        if ((id & 0xFF) != 0 && text.size() >= prefix.size() &&
            std::ranges::equal(std::string_view(text).substr(0, prefix.size()), prefix,
                               [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
        {
            out.push_back(text);
        }
    }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string AutoTranslate::encode(std::string_view s) const
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const auto close = s[i] == '{' ? s.find('}', i) : std::string_view::npos;
        if (close != std::string_view::npos)
        {
            const auto want = s.substr(i + 1, close - i - 1);
            const auto hit  = std::ranges::find_if(phrases_, [&](const auto& kv) {
                return (kv.first & 0xFF) != 0 && kv.second.size() == want.size() &&
                       std::ranges::equal(kv.second, want, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
            });
            if (hit != phrases_.end())
            {
                out += '\xFD';
                for (int k = 3; k >= 0; --k)
                {
                    out += static_cast<char>(hit->first >> (8 * k));
                }
                out += '\xFD';
                i = close;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::string BattleText::dialogName(uint16_t table, uint32_t value) const
{
    if (table == 0x33 || table == 0x36)
    {
        const auto* ki = keyItems_ ? keyItems_->find(value) : nullptr;
        return ki ? ki->name : std::string();
    }
    if (table == 0x04)
    {
        // A count before a 01 09 2a item (7031 #160 / #165 / #167 / #169 "You buy 01 05 04 .. 01 09 2a .. from the
        // guild."): the number itself (INFERRED from those texts; the article forms use 01 01 01 + 01 05 27 instead).
        return std::to_string(value);
    }
    const uint32_t count = table & 0x100 ? value >> 16 : 1;
    const auto     item  = items_ ? items_->find(static_cast<uint16_t>(value & 0xFFFF)) : std::nullopt;
    if (!item)
    {
        return {};
    }
    auto article = [](const std::string& n) {
        const char c0 = n.empty() ? 'x' : static_cast<char>(std::tolower(static_cast<unsigned char>(n[0])));
        return std::string(std::string_view("aeiou").find(c0) != std::string_view::npos ? "an " : "a ") + n;
    };
    // Tables 0x20..0x28 share one handler (CODE 0x100beb22, English): (table - 0x20) / 3 picks where the value comes
    // from (the text itself, the event's numbers, the message's numbers) and (table - 0x20) % 3 the form, so 0x20 /
    // 0x23 / 0x26 name the item as 0x23 does, 0x21 / 0x24 / 0x27 as 0x24, 0x22 / 0x25 / 0x28 as 0x25.
    switch (table)
    {
        case 0x20:
        case 0x23:
        case 0x26: return item->name;
        case 0x21:
        case 0x24:
        case 0x27: return article(item->logName);
        case 0x22:
        case 0x25:
        case 0x28:
        case 0x35: return item->logPlural;
        case 0x129: return count <= 1 ? article(item->logName) : fmt::format("{} {}", count, item->logPlural);
        case 0x12a: return count <= 1 ? item->logName : item->logPlural; // after a 01 05 04 count (INFERRED)
        default: return {};
    }
}

BattleText::BattleText(const DatIndex& index)
: BattleText(DialogTable::parse(index.read(7027)), StringTable::parse(index.read(55701)), StringTable::parse(index.read(55702)),
             StringTable::parse(index.read(55732)))
{
    misc_ = DialogTable::parse(index.read(7021));
    monsterAbilities_ = DialogTable::parse(index.read(7035));
    trust_ = StringTable::parse(index.read(55693));
    keyItems_ = KeyItemTable::parse(index.read(KeyItemTable::kFile));
    autoTranslate_ = AutoTranslate::parse(index.read(AutoTranslate::kFile));
    {
        // "@J6" / "@A68" / "@C2B" / "@Y223": job (55467), area (55465), spell and ability names by hex id.
        const auto jobs  = StringTable::parse(index.read(55467));
        const auto areas = StringTable::parse(index.read(55465));
        autoTranslate_->resolve([&](char kind, uint32_t id) -> std::string {
            switch (kind)
            {
                case 'J': return std::string(jobs.get(id));
                case 'A': return std::string(areas.get(id));
                case 'C': return std::string(spells_.get(id));
                case 'Y': return std::string(abilities_.get(id));
                default: return {};
            }
        });
        auto keys = std::make_shared<const KeyItemTable>(*keyItems_);
        autoTranslate_->setNames(nullptr, [keys](uint16_t id) {
            const auto* k = keys->find(id);
            return k ? k->name : std::string();
        });
    }
    // Help texts, indexed like the names (55733 abilities and weapon skills, 55734 spells).
    abilityHelp_ = StringTable::parse(index.read(55733));
    spellHelp_   = StringTable::parse(index.read(55734));
}

BattleText::BattleText(DialogTable messages, StringTable abilities, StringTable spells, StringTable statuses)
: messages_(std::move(messages))
, abilities_(std::move(abilities))
, spells_(std::move(spells))
, statuses_(std::move(statuses))
{
}

std::string BattleText::miscName(uint32_t id) const
{
    // File 7021 (dialog format): one name per entry, ending in control bytes. Without it (tests), the built-in skills.
    std::string_view raw = misc_.raw(id);
    while (!raw.empty() && static_cast<unsigned char>(raw.back()) < 0x20)
    {
        raw.remove_suffix(1);
    }
    return raw.empty() || raw.starts_with('#') ? skillName(id) : std::string(raw);
}

std::string BattleText::monsterAbilityName(uint32_t id) const
{
    if (id < 256)
    {
        return {};
    }
    std::string_view raw = monsterAbilities_.raw(id - 256);
    while (!raw.empty() && static_cast<unsigned char>(raw.back()) < 0x20)
    {
        raw.remove_suffix(1);
    }
    return raw.starts_with('#') ? std::string() : std::string(raw); // entry 0 is the "#0" placeholder
}

bool BattleText::has(uint16_t message) const
{
    const auto m = messages_.raw(message);
    return !m.empty() && !m.starts_with("dummy");
}

std::vector<std::string> BattleText::format(uint16_t message, const MessageArgs& args) const
{
    std::vector<std::string> lines;
    if (!has(message))
    {
        return lines;
    }
    const std::string_view s = messages_.raw(message);
    std::string            line;
    auto at  = [&](std::size_t i) -> uint8_t { return i < s.size() ? static_cast<uint8_t>(s[i]) : 0; };
    auto num = [&](uint8_t slot) -> uint32_t { return slot < args.numbers.size() ? args.numbers[slot] : 0; };
    auto name = [](std::string_view n, uint32_t id) { return n.empty() ? fmt::format("#{}", id) : std::string(n); };
    // "[first/second]" right after a control code: returns both options and moves i past the ']'.
    auto choice = [&](std::size_t& i) -> std::pair<std::string_view, std::string_view> {
        if (at(i) != '[')
        {
            return {};
        }
        const auto slash = s.find('/', i);
        const auto close = s.find(']', i);
        if (slash == std::string_view::npos || close == std::string_view::npos || slash > close)
        {
            return {};
        }
        const auto a = s.substr(i + 1, slash - i - 1);
        const auto b = s.substr(slash + 1, close - slash - 1);
        i            = close + 1;
        return { a, b };
    };

    std::size_t i = 0;
    while (i < s.size())
    {
        const uint8_t c = at(i);
        if (c == 0x7F)
        {
            const uint8_t op = at(i + 1);
            if (op == 0x31) // end of message
            {
                break;
            }
            const uint8_t slot = at(i + 2);
            switch (op)
            {
                case 0x84: // indent after a line break
                    i += 3;
                    break;
                case 0x86: // [singular/plural] by number slot
                {
                    i += 3;
                    const auto [a, b] = choice(i);
                    line += num(slot) == 1 ? a : b;
                    break;
                }
                case 0x87: // verb agreement for actor / target: always one entity here
                {
                    i += 3;
                    line += choice(i).first;
                    break;
                }
                case 0x88: // article: none for players
                {
                    i += 3;
                    const auto [a, b] = choice(i);
                    line += (slot == 0 ? args.actorIsPc : args.targetIsPc) ? b : a;
                    break;
                }
                case 0x8F: // job ability name (abilities table, offset 512)
                    line += name(abilities_.get(512 + num(slot)), num(slot));
                    i += 3;
                    break;
                case 0x90: // [his/her] without a slot
                case 0x91:
                {
                    i += 2;
                    line += choice(i).first;
                    break;
                }
                case 0xB4: // gil amount
                    line += fmt::format("{} gil", num(slot));
                    i += 3;
                    break;
                case 0x9B: // tenths
                    line += fmt::format("{}.{}", num(slot) / 10, num(slot) % 10);
                    i += 3;
                    break;
                default: // other numeric fields (0x97, 0xB0, 0xB1, 0xB7, ...)
                    line += fmt::format("{}", num(slot));
                    i += 3;
                    break;
            }
            continue;
        }
        switch (c)
        {
            case 0x00:
                ++i;
                break;
            case 0x01:
                if (at(i + 1) == 0x01) // 01 01 10 actor, 01 01 11 target, 01 01 01 article of the item named next
                {
                    const uint8_t who = at(i + 2);
                    line += who == 0x10 ? args.actor : who == 0x11 ? args.target : std::string_view();
                    i += 3;
                    if (who == 0x01)
                    {
                        // Look ahead for the item reference ("01 05 27 ..") and pick "a" / "an" from its log name.
                        const auto ref = s.find("\x01\x05", i);
                        std::string article;
                        if (ref != std::string_view::npos && items_)
                        {
                            const uint32_t n = num(static_cast<uint8_t>(at(ref + 4) - 0x80));
                            if (const auto item = items_->find(static_cast<uint16_t>(n)); item && !item->logName.empty())
                            {
                                const char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(item->logName[0])));
                                article       = std::string_view("aeiou").find(c0) != std::string_view::npos ? "an" : "a";
                            }
                        }
                        if (article.empty() && at(i) == ' ')
                        {
                            ++i; // no article: drop the following space
                        }
                        line += article;
                    }
                }
                else if (at(i + 1) == 0x05) // 01 05 <table> 82 <0x80 + slot> 80 80: a name from another table
                {
                    const uint8_t  table = at(i + 2);
                    const uint32_t n     = num(static_cast<uint8_t>(at(i + 4) - 0x80));
                    if (table == 0x13 || table == 0x14) // status effect noun / adjective
                    {
                        line += name(statuses_.get(n, table == 0x13 ? 0 : 1), n);
                    }
                    else if (table == 0x88) // a trust's party line (message 711, LSB trustPartyMessage)
                    {
                        line += trust_.get(n);
                    }
                    else if (const auto item = items_ ? items_->find(static_cast<uint16_t>(n)) : std::nullopt; item && (table >= 0x26 && table <= 0x28))
                    {
                        // 0x26 name, 0x27 log name (singular), 0x28 log name (plural).
                        line += table == 0x26 ? item->name : table == 0x27 ? item->logName : item->logPlural;
                    }
                    else
                    {
                        line += fmt::format("(item #{})", n);
                    }
                    i += 7;
                }
                else
                {
                    i += 2;
                }
                break;
            case 0x05: // name from file 7021: skills 1-57, synthesis tiers, check ratings 64-71 ("like easy prey")
                line += miscName(num(at(i + 1)));
                i += 2;
                break;
            case 0x07: // line break
                lines.push_back(std::move(line));
                line.clear();
                ++i;
                break;
            case 0x10: // spell name
                line += name(spells_.get(num(at(i + 1))), num(at(i + 1)));
                i += 2;
                break;
            case 0x12: // number
                line += fmt::format("{}", num(at(i + 1)));
                i += 2;
                break;
            case 0x16: // weapon skill (0-255, file 55701) or monster TP move (256 and up, file 7035)
            {
                const uint32_t id = num(at(i + 1));
                line += id < 256 ? name(abilities_.get(id), id) : name(monsterAbilityName(id), id);
                i += 2;
                break;
            }
            case 0x1F: // colour
                i += 2;
                break;
            default:
                if (c >= 0x20 && c < 0x7F)
                {
                    line += static_cast<char>(c);
                    ++i;
                }
                else if (c >= 0x81) // Shift-JIS symbol (e.g. an arrow): two bytes, not shown
                {
                    i += 2;
                }
                else
                {
                    ++i;
                }
                break;
        }
    }
    if (!line.empty())
    {
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace xi::dat
