// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/keymap.h"

#include <glm/common.hpp>

#include <algorithm>
#include <array>

namespace xi::ui
{

namespace
{

using A = KeyAction;
using M = KeyMods;

// The fixed bindings: the game's action table (FFXiMain.dll 0x1036d6c0, 188 actions x 3 u16 key words: low byte DIK
// code, high byte modifier flags), the rows whose action is labelled and that the assignment lists do not cover.
constexpr std::array kFixed{
    KeyBinding{ SDL_SCANCODE_UP, M::Plain, A::CursorUp },
    KeyBinding{ SDL_SCANCODE_DOWN, M::Plain, A::CursorDown },
    KeyBinding{ SDL_SCANCODE_LEFT, M::Plain, A::CursorLeft },
    KeyBinding{ SDL_SCANCODE_RIGHT, M::Plain, A::CursorRight },
    KeyBinding{ SDL_SCANCODE_RETURN, M::Any, A::Confirm },
    KeyBinding{ SDL_SCANCODE_KP_ENTER, M::Any, A::Confirm },
    KeyBinding{ SDL_SCANCODE_ESCAPE, M::Plain, A::Cancel },
    KeyBinding{ SDL_SCANCODE_KP_MINUS, M::Plain, A::MainMenu },
    KeyBinding{ SDL_SCANCODE_KP_PLUS, M::Plain, A::SelectWindow },
    KeyBinding{ SDL_SCANCODE_KP_5, M::Plain, A::ChangeView },
    KeyBinding{ SDL_SCANCODE_KP_7, M::Plain, A::Autorun },
    KeyBinding{ SDL_SCANCODE_KP_MULTIPLY, M::Plain, A::LockTarget }, // action 0x30: the target object's lock toggle
    KeyBinding{ SDL_SCANCODE_KP_DIVIDE, M::Plain, A::WalkRun },
    KeyBinding{ SDL_SCANCODE_PAGEUP, M::Plain, A::ZoomIn },
    KeyBinding{ SDL_SCANCODE_KP_9, M::Plain, A::ZoomIn },
    KeyBinding{ SDL_SCANCODE_PAGEDOWN, M::Plain, A::ZoomOut },
    KeyBinding{ SDL_SCANCODE_KP_3, M::Plain, A::ZoomOut },
    KeyBinding{ SDL_SCANCODE_PRINTSCREEN, M::Any, A::Screenshot },
    KeyBinding{ SDL_SCANCODE_SCROLLLOCK, M::Any, A::HideWindow },
    KeyBinding{ SDL_SCANCODE_HOME, M::Plain, A::CameraBehind }, // action 0x51 = DIK 0xC7 | 0x80
};

// "menu k1assign" rows (labels: table 0x1037a128; defaults: 0x1019cd60; each row drives the pad bits listed at
// 0x1037a1a0, Turn Right / Left the actions 0xaa / 0xa9). Rows 0..11 and 28..29 are held keys (move / camera / turn).
constexpr std::array<AssignRow, KeyAssign::kCompactRows> kCompactTable{ {
    { 89, SDL_SCANCODE_W },             // Move Up
    { 90, SDL_SCANCODE_S },             // Move Down
    { 92, SDL_SCANCODE_A },             // Move Left
    { 91, SDL_SCANCODE_D },             // Move Right
    { 101, SDL_SCANCODE_I },            // Menu Cursor & Camera Up
    { 102, SDL_SCANCODE_K },            // ... Down
    { 104, SDL_SCANCODE_J },            // ... Left
    { 103, SDL_SCANCODE_L },            // ... Right
    { 93, SDL_SCANCODE_UNKNOWN },       // Move Camera Up
    { 94, SDL_SCANCODE_UNKNOWN },       // ... Down
    { 96, SDL_SCANCODE_UNKNOWN },       // ... Left
    { 95, SDL_SCANCODE_UNKNOWN },       // ... Right
    { 137, SDL_SCANCODE_PERIOD },       // Zoom In
    { 136, SDL_SCANCODE_COMMA },        // Zoom Out
    { 97, SDL_SCANCODE_UNKNOWN },       // Menu/Target Cursor Up
    { 98, SDL_SCANCODE_UNKNOWN },       // ... Down
    { 100, SDL_SCANCODE_UNKNOWN },      // ... Left
    { 99, SDL_SCANCODE_UNKNOWN },       // ... Right
    { 105, SDL_SCANCODE_Y },            // Confirm
    { 106, SDL_SCANCODE_N },            // Cancel
    { 107, SDL_SCANCODE_V },            // Change View
    { 108, SDL_SCANCODE_R },            // Autorun
    { 109, SDL_SCANCODE_H },            // Heal
    { 110, SDL_SCANCODE_F },            // Select Window
    { 111, SDL_SCANCODE_MINUS },        // Open Main Menu
    { 138, SDL_SCANCODE_Z },            // Toggle Walk/Run
    { 142, SDL_SCANCODE_RIGHTBRACKET }, // Hide/Show Window
    { 141, SDL_SCANCODE_LEFTBRACKET },  // Take Screenshot
    { 198, SDL_SCANCODE_E },            // Turn Right (Compact 2)
    { 199, SDL_SCANCODE_Q },            // Turn Left (Compact 2)
} };
// The press actions of Compact rows 12..27 (rows 4..7, the menu cursor, are handled in pressed()).
constexpr int        kCompactPressFirst = 12;
constexpr std::array kCompactPress{ A::ZoomIn,     A::ZoomOut, A::CursorUp, A::CursorDown,   A::CursorLeft, A::CursorRight, A::Confirm,    A::Cancel,
                                    A::ChangeView, A::Autorun, A::Heal,     A::SelectWindow, A::MainMenu,   A::WalkRun,     A::HideWindow, A::Screenshot };
static_assert(kCompactPressFirst + kCompactPress.size() == 28);

// "menu k2assign" rows (labels: table 0x1037a218; defaults: 0x1019ca10; actions: up to 5 per row at 0x1037a290).
constexpr std::array<AssignRow, KeyAssign::kShortcutRows> kShortcutTable{ {
    { 112, SDL_SCANCODE_A },            // Attack
    { 113, SDL_SCANCODE_I },            // Items
    { 114, SDL_SCANCODE_M },            // Magic
    { 115, SDL_SCANCODE_J },            // Abilities
    { 116, SDL_SCANCODE_W },            // Weapon Skills
    { 133, SDL_SCANCODE_B },            // Pet Commands
    { 134, SDL_SCANCODE_Y },            // Confirm
    { 135, SDL_SCANCODE_N },            // Cancel
    { 140, SDL_SCANCODE_MINUS },        // Open Main Menu
    { 139, SDL_SCANCODE_F },            // Select Window
    { 142, SDL_SCANCODE_RIGHTBRACKET }, // Hide/Show Window
    { 141, SDL_SCANCODE_LEFTBRACKET },  // Take Screenshot
    { 138, SDL_SCANCODE_Z },            // Toggle Walk/Run
    { 137, SDL_SCANCODE_PERIOD },       // Zoom In
    { 136, SDL_SCANCODE_COMMA },        // Zoom Out
    { 117, SDL_SCANCODE_UNKNOWN },      // Ranged Attack
    { 118, SDL_SCANCODE_UNKNOWN },      // Call For Help
    { 119, SDL_SCANCODE_E },            // Equipment
    { 120, SDL_SCANCODE_V },            // Change View
    { 121, SDL_SCANCODE_H },            // Heal
    { 122, SDL_SCANCODE_C },            // Check
    { 123, SDL_SCANCODE_S },            // Chat: /Say
    { 124, SDL_SCANCODE_P },            // Chat: /Party
    { 125, SDL_SCANCODE_T },            // Chat: /Tell
    { 126, SDL_SCANCODE_L },            // Chat: /Linkshell
    { 130, SDL_SCANCODE_R },            // Chat: /Reply to tell
    { 129, SDL_SCANCODE_U },            // Chat: /Unity
    { 127, SDL_SCANCODE_X },            // Chat: /Linkshell2
    { 128, SDL_SCANCODE_Q },            // Chat: /AssistE or /AssistJ
} };
constexpr std::array<A, KeyAssign::kShortcutRows> kShortcutAction{
    A::Attack,        A::Items,        A::Magic,      A::Abilities,      A::WeaponSkills, A::PetCommands, A::Confirm,   A::Cancel,
    A::MainMenu,      A::SelectWindow, A::HideWindow, A::Screenshot,     A::WalkRun,      A::ZoomIn,      A::ZoomOut,   A::RangedAttack,
    A::CallForHelp,   A::Equipment,    A::ChangeView, A::Heal,           A::Check,        A::ChatSay,     A::ChatParty, A::ChatTell,
    A::ChatLinkshell, A::ChatReply,    A::ChatUnity,  A::ChatLinkshell2, A::ChatAssist,
};

// The assignable keys: those with a key-top character (the US table of 0x101af8c0). The Japanese-only keys (DIK 0x7d
// yen, 0x90 ^, 0x91 @, 0x92 :) and the German / French remaps (pseudo codes 0xf0..0xfc) are not mapped.
struct KeyTop
{
    uint8_t      dik;
    SDL_Scancode key;
    char         top;
};
constexpr std::array kKeyTops{
    KeyTop{ 0x02, SDL_SCANCODE_1, '1' },           KeyTop{ 0x03, SDL_SCANCODE_2, '2' },
    KeyTop{ 0x04, SDL_SCANCODE_3, '3' },           KeyTop{ 0x05, SDL_SCANCODE_4, '4' },
    KeyTop{ 0x06, SDL_SCANCODE_5, '5' },           KeyTop{ 0x07, SDL_SCANCODE_6, '6' },
    KeyTop{ 0x08, SDL_SCANCODE_7, '7' },           KeyTop{ 0x09, SDL_SCANCODE_8, '8' },
    KeyTop{ 0x0a, SDL_SCANCODE_9, '9' },           KeyTop{ 0x0b, SDL_SCANCODE_0, '0' },
    KeyTop{ 0x0c, SDL_SCANCODE_MINUS, '-' },       KeyTop{ 0x0d, SDL_SCANCODE_EQUALS, '=' },
    KeyTop{ 0x10, SDL_SCANCODE_Q, 'Q' },           KeyTop{ 0x11, SDL_SCANCODE_W, 'W' },
    KeyTop{ 0x12, SDL_SCANCODE_E, 'E' },           KeyTop{ 0x13, SDL_SCANCODE_R, 'R' },
    KeyTop{ 0x14, SDL_SCANCODE_T, 'T' },           KeyTop{ 0x15, SDL_SCANCODE_Y, 'Y' },
    KeyTop{ 0x16, SDL_SCANCODE_U, 'U' },           KeyTop{ 0x17, SDL_SCANCODE_I, 'I' },
    KeyTop{ 0x18, SDL_SCANCODE_O, 'O' },           KeyTop{ 0x19, SDL_SCANCODE_P, 'P' },
    KeyTop{ 0x1a, SDL_SCANCODE_LEFTBRACKET, '[' }, KeyTop{ 0x1b, SDL_SCANCODE_RIGHTBRACKET, ']' },
    KeyTop{ 0x1e, SDL_SCANCODE_A, 'A' },           KeyTop{ 0x1f, SDL_SCANCODE_S, 'S' },
    KeyTop{ 0x20, SDL_SCANCODE_D, 'D' },           KeyTop{ 0x21, SDL_SCANCODE_F, 'F' },
    KeyTop{ 0x22, SDL_SCANCODE_G, 'G' },           KeyTop{ 0x23, SDL_SCANCODE_H, 'H' },
    KeyTop{ 0x24, SDL_SCANCODE_J, 'J' },           KeyTop{ 0x25, SDL_SCANCODE_K, 'K' },
    KeyTop{ 0x26, SDL_SCANCODE_L, 'L' },           KeyTop{ 0x27, SDL_SCANCODE_SEMICOLON, ';' },
    KeyTop{ 0x28, SDL_SCANCODE_APOSTROPHE, '\'' }, KeyTop{ 0x2b, SDL_SCANCODE_BACKSLASH, '\\' },
    KeyTop{ 0x2c, SDL_SCANCODE_Z, 'Z' },           KeyTop{ 0x2d, SDL_SCANCODE_X, 'X' },
    KeyTop{ 0x2e, SDL_SCANCODE_C, 'C' },           KeyTop{ 0x2f, SDL_SCANCODE_V, 'V' },
    KeyTop{ 0x30, SDL_SCANCODE_B, 'B' },           KeyTop{ 0x31, SDL_SCANCODE_N, 'N' },
    KeyTop{ 0x32, SDL_SCANCODE_M, 'M' },           KeyTop{ 0x33, SDL_SCANCODE_COMMA, ',' },
    KeyTop{ 0x34, SDL_SCANCODE_PERIOD, '.' },
};

const KeyTop* keyTopOf(SDL_Scancode key)
{
    for (const auto& k : kKeyTops)
    {
        if (k.key == key)
        {
            return &k;
        }
    }
    return nullptr;
}

bool ctrlOrAlt(SDL_Keymod mods)
{
    return (mods & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) != 0;
}

bool matches(KeyMods want, SDL_Keymod mods)
{
    switch (want)
    {
        case M::Any: return true;
        case M::Plain: return !ctrlOrAlt(mods);
        case M::CtrlOrAlt: return ctrlOrAlt(mods);
    }
    return false;
}

bool held(const bool* keys, SDL_Scancode k)
{
    return k != SDL_SCANCODE_UNKNOWN && keys[k];
}

float axis(const bool* keys, SDL_Scancode minus, SDL_Scancode plus)
{
    return static_cast<float>(held(keys, plus)) - static_cast<float>(held(keys, minus));
}

} // namespace

KeyAssign::KeyAssign()
{
    reset();
}

std::span<const AssignRow> KeyAssign::rows(AssignList list)
{
    if (list == AssignList::Compact)
    {
        return kCompactTable;
    }
    return kShortcutTable;
}

int KeyAssign::shown(AssignList list, KeyLayout layout)
{
    if (list == AssignList::Compact)
    {
        return layout == KeyLayout::Compact2 ? kCompactRows : kCompactRows - 2;
    }
    return kShortcutRows;
}

int KeyAssign::cfgId(AssignList list, int row)
{
    return (list == AssignList::Compact ? kCompactCfg : kShortcutCfg) + row;
}

SDL_Scancode KeyAssign::key(AssignList list, int row) const
{
    if (list == AssignList::Compact)
    {
        return row >= 0 && row < kCompactRows ? compact_[static_cast<std::size_t>(row)] : SDL_SCANCODE_UNKNOWN;
    }
    return row >= 0 && row < kShortcutRows ? shortcut_[static_cast<std::size_t>(row)] : SDL_SCANCODE_UNKNOWN;
}

int KeyAssign::find(AssignList list, SDL_Scancode key) const
{
    if (key == SDL_SCANCODE_UNKNOWN)
    {
        return -1;
    }
    const int n = list == AssignList::Compact ? kCompactRows : kShortcutRows;
    for (int r = 0; r < n; ++r)
    {
        if (this->key(list, r) == key)
        {
            return r;
        }
    }
    return -1;
}

KeyAssign::Outcome KeyAssign::assign(AssignList list, int row, SDL_Scancode key)
{
    const int n = list == AssignList::Compact ? kCompactRows : kShortcutRows;
    if (row < 0 || row >= n || menuKey(key))
    {
        return {};
    }
    const auto* top = keyTopOf(key);
    // The shortcut list refuses the digits: Ctrl / Alt + 1..0 are the macro palettes (CODE 0x1019cc8a).
    if (!top || (list == AssignList::Shortcut && top->dik >= 0x02 && top->dik <= 0x0b))
    {
        return { Result::Refused, -1 };
    }
    const std::span<SDL_Scancode> slots = list == AssignList::Compact ? std::span<SDL_Scancode>(compact_) : std::span<SDL_Scancode>(shortcut_);
    auto&                         slot  = slots[static_cast<std::size_t>(row)];
    if (slot == key)
    {
        slot = SDL_SCANCODE_UNKNOWN; // pressing the row's own key again clears it
        return { Result::Cleared, -1 };
    }
    // The key moves: the row that held it is left empty (CODE 0x1019c41b..0x1019c44e).
    const int other = find(list, key);
    if (other >= 0)
    {
        slots[static_cast<std::size_t>(other)] = SDL_SCANCODE_UNKNOWN;
    }
    slot = key;
    return { Result::Assigned, other };
}

bool KeyAssign::menuKey(SDL_Scancode key)
{
    switch (key)
    {
        // The game passes these over silently (jump table 0x1019c498: Enter, Ctrl, Shift, Alt, the arrows); Esc and
        // Page Up / Down are the menu's own keys here (the game beeps at them).
        case SDL_SCANCODE_UNKNOWN:
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:
        case SDL_SCANCODE_UP:
        case SDL_SCANCODE_DOWN:
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_RIGHT:
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_PAGEUP:
        case SDL_SCANCODE_PAGEDOWN: return true;
        default: return false;
    }
}

void KeyAssign::reset()
{
    for (std::size_t r = 0; r < compact_.size(); ++r)
    {
        compact_[r] = kCompactTable[r].key;
    }
    for (std::size_t r = 0; r < shortcut_.size(); ++r)
    {
        shortcut_[r] = kShortcutTable[r].key;
    }
}

void KeyAssign::load(const std::map<int, int>& cfg)
{
    auto read = [&](AssignList list, std::span<SDL_Scancode> slots) {
        for (std::size_t r = 0; r < slots.size(); ++r)
        {
            if (const auto it = cfg.find(cfgId(list, static_cast<int>(r))); it != cfg.end())
            {
                slots[r] = fromDik(it->second);
            }
        }
    };
    read(AssignList::Compact, compact_);
    read(AssignList::Shortcut, shortcut_);
}

uint8_t KeyAssign::dik(SDL_Scancode key)
{
    const auto* top = keyTopOf(key);
    return top ? top->dik : 0;
}

SDL_Scancode KeyAssign::fromDik(int dik)
{
    for (const auto& k : kKeyTops)
    {
        if (k.dik == dik)
        {
            return k.key;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

std::string KeyAssign::keyTop(SDL_Scancode key)
{
    const auto* top = keyTopOf(key);
    return top ? std::string(1, top->top) : std::string();
}

std::span<const KeyBinding> Keymap::fixed()
{
    return kFixed;
}

SDL_Scancode Keymap::row(int r) const
{
    return keys_.key(AssignList::Compact, r);
}

std::optional<KeyAction> Keymap::pressed(SDL_Scancode key, SDL_Keymod mods, bool menuOpen) const
{
    if (key == SDL_SCANCODE_UNKNOWN)
    {
        return std::nullopt;
    }
    if (layout_ != KeyLayout::Normal && !ctrlOrAlt(mods))
    {
        // Menu Cursor & Camera keys without Shift move the menu cursor while a menu is open (0x1015ecc0 check,
        // INFERRED "menu open"); otherwise they are held camera keys (camera()).
        if (menuOpen && !(mods & SDL_KMOD_SHIFT))
        {
            constexpr std::array kCursor{ A::CursorUp, A::CursorDown, A::CursorLeft, A::CursorRight };
            for (int r = 0; r < 4; ++r)
            {
                if (row(4 + r) == key)
                {
                    return kCursor[static_cast<std::size_t>(r)];
                }
            }
        }
        for (std::size_t i = 0; i < kCompactPress.size(); ++i)
        {
            if (row(kCompactPressFirst + static_cast<int>(i)) == key)
            {
                return kCompactPress[i];
            }
        }
    }
    if (ctrlOrAlt(mods))
    {
        if (const int r = keys_.find(AssignList::Shortcut, key); r >= 0)
        {
            return kShortcutAction[static_cast<std::size_t>(r)];
        }
    }
    for (const auto& b : kFixed)
    {
        if (b.key == key && matches(b.mods, mods))
        {
            return b.action;
        }
    }
    return std::nullopt;
}

glm::vec2 Keymap::move(const bool* keys, SDL_Keymod mods) const
{
    // Normal: the keyboard stick axes (0x10124080): Num4/Num6 and Num8/Num2.
    glm::vec2 v(axis(keys, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6), axis(keys, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_8));
    // Compact: the Move rows (WASD) drive the same stick. Not with Ctrl/Alt, which make them shortcut keys.
    if (layout_ != KeyLayout::Normal && !ctrlOrAlt(mods))
    {
        v += glm::vec2(axis(keys, row(2), row(3)), axis(keys, row(1), row(0)));
    }
    return glm::clamp(v, glm::vec2(-1.0f), glm::vec2(1.0f));
}

glm::vec2 Keymap::camera(const bool* keys, SDL_Keymod mods, bool menuOpen) const
{
    glm::vec2 v(0.0f);
    if (ctrlOrAlt(mods))
    {
        return v; // Ctrl/Alt + arrows are other actions (0xb0..0xb3, not traced)
    }
    if (!menuOpen)
    {
        v += glm::vec2(axis(keys, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT), axis(keys, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP));
    }
    if (layout_ != KeyLayout::Normal)
    {
        // Menu Cursor & Camera (IJKL): the camera with Shift, or without Shift while no menu is open.
        if ((mods & SDL_KMOD_SHIFT) || !menuOpen)
        {
            v += glm::vec2(axis(keys, row(6), row(7)), axis(keys, row(5), row(4)));
        }
        // Move Camera (no default keys): always the camera.
        v += glm::vec2(axis(keys, row(10), row(11)), axis(keys, row(9), row(8)));
    }
    return glm::clamp(v, glm::vec2(-1.0f), glm::vec2(1.0f));
}

float Keymap::turn(const bool* keys, SDL_Keymod mods) const
{
    // Turn Right / Left (labels 198 / 199): actions 0xaa / 0xa9, E / Q with flag 0x80 (no Ctrl/Alt), Compact 2 only.
    if (layout_ != KeyLayout::Compact2 || ctrlOrAlt(mods))
    {
        return 0.0f;
    }
    return axis(keys, row(29), row(28));
}

float Keymap::zoom(const bool* keys, SDL_Keymod mods) const
{
    float z = 0.0f;
    for (const auto& b : kFixed)
    {
        if ((b.action == A::ZoomIn || b.action == A::ZoomOut) && matches(b.mods, mods) && held(keys, b.key))
        {
            z += b.action == A::ZoomIn ? 1.0f : -1.0f;
        }
    }
    if (layout_ != KeyLayout::Normal && !ctrlOrAlt(mods))
    {
        z += axis(keys, row(kCompactPressFirst + 1), row(kCompactPressFirst)); // kCompactPress: ZoomIn, ZoomOut first
    }
    return std::clamp(z, -1.0f, 1.0f);
}

bool Keymap::zoomReset(const bool* keys, SDL_Keymod mods) const
{
    bool in = false, out = false;
    for (const auto& b : kFixed)
    {
        if ((b.action == A::ZoomIn || b.action == A::ZoomOut) && matches(b.mods, mods) && held(keys, b.key))
        {
            (b.action == A::ZoomIn ? in : out) = true;
        }
    }
    if (layout_ != KeyLayout::Normal && !ctrlOrAlt(mods))
    {
        in  = in || held(keys, row(kCompactPressFirst));
        out = out || held(keys, row(kCompactPressFirst + 1));
    }
    return in && out;
}

const char* Keymap::name(KeyAction action)
{
    switch (action)
    {
        case A::CursorUp: return "Cursor Up";
        case A::CursorDown: return "Cursor Down";
        case A::CursorLeft: return "Cursor Left";
        case A::CursorRight: return "Cursor Right";
        case A::Confirm: return "Confirm";
        case A::Cancel: return "Cancel";
        case A::MainMenu: return "Open Main Menu";
        case A::SelectWindow: return "Select Window";
        case A::ChangeView: return "Change View";
        case A::Autorun: return "Autorun";
        case A::Heal: return "Heal";
        case A::LockTarget: return "Lock Target";
        case A::WalkRun: return "Toggle Walk/Run";
        case A::ZoomIn: return "Zoom In";
        case A::ZoomOut: return "Zoom Out";
        case A::Screenshot: return "Take Screenshot";
        case A::HideWindow: return "Hide/Show Window";
        case A::CameraBehind: return "Camera Behind";
        case A::Attack: return "Attack";
        case A::Items: return "Items";
        case A::Magic: return "Magic";
        case A::Abilities: return "Abilities";
        case A::WeaponSkills: return "Weapon Skills";
        case A::PetCommands: return "Pet Commands";
        case A::Equipment: return "Equipment";
        case A::Check: return "Check";
        case A::ChatSay: return "Chat: /Say";
        case A::ChatParty: return "Chat: /Party";
        case A::ChatTell: return "Chat: /Tell";
        case A::ChatLinkshell: return "Chat: /Linkshell";
        case A::ChatLinkshell2: return "Chat: /Linkshell2";
        case A::ChatAssist: return "Chat: /Assist";
        case A::ChatUnity: return "Chat: /Unity";
        case A::ChatReply: return "Chat: /Reply to tell";
        case A::RangedAttack: return "Ranged Attack";
        case A::CallForHelp: return "Call For Help";
    }
    return "?";
}

} // namespace xi::ui
