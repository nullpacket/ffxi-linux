// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"
#include "dat/items.h"

#include <array>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

// Game text tables (docs/dat-format.md §11).
namespace xi::dat
{

// `d_msg` string table: entries of one or more string fields. Some files are bit-inverted (header +0x0A = 1).
// An "XISTRING" file: "XISTRING" magic, u32 count at 0x24, `count` 12-byte entries {u32 offset, u32 length, u32 ?}
// at 0x38, then the texts (offsets from the end of the entries; plain Shift-JIS, NUL-terminated). The game's string
// table 10 is file 50 in English (Dat_LangFileId(10)): printf formats such as #39 "Macro Set %s (#%d)".
class XiStringTable
{
public:
    static XiStringTable parse(std::span<const std::byte> file);
    [[nodiscard]] std::string_view get(std::size_t index) const
    {
        return index < entries_.size() ? std::string_view(entries_[index]) : std::string_view();
    }
    [[nodiscard]] std::size_t size() const
    {
        return entries_.size();
    }

private:
    std::vector<std::string> entries_;
};

class StringTable
{
public:
    static StringTable parse(std::span<const std::byte> file);

    [[nodiscard]] std::size_t size() const
    {
        return entries_.size();
    }
    // Number of fields of entry `index` (0 when out of range).
    [[nodiscard]] std::size_t fields(std::size_t index) const
    {
        return index < entries_.size() ? entries_[index].size() : 0;
    }
    // A numeric field (type 1: a u32 at the field's offset); 0 for string fields and out of range.
    [[nodiscard]] uint32_t number(std::size_t index, std::size_t field) const
    {
        return index < numbers_.size() && field < numbers_[index].size() ? numbers_[index][field] : 0;
    }
    // Field `field` of entry `index`; empty when either is out of range.
    [[nodiscard]] std::string_view get(std::size_t index, std::size_t field = 0) const;

private:
    std::vector<std::vector<std::string>> entries_;
    std::vector<std::vector<uint32_t>>    numbers_; // per field: the value of non-string fields, else 0
};

// Dialog-format table (XOR 0x80, u32 offset table): raw messages with inline control codes.
class DialogTable
{
public:
    static DialogTable parse(std::span<const std::byte> file);

    [[nodiscard]] std::size_t size() const
    {
        return messages_.size();
    }
    [[nodiscard]] std::string_view raw(std::size_t index) const
    {
        return index < messages_.size() ? std::string_view(messages_[index]) : std::string_view();
    }

private:
    std::vector<std::string> messages_;
};

// Combat/magic/craft skill name by FFXI skill id ("great axe"); "#id" when unknown.
std::string skillName(uint32_t id);

// What a battle message refers to. Number slots as the client fills them (§11.3): 0 = action id (spell / ability),
// 1 = result param, 2 = additional-effect param, 3 = spikes / counter param. For S2C 0x029 / 0x02D, slot 0 = Data
// and slot 1 = Data2.
struct MessageArgs
{
    std::string_view        actor;
    std::string_view        target;
    bool                    actorIsPc  = false; // players get no article ("The")
    bool                    targetIsPc = false;
    std::array<uint32_t, 4> numbers{};
};

// Zone dialog (NPC text, file 6420 + zone for zones below 256): numbers from S2C 0x02A num[4]; §11.4.
struct DialogArgs
{
    std::array<int32_t, 8>     numbers{};
    bool                       female = false;
    std::array<std::string, 8> strings{}; // 0x1C n: text argument n (standard messages: "stringN <text>")
    std::string                actor, target;      // 0x01 0x01 0x10 / 0x11: the actor's / target's name (emotes, file 7025)
    std::string                player;             // 0x08: our own name (CODE 0x100c22cd)
    // Event op 0xC3's name records (0x10488210, 20 bytes each): 01 05 84 <slot> prints record n's name, 01 05 85 <slot>
    // picks the next "[a/b]" by its value byte (CODE 0x100bdb80, cases 3 / 4).
    std::array<std::pair<std::string, uint32_t>, 8> eventNames{};
    bool                       targetNoArticle = false; // 0x7F 0x88 [the /]: the second option (players take no article)
    // 01 05 <table> / 01 09 <table>: a name from another table (items, key items, ...) for a value; empty = unknown,
    // shown as "(...)". Count-aware tables (01 09) get table | 0x100 and the value | count << 16.
    std::function<std::string(uint16_t table, uint32_t value)> lookup;
};
std::vector<std::string> formatDialog(std::string_view raw, const DialogArgs& args);
// File id of a zone's dialog table; nothing for zones whose range is not mapped yet.
std::optional<uint32_t> zoneDialogFile(uint16_t zone);

// The basic battle messages (MsgBasic ids) with the name tables they reference.
// Key item names and descriptions: d_msg 55695 (English; ROM/175/35), fixed 700-byte entries. Fields: 0 key item id
// (u32), 1 category (u32), 4 in-text name, 5 plural, 6 description; entry 0 is a header. The entry order is not the id
// order (DATA: entry 1639 is id 35, Adventurer's Certificate).
// Auto-translate phrases (English: d_msg 55665, ROM/168/25). Groups back to back: u8[4] group id, 32-byte title
// ("{Greetings}" in Shift-JIS braces), 32-byte name, u32 count @0x44, u32 byte size @0x48, then `count` entries
// {u8[4] id, u8 length, text incl. NUL}. Chat carries a phrase as FD <id bytes> FD; index 0 names the group.
// Some phrases name another table by "@<kind><hex id>": @J job (55467), @A area (55465), @C spell (55702), @Y ability /
// weapon skill (55701); `resolve` swaps those for the names. Blank phrases (" ": the Controller / Keyboard glyphs) are
// kept by id but left out of the lists.
//
// The token's 4 bytes (LSB autotranslate.cpp, checked against the DLL's line object 0x101ac8b9..0x101aca40, which
// copies them as one dword between two FD bytes and refuses a token with a zero byte):
//   02 02 gg pp  phrase pp of group gg (02 = English)
//   07 02 hh ll  item hhll; hh = 0 is sent as 09 02 FF ll, ll = 0 as 0A 02 hh FF
//   13 02 hh ll  key item hhll; the zero escapes are 15 / 16 in the same way
class AutoTranslate
{
public:
    static constexpr uint32_t kFile = 55665;
    enum class Kind : uint8_t
    {
        Phrase,
        Item,
        KeyItem,
        Unknown,
    };
    struct Token
    {
        Kind     kind = Kind::Unknown;
        uint32_t id   = 0; // the phrase's 4-byte id, or the item / key item id
    };
    struct Entry
    {
        uint32_t    id = 0;
        std::string text;
    };
    struct Group
    {
        uint32_t           id = 0; // gg in 02 02 gg 00
        std::string        name;
        std::vector<Entry> entries; // file order, blanks dropped
    };
    static AutoTranslate parse(std::span<const std::byte> file);
    // The phrase for the 4 id bytes as a big-endian u32 (FD 02 02 01 01 FD = 0x02020101 "Nice to meet you.").
    [[nodiscard]] std::string_view find(uint32_t id) const
    {
        const auto it = phrases_.find(id);
        return it != phrases_.end() ? std::string_view(it->second) : std::string_view();
    }
    [[nodiscard]] std::size_t size() const
    {
        return phrases_.size();
    }
    [[nodiscard]] const std::vector<Group>& groups() const
    {
        return groups_;
    }
    // Swaps "@J6" style references for names; `name(kind, id)` returns the name or nothing.
    void resolve(const std::function<std::string(char kind, uint32_t id)>& name);
    // Names for item / key item tokens (optional; unknown ones expand to "{...}").
    void setNames(std::function<std::string(uint16_t)> item, std::function<std::string(uint16_t)> keyItem)
    {
        itemName_    = std::move(item);
        keyItemName_ = std::move(keyItem);
    }
    void setItemNames(std::function<std::string(uint16_t)> item)
    {
        itemName_ = std::move(item);
    }
    [[nodiscard]] bool hasItemNames() const
    {
        return static_cast<bool>(itemName_);
    }

    // The 6 bytes FD .. FD for a phrase id / an item / a key item, with the zero-byte escapes.
    [[nodiscard]] static std::string phraseToken(uint32_t id);
    [[nodiscard]] static std::string itemToken(uint16_t item);
    [[nodiscard]] static std::string keyItemToken(uint16_t keyItem);
    // Reads the token at `s[i]` (FD + 4 bytes + FD); nothing when there is none.
    [[nodiscard]] static std::optional<Token> tokenAt(std::string_view s, std::size_t i);
    // The text a token shows (the phrase, item or key item name); empty when unknown.
    [[nodiscard]] std::string tokenText(const Token& t) const;

    // Replaces each FD xx xx xx xx FD in a chat line with "{phrase}" ("{...}" when unknown).
    [[nodiscard]] std::string expand(std::string_view chat) const;
    // As the game shows a token in the log: the text between the gaiji brackets EF 27 / EF 28 (the green "{" and red
    // "}", "font usgaiji" 8 / 9), so a "{" a player typed stays plain.
    [[nodiscard]] std::string expandMarked(std::string_view chat) const;
    static constexpr std::string_view kOpenMark  = "\xEF\x27";
    static constexpr std::string_view kCloseMark = "\xEF\x28";
    // Turns the gaiji brackets back into "{" / "}" (for plain-text uses: files, filters, addons that want text).
    [[nodiscard]] static std::string plainMarks(std::string_view s);
    // The reverse for typed lines: each "{phrase}" naming a phrase exactly (any case) becomes its FD code.
    [[nodiscard]] std::string encode(std::string_view typed) const;
    // Phrases starting with `prefix` (any case), sorted; group names excluded. For Tab completion.
    [[nodiscard]] std::vector<std::string_view> complete(std::string_view prefix) const;
    // Phrase tokens whose text starts with `prefix` (any case), in group and file order, each phrase once.
    [[nodiscard]] std::vector<Entry> search(std::string_view prefix) const;

private:
    std::unordered_map<uint32_t, std::string> phrases_;
    std::vector<Group>                        groups_;
    std::function<std::string(uint16_t)>      itemName_, keyItemName_;
};

// Chat-line editing with auto-translate tokens: a token (FD + 4 bytes + FD) moves and deletes as one unit.
namespace chatedit
{
// The byte position one step left / right of `pos` (over a whole token, or one UTF-8 character).
std::size_t stepLeft(std::string_view s, std::size_t pos);
std::size_t stepRight(std::string_view s, std::size_t pos);
// Backspace / Delete at `pos`; returns the new cursor position.
std::size_t eraseLeft(std::string& s, std::size_t pos);
std::size_t eraseRight(std::string& s, std::size_t pos);
// The word being typed before `pos` (back to a space, a token or the start): its start position.
std::size_t wordStart(std::string_view s, std::size_t pos);
} // namespace chatedit

class KeyItemTable
{
public:
    static constexpr uint32_t kFile = 55695;
    struct KeyItem
    {
        std::string name, description;
        uint32_t    category = 0;
    };
    static KeyItemTable parse(std::span<const std::byte> file);
    [[nodiscard]] const KeyItem* find(uint32_t id) const
    {
        const auto it = items_.find(id);
        return it != items_.end() ? &it->second : nullptr;
    }
    [[nodiscard]] std::size_t size() const
    {
        return items_.size();
    }
    [[nodiscard]] const std::unordered_map<uint32_t, KeyItem>& all() const
    {
        return items_;
    }

private:
    std::unordered_map<uint32_t, KeyItem> items_;
};

// Quest and mission names (d_msg, fixed entries, DATA): field 0 id (u32), 1 name, 2 description. Quest logs:
// 55706 San d'Oria, 55707 Bastok, 55708 Windurst, 55709 Jeuno, 55710 Other Areas, 55711 Outlands, 55712 Aht Urhgan,
// 55713 Crystal War. Missions: 55715 San d'Oria, 55716 Bastok, 55717 Windurst, 55718 Zilart, 55719 Promathia,
// 55721 Aht Urhgan, 55722 Wings of the Goddess. Unused ids carry Japanese placeholder names; they are skipped.
class LogNameTable
{
public:
    static constexpr uint32_t kQuestFiles[8]   = { 55706, 55707, 55708, 55709, 55710, 55711, 55712, 55713 };
    static constexpr uint32_t kMissionFiles[4] = { 55715, 55716, 55717, 55718 }; // nation logs 0..2, Zilart
    struct Entry
    {
        std::string name, description;
    };
    static LogNameTable parse(std::span<const std::byte> file);
    [[nodiscard]] const Entry* find(uint32_t id) const
    {
        const auto it = entries_.find(id);
        return it != entries_.end() ? &it->second : nullptr;
    }

private:
    std::unordered_map<uint32_t, Entry> entries_;
};

class BattleText
{
public:
    // Loads file 7027 (messages), 55701 (abilities / weapon skills), 55702 (spells), 55732 (status effects), and the
    // help texts 55733 / 55734.
    explicit BattleText(const DatIndex& index);
    // For tests: build from already-parsed tables.
    BattleText(DialogTable messages, StringTable abilities, StringTable spells, StringTable statuses);

    [[nodiscard]] bool has(uint16_t message) const;
    // One string per display line (the source's 0x07 breaks); empty when the id is unknown or blank.
    [[nodiscard]] std::vector<std::string> format(uint16_t message, const MessageArgs& args) const;

    [[nodiscard]] std::string_view abilityName(uint32_t id) const
    {
        return abilities_.get(id);
    }
    [[nodiscard]] std::string_view statusName(uint32_t id) const
    {
        return statuses_.get(id);
    }
    [[nodiscard]] std::string_view spellName(uint32_t id) const
    {
        return spells_.get(id);
    }
    // Zone dialog name tables (INFERRED from the texts that use them): 0x23 item name, 0x24 "a" + log name, 0x25 / 0x35
    // log plural; 0x129 (01 09 29: count, item) "a crayfish" / "3 crayfish"; 0x33 / 0x36 key item. Empty when unknown.
    [[nodiscard]] std::string dialogName(uint16_t table, uint32_t value) const;
    // Auto-translate phrases for chat (empty when the file is missing).
    [[nodiscard]] std::string expandAutoTranslate(std::string_view chat) const
    {
        return autoTranslate_ ? autoTranslate_->expand(chat) : std::string(chat);
    }
    // For the chat log: tokens between the game's gaiji brackets (AutoTranslate::expandMarked).
    [[nodiscard]] std::string expandAutoTranslateMarked(std::string_view chat) const
    {
        return autoTranslate_ ? autoTranslate_->expandMarked(chat) : std::string(chat);
    }
    [[nodiscard]] std::string encodeAutoTranslate(std::string_view typed) const
    {
        return autoTranslate_ ? autoTranslate_->encode(typed) : std::string(typed);
    }
    [[nodiscard]] const AutoTranslate* autoTranslate() const
    {
        return autoTranslate_ ? &*autoTranslate_ : nullptr;
    }
    [[nodiscard]] const KeyItemTable* keyItems() const
    {
        return keyItems_ ? &*keyItems_ : nullptr;
    }
    [[nodiscard]] const ItemTable* items() const
    {
        return items_.get();
    }
    // Names for the `05 k` code: file 7021 (skills, synthesis tiers, check ratings).
    [[nodiscard]] std::string miscName(uint32_t id) const;
    // A monster TP move by the id the server sends (LSB's mob skill id, 256 and up): file 7035 (ROM/27/80, the table
    // Ashita's 'monsters.abilities' reads), entry id - 256. File 55701 holds only "." placeholders at 256-511, so the
    // `16 k` code takes names from here past 255. Empty when unknown.
    [[nodiscard]] std::string monsterAbilityName(uint32_t id) const;
    // For tests: the monster TP move table (file 7035).
    void setMonsterAbilities(DialogTable table)
    {
        monsterAbilities_ = std::move(table);
    }
    [[nodiscard]] std::string_view abilityHelp(uint32_t id) const
    {
        return abilityHelp_.get(id);
    }
    [[nodiscard]] std::string_view spellHelp(uint32_t id) const
    {
        return spellHelp_.get(id);
    }
    // Item names for messages that mention items (optional).
    void setItems(std::shared_ptr<const ItemTable> items)
    {
        items_ = std::move(items);
        if (autoTranslate_)
        {
            autoTranslate_->setItemNames([items = items_](uint16_t id) {
                const auto it = items ? items->find(id) : std::nullopt;
                return it ? it->name : std::string();
            });
        }
    }

private:
    DialogTable messages_;
    StringTable abilities_, spells_, statuses_;
    StringTable abilityHelp_, spellHelp_; // empty when built from tables (tests)
    DialogTable misc_;                    // file 7021
    DialogTable monsterAbilities_;        // file 7035: monster TP move names, entry = move id - 256
    StringTable trust_;                   // file 55693: trusts' party lines (name table 0x88)
    std::optional<KeyItemTable> keyItems_; // file 55695: key item names (dialog tables 0x33 / 0x36)
    std::optional<AutoTranslate> autoTranslate_;
    std::shared_ptr<const ItemTable> items_;
};

} // namespace xi::dat
