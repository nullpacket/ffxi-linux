// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// The Emote List's entries (docs/ui-format-notes.md §7.7): file 96, u32 count then 32-byte records
// {u16 id, u16 label, u16 help, u16 parent, char command[16], char argument[8]} (CODE 0x10225250 / 0x10225160). Label and
// help are entries of the language's text file (English 55675). An entry without a command is a folder (/bell,
// /jobemote, /sitchair): its children have it as parent; parent 0 is the top level, 0xFFFF hidden.
namespace xi::dat
{

struct EmoteEntry
{
    uint16_t    id     = 0;
    uint16_t    label  = 0; // text entry: "/bow"
    uint16_t    help   = 0; // text entry: "Bow politely."
    uint16_t    parent = 0;
    std::string command;    // "bow", "sitchair"; empty for a folder
    std::string argument;   // "1" (/sitchair 1), "C3+" (/bell C3+), "war" (/jobemote war)

    [[nodiscard]] bool folder() const
    {
        return command.empty();
    }
    // The text command that performs it: "/sitchair 1".
    [[nodiscard]] std::string line() const
    {
        return "/" + command + (argument.empty() ? "" : " " + argument);
    }
};

class EmoteList
{
public:
    static constexpr uint32_t kFile     = 96;
    static constexpr uint32_t kTextFile = 55675; // English labels / help

    static EmoteList parse(std::span<const std::byte> file);

    [[nodiscard]] const std::vector<EmoteEntry>& entries() const
    {
        return entries_;
    }
    // The entries shown under `parent` (0 = the top level), in file order (the game's is alphabetical there too).
    [[nodiscard]] std::vector<const EmoteEntry*> children(uint16_t parent) const;

private:
    std::vector<EmoteEntry> entries_;
};

} // namespace xi::dat
