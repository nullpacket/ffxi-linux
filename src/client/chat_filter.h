// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace xi::client
{

// Config Misc. 4 chat filter (cfg 206: 1 term filter, 2 player filter, 0 off; config_misc_re.md §206).
// The game reads two user text files, `prohibited.txt` and `allowed.txt` (UTF-8, at most 0x23f0 bytes / 0xc80
// characters, CODE 0x101bd3d0 -> 0x101bd430), one entry per line, at most 31 characters each. An entry may start
// with flags in parentheses: `(n)` = a player name (flag 8), `(i)` (4) and `(s)` (2) are parsed but their effect
// is not decoded. In the pattern `.` is any character, `[...]` a class (`a-z` ranges, `^` negates) and `\` escapes
// one of `( ) [ ] ^ - .` (INFERRED from the parser's special characters).
// The check (CODE 0x100da510, caller 0x100e385c) runs on shout, yell and assist (J / E) lines only (message kinds 1,
// 26, 34, 35; jump table 0x100e3cac), after the text is normalised (case-folded here). A match in allowed.txt wins
// (0x101bcf10). Mode 1 drops the line (return 1 -> 0x100e3c94); mode 2 keeps it with its text replaced by string
// table 9 #345 " ***This message has been deleted.***" (0x100e3870). Both modes match alike: 0x101bca60 gets the
// sender (16 characters) and the text (0x400); `(n)` entries test the sender, the others the text.
class ChatFilter
{
public:
    enum class Verdict
    {
        Show,
        Drop,
        Deleted,
    };

    // (Re)reads prohibited.txt / allowed.txt (and profanity.txt) from `dir`; the number of prohibited entries.
    std::size_t load(const std::filesystem::path& dir);
    // Config Global profanity filter (cfg 24: 0 on, the default / 1 off; CODE 0x101ba010 -> 0x101b9f40). The game's
    // word lists are PlayOnline's vulgar2.dic / entryz.dic, which are encrypted or compressed (7.99 bits / byte, read
    // through pol.exe's file service) and not decoded; ours come from the player's own profanity.txt (the same entry
    // syntax, one per line). Matches are masked with '*' (the game's mask is SJIS 0x819B or '?'; INFERRED).
    void        setProfanity(std::string_view words);
    void        setProfanityOn(bool on);
    [[nodiscard]] std::string censor(std::string_view text) const;
    // Entries as the files' text would give them (tests).
    void        set(std::string_view prohibited, std::string_view allowed);
    [[nodiscard]] std::size_t size() const;
    void        setMode(int mode);
    [[nodiscard]] int mode() const;
    [[nodiscard]] Verdict check(uint8_t kind, std::string_view from, std::string_view text) const;
    // Whether `kind` is one the game filters (shout, yell, assist).
    [[nodiscard]] static bool filtered(uint8_t kind);

private:
    struct Entry
    {
        uint32_t    flags = 0;
        std::string pattern; // lower case
    };
    static std::vector<Entry> parse(std::string_view text);
    static bool matchAt(std::string_view pattern, std::string_view s, std::size_t at, std::size_t* end);
    static bool contains(const Entry& e, std::string_view s);

    mutable std::mutex mutex_;
    std::vector<Entry> prohibited_, allowed_, profanity_;
    int                mode_ = 0;
    bool               profanityOn_ = true;
};

} // namespace xi::client
