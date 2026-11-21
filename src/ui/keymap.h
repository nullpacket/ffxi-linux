// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <glm/vec2.hpp>

// The modifier keys held, or none while the window does not have the keyboard focus (SDL can report keys held in
// other windows: Ctrl / Alt opened the macro palettes in the background).
inline SDL_Keymod focusedModState()
{
    return SDL_GetKeyboardFocus() ? SDL_GetModState() : SDL_KMOD_NONE;
}

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>

// The game's default keyboard bindings (docs/input-format.md). FFXI reads the keyboard by DirectInput scan code, so
// bindings are by physical key (SDL scancodes), independent of the keyboard's language layout.
//
// Two layouts, the game's Config "keyboard" option (config id 66, 0 = Normal, 1 = Compact):
//   Normal   full-size keyboards: numeric-keypad movement, arrows for the cursor and camera.
//   Compact  keyboards without a numeric keypad: everything in Normal plus letter keys for movement (WASD), cursor and
//            camera (IJKL), and single-key commands (Y/N confirm/cancel, R autorun, ...).
// In both, Ctrl or Alt + a letter is a shortcut command (Ctrl/Alt + A attack, + I items, + S /say, ...).
// The Compact keys and the shortcut keys can be re-assigned (KeyAssign: Config -> Misc. 2 -> "menu k1assign" /
// "menu k2assign"); the rest are fixed.
namespace xi::ui
{

enum class KeyLayout : uint8_t
{
    Normal   = 0,
    Compact  = 1,
    Compact2 = 2, // Compact plus Turn Left / Right on Q / E (0x1025dbb0); its mouse-wheel controls are not traced
};

enum class KeyAction : uint8_t
{
    CursorUp,
    CursorDown,
    CursorLeft,
    CursorRight,
    Confirm,
    Cancel,
    MainMenu,
    SelectWindow,
    ChangeView,
    Autorun,
    Heal,
    WalkRun,
    ZoomIn,
    ZoomOut,
    Screenshot,
    HideWindow,
    CameraBehind, // Home (action 0x51): swing the camera behind the character (CODE 0x1001ff2a)
    // Shortcut commands (Ctrl or Alt + key).
    Attack,
    Items,
    Magic,
    Abilities,
    WeaponSkills,
    PetCommands,
    Equipment,
    Check,
    ChatSay,
    ChatParty,
    ChatTell,
    ChatLinkshell,
    ChatLinkshell2,
    ChatAssist,
    ChatUnity,
    ChatReply,
    RangedAttack, // shortcut list only, no default key
    CallForHelp,  // shortcut list only, no default key
    LockTarget,   // Num * (key action 0x30, CODE 0x10158110): lock / unlock the target, as /lockon
};

// Modifier condition of a binding (the high byte of the game's key words).
enum class KeyMods : uint8_t
{
    Any,       // whatever is held
    Plain,     // neither Ctrl nor Alt (game flag 0x80)
    CtrlOrAlt, // Ctrl or Alt held: the shortcut keys (game flag 0x10)
};

struct KeyBinding
{
    SDL_Scancode key;
    KeyMods      mods;
    KeyAction    action;
};

// The two re-assignable key lists (docs/input-format.md §5). Each row holds one key (or none); a key is in at most one
// row of a list. Saved as the game does: config ids 74.. (Compact) and 106.. (shortcuts), the value a DIK scan code
// (0 = none), here as cfg_<id> in client.ini.
enum class AssignList : uint8_t
{
    Compact,  // "menu k1assign": movement, cursor, camera and single-key commands (Compact keyboards only)
    Shortcut, // "menu k2assign": the Ctrl / Alt + key shortcut commands (both layouts)
};

struct AssignRow
{
    uint16_t     label;   // the row's text: d_msg 55650 line
    SDL_Scancode key;     // the default (US layout; 0x1019cd60 / 0x1019ca10)
};

class KeyAssign
{
public:
    static constexpr int kCompactRows  = 30; // 28 unless Compact 2 (Turn Right / Left; CODE 0x1019cf50)
    static constexpr int kShortcutRows = 29;
    static constexpr int kCompactCfg   = 74; // first config id of each list (CODE 0x1019d00f / 0x1019cc4f)
    static constexpr int kShortcutCfg  = 106;

    KeyAssign(); // the defaults

    [[nodiscard]] static std::span<const AssignRow> rows(AssignList list);
    // Rows the menu shows: the Compact list's last two (Turn Right / Left) only in Compact 2.
    [[nodiscard]] static int shown(AssignList list, KeyLayout layout);
    [[nodiscard]] static int cfgId(AssignList list, int row);

    [[nodiscard]] SDL_Scancode key(AssignList list, int row) const;
    // The row holding a key, or -1.
    [[nodiscard]] int find(AssignList list, SDL_Scancode key) const;

    // A key pressed on a row of the assignment menu (CODE 0x1019c3c0 / 0x1019cc80).
    enum class Result : uint8_t
    {
        Ignored,  // navigation / modifier keys (Enter, arrows, Shift, Ctrl, Alt, Esc, Page Up / Down): the menu's own
        Refused,  // a key that cannot be assigned (no key-top character; digits in the shortcut list): error sound
        Assigned, // the row now holds the key; `moved` = the row that held it before (now empty), or -1
        Cleared,  // the row already held the key: now empty
    };
    struct Outcome
    {
        Result result = Result::Ignored;
        int    moved  = -1;
    };
    Outcome assign(AssignList list, int row, SDL_Scancode key);
    // The keys the assignment menu leaves to its own navigation (Result::Ignored).
    [[nodiscard]] static bool menuKey(SDL_Scancode key);
    void    reset(); // both lists to their defaults ("Reset keyboard assignment to default?", CODE 0x10196ad0)

    // Config values: the DIK code per row. load() reads ids 74..103 / 106..134 (missing ids keep the default).
    void load(const std::map<int, int>& cfg);
    [[nodiscard]] static uint8_t      dik(SDL_Scancode key);
    [[nodiscard]] static SDL_Scancode fromDik(int dik);
    // The key-top character the menu shows ("" for none; 0x101af890).
    [[nodiscard]] static std::string keyTop(SDL_Scancode key);

    bool operator==(const KeyAssign&) const = default;

private:
    std::array<SDL_Scancode, kCompactRows>  compact_{};
    std::array<SDL_Scancode, kShortcutRows> shortcut_{};
};

class Keymap
{
public:
    explicit Keymap(KeyLayout layout = KeyLayout::Normal, const KeyAssign& keys = {}) : layout_(layout), keys_(keys)
    {
    }

    [[nodiscard]] KeyLayout layout() const
    {
        return layout_;
    }

    // The action a key press triggers, if any. `menuOpen`: the Compact IJKL keys move the menu cursor while a menu
    // is open (and the camera otherwise, see camera()).
    [[nodiscard]] std::optional<KeyAction> pressed(SDL_Scancode key, SDL_Keymod mods, bool menuOpen) const;

    // Held keys as axes, each component -1..1. move: x = right, y = forward. camera: x = right, y = up. The arrow
    // keys (and Compact IJKL without Shift) turn the camera only while no menu is open.
    [[nodiscard]] glm::vec2 move(const bool* keys, SDL_Keymod mods) const;
    [[nodiscard]] glm::vec2 camera(const bool* keys, SDL_Keymod mods, bool menuOpen) const;
    // Compact 2's Turn Left / Right (Q / E, no Ctrl/Alt): -1 left .. 1 right; 0 in the other layouts.
    [[nodiscard]] float turn(const bool* keys, SDL_Keymod mods) const;
    // Zoom In / Out held: 1 in .. -1 out (the game raises its projection zoom while Zoom In is held, CODE 0x1001f742).
    [[nodiscard]] float zoom(const bool* keys, SDL_Keymod mods) const;
    // Zoom In and Zoom Out both held: the game eases the zoom back to 350 (CODE 0x1001f6f4, DAT_10456d84).
    [[nodiscard]] bool zoomReset(const bool* keys, SDL_Keymod mods) const;

    [[nodiscard]] const KeyAssign& keys() const
    {
        return keys_;
    }

    // The fixed key-press bindings (the action table rows that are not re-assignable), active in both layouts.
    [[nodiscard]] static std::span<const KeyBinding> fixed();
    [[nodiscard]] static const char* name(KeyAction action);

private:
    [[nodiscard]] SDL_Scancode row(int r) const; // a Compact row's key

    KeyLayout layout_;
    KeyAssign keys_;
};

} // namespace xi::ui
