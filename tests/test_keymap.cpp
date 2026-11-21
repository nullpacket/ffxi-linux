// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/keymap.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using xi::ui::KeyAction;
using xi::ui::KeyLayout;
using xi::ui::Keymap;

namespace
{

std::optional<KeyAction> press(const Keymap& k, SDL_Scancode sc, SDL_Keymod mods = SDL_KMOD_NONE, bool menu = false)
{
    return k.pressed(sc, mods, menu);
}

} // namespace

TEST_CASE("Normal layout: the game's action table defaults")
{
    const Keymap k(KeyLayout::Normal);
    // Rows read from FFXiMain.dll 0x1036d6c0 (docs/input-format.md).
    CHECK(press(k, SDL_SCANCODE_RETURN) == KeyAction::Confirm);
    CHECK(press(k, SDL_SCANCODE_KP_ENTER) == KeyAction::Confirm);
    CHECK(press(k, SDL_SCANCODE_ESCAPE) == KeyAction::Cancel);
    CHECK(press(k, SDL_SCANCODE_KP_MINUS) == KeyAction::MainMenu);
    CHECK(press(k, SDL_SCANCODE_KP_5) == KeyAction::ChangeView);
    CHECK(press(k, SDL_SCANCODE_KP_7) == KeyAction::Autorun);
    CHECK(press(k, SDL_SCANCODE_KP_MULTIPLY) == KeyAction::LockTarget);
    CHECK(press(k, SDL_SCANCODE_KP_DIVIDE) == KeyAction::WalkRun);
    CHECK(press(k, SDL_SCANCODE_PAGEUP) == KeyAction::ZoomIn);
    CHECK(press(k, SDL_SCANCODE_KP_3) == KeyAction::ZoomOut);
    // Plain letters do nothing in Normal; with Ctrl or Alt they are the shortcut keys.
    CHECK_FALSE(press(k, SDL_SCANCODE_Y));
    CHECK_FALSE(press(k, SDL_SCANCODE_W));
    CHECK(press(k, SDL_SCANCODE_Y, SDL_KMOD_LCTRL) == KeyAction::Confirm);
    CHECK(press(k, SDL_SCANCODE_A, SDL_KMOD_LALT) == KeyAction::Attack);
    CHECK(press(k, SDL_SCANCODE_W, SDL_KMOD_RCTRL) == KeyAction::WeaponSkills);
    CHECK(press(k, SDL_SCANCODE_S, SDL_KMOD_LCTRL) == KeyAction::ChatSay);
    // Esc and the keypad commands need Ctrl and Alt released (flag 0x80).
    CHECK_FALSE(press(k, SDL_SCANCODE_ESCAPE, SDL_KMOD_LCTRL));
    CHECK_FALSE(press(k, SDL_SCANCODE_KP_MINUS, SDL_KMOD_LALT));
}

TEST_CASE("Compact layout adds letter keys on top of Normal")
{
    const Keymap k(KeyLayout::Compact);
    CHECK(press(k, SDL_SCANCODE_Y) == KeyAction::Confirm);
    CHECK(press(k, SDL_SCANCODE_N) == KeyAction::Cancel);
    CHECK(press(k, SDL_SCANCODE_R) == KeyAction::Autorun);
    CHECK(press(k, SDL_SCANCODE_MINUS) == KeyAction::MainMenu);
    CHECK(press(k, SDL_SCANCODE_PERIOD) == KeyAction::ZoomIn);
    CHECK(press(k, SDL_SCANCODE_LEFTBRACKET) == KeyAction::Screenshot);
    // Normal's keys still work, and Ctrl/Alt + letter stays a shortcut.
    CHECK(press(k, SDL_SCANCODE_KP_MINUS) == KeyAction::MainMenu);
    CHECK(press(k, SDL_SCANCODE_R, SDL_KMOD_LCTRL) == KeyAction::ChatReply);
    // IJKL: the menu cursor while a menu is open (not with Shift), otherwise nothing to press (the camera is held).
    CHECK(press(k, SDL_SCANCODE_I, SDL_KMOD_NONE, true) == KeyAction::CursorUp);
    CHECK(press(k, SDL_SCANCODE_L, SDL_KMOD_NONE, true) == KeyAction::CursorRight);
    CHECK_FALSE(press(k, SDL_SCANCODE_K, SDL_KMOD_LSHIFT, true));
    CHECK(press(k, SDL_SCANCODE_J, SDL_KMOD_LALT) == KeyAction::Abilities);
}

TEST_CASE("Compact 2 adds Turn Left / Right on Q / E")
{
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    keys[SDL_SCANCODE_E] = true;
    CHECK(Keymap(KeyLayout::Compact2).turn(keys.data(), SDL_KMOD_NONE) == 1.0f);
    CHECK(Keymap(KeyLayout::Compact2).turn(keys.data(), SDL_KMOD_LCTRL) == 0.0f); // Ctrl+E is Equipment
    CHECK(Keymap(KeyLayout::Compact).turn(keys.data(), SDL_KMOD_NONE) == 0.0f);
    // Everything else as Compact.
    CHECK(Keymap(KeyLayout::Compact2).pressed(SDL_SCANCODE_Y, SDL_KMOD_NONE, false) == KeyAction::Confirm);
    keys = {};
    keys[SDL_SCANCODE_W] = true;
    CHECK(Keymap(KeyLayout::Compact2).move(keys.data(), SDL_KMOD_NONE) == glm::vec2(0.0f, 1.0f));
}

TEST_CASE("held keys: Zoom In / Out drive the projection zoom (goal 22)")
{
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    CHECK(Keymap(KeyLayout::Normal).zoom(keys.data(), SDL_KMOD_NONE) == 0.0f);
    keys[SDL_SCANCODE_PAGEUP] = true;
    CHECK(Keymap(KeyLayout::Normal).zoom(keys.data(), SDL_KMOD_NONE) == 1.0f);
    keys[SDL_SCANCODE_KP_3] = true; // both held: nothing
    CHECK(Keymap(KeyLayout::Normal).zoom(keys.data(), SDL_KMOD_NONE) == 0.0f);
    keys = {};
    keys[SDL_SCANCODE_PAGEDOWN] = true;
    CHECK(Keymap(KeyLayout::Compact).zoom(keys.data(), SDL_KMOD_NONE) == -1.0f);
}

TEST_CASE("held keys: movement and camera axes")
{
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    const Keymap normal(KeyLayout::Normal), compact(KeyLayout::Compact);
    keys[SDL_SCANCODE_KP_8] = true;
    keys[SDL_SCANCODE_KP_4] = true;
    CHECK(normal.move(keys.data(), SDL_KMOD_NONE) == glm::vec2(-1.0f, 1.0f));
    keys = {};
    keys[SDL_SCANCODE_W] = true;
    CHECK(normal.move(keys.data(), SDL_KMOD_NONE) == glm::vec2(0.0f));
    CHECK(compact.move(keys.data(), SDL_KMOD_NONE) == glm::vec2(0.0f, 1.0f));
    CHECK(compact.move(keys.data(), SDL_KMOD_LCTRL) == glm::vec2(0.0f)); // Ctrl+W is Weapon Skills
    keys = {};
    keys[SDL_SCANCODE_RIGHT] = true;
    CHECK(normal.camera(keys.data(), SDL_KMOD_NONE, false) == glm::vec2(1.0f, 0.0f));
    CHECK(normal.camera(keys.data(), SDL_KMOD_NONE, true) == glm::vec2(0.0f)); // the arrows move the menu cursor
    keys = {};
    keys[SDL_SCANCODE_I] = true;
    CHECK(compact.camera(keys.data(), SDL_KMOD_NONE, false) == glm::vec2(0.0f, 1.0f));
    CHECK(compact.camera(keys.data(), SDL_KMOD_NONE, true) == glm::vec2(0.0f));
    CHECK(compact.camera(keys.data(), SDL_KMOD_LSHIFT, true) == glm::vec2(0.0f, 1.0f));
}

TEST_CASE("key assignment: the game's rows and defaults")
{
    using xi::ui::AssignList;
    using xi::ui::KeyAssign;
    // "menu k1assign": 30 rows (28 unless Compact 2), "menu k2assign": 29; labels from tables 0x1037a128 / 0x1037a218.
    CHECK(KeyAssign::rows(AssignList::Compact).size() == 30);
    CHECK(KeyAssign::rows(AssignList::Shortcut).size() == 29);
    CHECK(KeyAssign::shown(AssignList::Compact, KeyLayout::Compact) == 28);
    CHECK(KeyAssign::shown(AssignList::Compact, KeyLayout::Compact2) == 30);
    CHECK(KeyAssign::rows(AssignList::Compact)[2].label == 92); // Move Left
    CHECK(KeyAssign::rows(AssignList::Compact)[2].key == SDL_SCANCODE_A);
    CHECK(KeyAssign::rows(AssignList::Shortcut)[15].label == 117); // Ranged Attack, no default
    CHECK(KeyAssign::rows(AssignList::Shortcut)[15].key == SDL_SCANCODE_UNKNOWN);
    // Config ids 74.. and 106.. hold DIK codes.
    CHECK(KeyAssign::cfgId(AssignList::Compact, 0) == 74);
    CHECK(KeyAssign::cfgId(AssignList::Shortcut, 28) == 134);
    CHECK(KeyAssign::dik(SDL_SCANCODE_W) == 0x11);
    CHECK(KeyAssign::fromDik(0x2c) == SDL_SCANCODE_Z);
    CHECK(KeyAssign::fromDik(0) == SDL_SCANCODE_UNKNOWN);
    CHECK(KeyAssign::keyTop(SDL_SCANCODE_LEFTBRACKET) == "[");
    CHECK(KeyAssign::keyTop(SDL_SCANCODE_F1).empty());
}

TEST_CASE("key assignment: assign, move, clear, refuse")
{
    using xi::ui::AssignList;
    using xi::ui::KeyAssign;
    using R = KeyAssign::Result;
    KeyAssign k;
    // Move Up (row 0) to T.
    auto o = k.assign(AssignList::Compact, 0, SDL_SCANCODE_T);
    CHECK(o.result == R::Assigned);
    CHECK(o.moved == -1);
    CHECK(k.key(AssignList::Compact, 0) == SDL_SCANCODE_T);
    // Move Down (row 1) to Y: Confirm (row 18) loses it.
    o = k.assign(AssignList::Compact, 1, SDL_SCANCODE_Y);
    CHECK(o.result == R::Assigned);
    CHECK(o.moved == 18);
    CHECK(k.key(AssignList::Compact, 18) == SDL_SCANCODE_UNKNOWN);
    // The row's own key again clears it.
    CHECK(k.assign(AssignList::Compact, 1, SDL_SCANCODE_Y).result == R::Cleared);
    CHECK(k.key(AssignList::Compact, 1) == SDL_SCANCODE_UNKNOWN);
    // No key top: refused; navigation keys: left to the menu.
    CHECK(k.assign(AssignList::Compact, 2, SDL_SCANCODE_F5).result == R::Refused);
    CHECK(k.assign(AssignList::Compact, 2, SDL_SCANCODE_UP).result == R::Ignored);
    CHECK(k.assign(AssignList::Compact, 2, SDL_SCANCODE_RETURN).result == R::Ignored);
    // Digits: fine for Compact keys, refused for shortcuts (Ctrl / Alt + digit are the macros).
    CHECK(k.assign(AssignList::Compact, 8, SDL_SCANCODE_1).result == R::Assigned);
    CHECK(k.assign(AssignList::Shortcut, 15, SDL_SCANCODE_1).result == R::Refused);
    // The lists are independent: G for Ranged Attack does not touch the Compact list.
    CHECK(k.assign(AssignList::Shortcut, 15, SDL_SCANCODE_G).result == R::Assigned);
    CHECK(k.find(AssignList::Compact, SDL_SCANCODE_G) == -1);
    k.reset();
    CHECK(k == KeyAssign());
}

TEST_CASE("key assignment: saved as config and used by the keymap")
{
    using xi::ui::AssignList;
    using xi::ui::KeyAssign;
    // As client.ini holds them: cfg_74 = 0x14 (Move Up = T), cfg_75 = 0 (Move Down unassigned), cfg_121 = 0x22 (Ranged
    // Attack = G); cfg_106 absent (Attack keeps A).
    const std::map<int, int> cfg{ { 74, 0x14 }, { 75, 0 }, { 121, 0x22 } };
    KeyAssign keys;
    keys.load(cfg);
    CHECK(keys.key(AssignList::Compact, 0) == SDL_SCANCODE_T);
    CHECK(keys.key(AssignList::Compact, 1) == SDL_SCANCODE_UNKNOWN);
    CHECK(keys.key(AssignList::Shortcut, 15) == SDL_SCANCODE_G);
    CHECK(keys.key(AssignList::Shortcut, 0) == SDL_SCANCODE_A);

    const Keymap compact(KeyLayout::Compact, keys);
    std::array<bool, SDL_SCANCODE_COUNT> held{};
    held[SDL_SCANCODE_T] = true;
    CHECK(compact.move(held.data(), SDL_KMOD_NONE) == glm::vec2(0.0f, 1.0f));
    held = {};
    held[SDL_SCANCODE_W] = true;
    CHECK(compact.move(held.data(), SDL_KMOD_NONE) == glm::vec2(0.0f)); // W no longer moves
    CHECK(press(compact, SDL_SCANCODE_G, SDL_KMOD_LCTRL) == KeyAction::RangedAttack);
    CHECK(press(compact, SDL_SCANCODE_A, SDL_KMOD_LALT) == KeyAction::Attack);
    // Shortcuts apply in the Normal layout too; the Compact list does not.
    const Keymap normal(KeyLayout::Normal, keys);
    CHECK(press(normal, SDL_SCANCODE_G, SDL_KMOD_LCTRL) == KeyAction::RangedAttack);
    held = {};
    held[SDL_SCANCODE_T] = true;
    CHECK(normal.move(held.data(), SDL_KMOD_NONE) == glm::vec2(0.0f));

    // A moved Compact key: Confirm on U, Y free.
    KeyAssign k2;
    k2.assign(AssignList::Compact, 18, SDL_SCANCODE_U);
    const Keymap c2(KeyLayout::Compact, k2);
    CHECK(press(c2, SDL_SCANCODE_U) == KeyAction::Confirm);
    CHECK_FALSE(press(c2, SDL_SCANCODE_Y).has_value());
    // Menu Cursor & Camera Up on O: the menu cursor with a menu open, the camera without.
    k2.assign(AssignList::Compact, 4, SDL_SCANCODE_O);
    const Keymap c3(KeyLayout::Compact, k2);
    CHECK(press(c3, SDL_SCANCODE_O, SDL_KMOD_NONE, true) == KeyAction::CursorUp);
    held = {};
    held[SDL_SCANCODE_O] = true;
    CHECK(c3.camera(held.data(), SDL_KMOD_NONE, false) == glm::vec2(0.0f, 1.0f));
    // Move Camera Right (no default) on ';': the camera even with a menu open.
    k2.assign(AssignList::Compact, 11, SDL_SCANCODE_SEMICOLON);
    held = {};
    held[SDL_SCANCODE_SEMICOLON] = true;
    CHECK(Keymap(KeyLayout::Compact, k2).camera(held.data(), SDL_KMOD_NONE, true) == glm::vec2(1.0f, 0.0f));
}
