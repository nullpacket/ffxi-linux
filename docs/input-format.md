# Keyboard input and default key bindings

How FFXiMain.dll reads the keyboard, and its default bindings for both keyboard layouts. Addresses are in the
unpacked lsb-dev build (FFXiMain.dll is LZSS-packed; the addresses are those of the unpacked image). Tags: **CODE** = seen in disassembly, **DATA** = read from a
table, **INFERRED** = deduced, not confirmed. Implemented in `src/ui/keymap.*` (tests: `tests/test_keymap.cpp`).

## 1. Input pipeline (CODE)

- `0x100058f0` creates DirectInput 8 devices. The input object is at global `0x10455c20`; the keyboard device is at
  +0x88, the mouse at +0x8c and the pad at +0x90.
- `0x10005a40` polls each frame:
  - keyboard `GetDeviceState(256)`, which gives a byte per **DIK scan code**, so bindings are by physical key;
  - then `GetDeviceData`.
- On German and French clients the scan codes are remapped first, through pair tables 0x1034f9d4 / 0x1034f9b4 (language `[0x10456a28]+8` = 2 / 3).
- The key manager at `0x104e1c40` keeps a byte of flags per scan code:

  | Bit | Meaning |
  |---|---|
  | 1 | pressed |
  | 2 | held |
  | 3 | released |
  | 0 | repeat |
  | 4 | toggle state |

  Its getters are 0x10123fa0 / 0x10123fc0 (pressed), 0x10124060 (held), 0x10124000 / 0x10124020 (repeat),
  0x10123fe0 (released) and 0x10124040 (toggle).
- **Actions.** `0x101239a0(_, action, mode, slot)` tests an action against the action table at `0x1036d6c0`.
  - The table has 188 actions with 3 key words each (u16). The low byte is the DIK code; the high byte holds the
    modifier flags:

    | Flag | Meaning |
    |---|---|
    | 0x80 | Ctrl and Alt not held |
    | 0x01 | Shift |
    | 0x02 | Ctrl |
    | 0x04 | Alt |
    | 0x08 | Ctrl+Alt |
    | 0x10 | Ctrl **or** Alt (the shortcut keys; also needs shortcuts enabled, 0x1036cf6c) |
    | 0x20 | Shift not held |
    | 0x40 | not while text input is active |

  - Each action also has a context mask (u16 at `0x1036cf70`). It is ANDed with the current context at 0x1036e3b0.
  - `0x1036db28` lists two virtual-pad checkers per action, so a gamepad button fires the same action.
- **Virtual pad.** A 64-bit button mask is at `[0x104e1d44]+0x220`, and the stick bytes at +0x250..+0x253.
  - The gamepad feeds it through the button map at 0x10455c48.
  - The keyboard feeds it only through the Compact table (§3), via 0x10123630, ORed in at 0x10007477.
- **Keyboard stick axes.** `0x10122d60(axis)` falls back to the keyboard axes in `0x10124080` when the stick is centred:

  | Axis | Keys | Use |
  |---|---|---|
  | 1 | Num4 / Num6 | movement stick |
  | 2 | Num8 / Num2 | movement stick |
  | 3 | Left / Right | camera stick |
  | 4 | Up / Down | camera stick |
  | 5 | Shift+Num4 / Num6 | not traced |
  | 6 | Num9 / PgUp and Num3 / PgDn | unused |

  The axis-action table at 0x1036d0e8 pairs axes 1–4 with stick bytes 0x252 / 0x253 (movement) and 0x250 / 0x251 (camera).
- **Num Lock** matters only while text input is active (0x10630220 → 0x101b5840). With it on, the keypad keys
  0x47–0x53 are ignored for actions and movement (0x101232b0, 0x101241c0).

## 2. Keyboard type (CODE)

- The type is config id **66**, cached at `[[0x1062fc4c]+0x44]`; the setter is at 0x1019b180.
  - **0 = Normal** (help 145 / 210).
  - **1 = Compact** (help 146 / 211).
  - **2** is a later option item. It is INFERRED to be help 686, "New input method supporting scroll wheel and keyboard/mouse combination controls".
- `0x101b7a90` is true for types 1 and 2 and enables the Compact table. `0x1025dbb0` is true only for type 2 and adds Turn Left/Right (E / Q).
- Id 66 is **not** one of the 4-digit registry settings: the bootstrap table at 0x103507e8 covers ids 0–43 only. Where it is persisted was not traced.
- The in-game option is Config → Misc. 2 ("menu conf6win") → **Keyboard Size**: Full (element 5, help 145),
  Compact 1 (element 6, help 146) and Compact 2 (element 11, help 686). These are the same option items 5 / 6 / 11
  that the setter at 0x1019ae1e.. writes, so type 2 = "Compact 2".
- xi-client: Config → Misc. 2 → Keyboard Size (Full / Compact 1 / Compact 2, saved to client.ini),
  `[ffxi-linux] keyboard = 0|1|2`, or `--keyboard normal|compact|compact2`. Compact 2 = Compact plus Q/E turning (the
  camera, at the camera-key rate); its mouse-wheel / keyboard-mouse combination controls (help 686) are not traced.

## 3. Default bindings

### Normal (always active; DATA, action table 0x1036d6c0)

| Action | Keys | Notes |
|---|---|---|
| Move forward / back / left / right | Num8 / Num2 / Num4 / Num6 | stick axes (CODE); "movement" INFERRED from the Compact labels for the same stick |
| Camera | arrow keys | stick axes 3/4 (CODE); extra Shift gating in 0x10123210 / 0x10123380 not traced. xi-client: only while no menu is open |
| Menu/target cursor | arrow keys | actions 0–3, 0x2b–0x2e, 0x31–0x34; in menus events 1 / 2 / 3 / 4 = Down / Up / Right / Left (0x1011b150). In the scrolled item lists Right pages down and Left pages up (#121, ui-format-notes §10.14) |
| Confirm | Enter, Num Enter | action 0x0e (pad bit 0x1) |
| Cancel | Esc | 0x0f (pad 0x8) |
| Open Main Menu | Num − | 0x18 |
| Select Window | Num + | 0x11; from the field: the log window, then our status icons, then neither (#122, ui-format-notes §9.5); in menus event 8: the Macros options, the Items list <-> its sort window (#117, ui-format-notes §10.12) |
| Change View | Num 5 | 0x10 |
| Toggle Walk/Run | Num / | 0x45 |
| Zoom In / Out | PgUp or Num 9 / PgDn or Num 3 | 0x4f / 0x50 |
| Autorun | Num 7 | 0x1c (INFERRED: pad bit 0x100 is "Autorun" in the Compact table) |
| Lock Target | Num * | 0x30: the target lock toggle (CODE 0x10158110; ui-format-notes §9.2). The Compact table labels its pad bit 0x400 "Heal", but no heal reads action 0x30; Heal is Ctrl/Alt+H |
| Take Screenshot | Print Screen | 0x55 |
| Hide/Show Window | Scroll Lock | 0x56 |
| Macros | Ctrl+0..9 (0x6c–0x75), Alt+0..9 (0x62–0x6b) | INFERRED "macros"; Ctrl/Alt held alone (0x76 / 0x77) INFERRED to show the palette |
| Macro sets | (Ctrl\|Alt)+Shift+1..9, 0 (0x59–0x61, 0x58); Ctrl / Alt + Down / Up | set 1..10 (palette handler 0x101a3100); next / previous set while the palette shows (0x101a35e0); ui-format-notes §7.9 |
| Tab / Num 0 (no Shift), Shift+Tab / Shift+Num 0 | | 0x84 / 0x97: next / previous target (target commands 15 / 16; ui-format-notes §9.2) |
| F1–F9, F10, F11, F12 | | 0x46–0x4e, 0x52, 0x53, 0x54: party members, nearest monster (F8) and the other target commands (ui-format-notes §9.2; xi-client: F1–F6) |
| Camera behind | Home | 0x51: for 8 frames the camera swings toward 3 behind the character (CODE 0x1001ff2a, dat-format §7.8b "Camera follow") |
| Pause, plain D, lone left Shift, Ctrl/Alt+F8, Alt+Left/Right, Ctrl/Alt+arrows, Ctrl+Alt+Shift+R/B/D | | meaning not traced |

### Shortcut keys (Ctrl or Alt + key, both layouts; help 152)

The defaults come from 0x1019ca10 (CODE) and match the static table (DATA). The labels are in d_msg **55650**,
lines 112–142.

| Key | Action | Key | Action |
|---|---|---|---|
| A | Attack | Y | Confirm |
| I | Items | N | Cancel |
| M | Magic | − | Open Main Menu |
| J | Abilities | F | Select Window |
| W | Weapon Skills | ] | Hide/Show Window |
| B | Pet Commands | [ | Take Screenshot |
| E | Equipment | Z | Toggle Walk/Run |
| V | Change View | . / , | Zoom In / Out |
| H | Heal | S, P, T, L | Chat: /Say, /Party, /Tell, /Linkshell |
| C | Check | R, U, X, Q | Chat: /Reply, /Unity, /Linkshell2, /Assist |

Ranged Attack and Call for Help have no default. Saved as config ids 106–134.

### Compact additions (types 1/2; key-to-pad table 0x1036e100, restored by 0x1019cd60)

The rows map through 0x1037a1a0 to labels in 55650 lines 89–111, 136–142 and 198–199, and are saved as config ids
74–103 (the game writes 74–105; the last two slots are unused). Order and labels: §5.

| Label | Default |
|---|---|
| Move Up / Down / Left / Right | W / S / A / D |
| Menu Cursor & Camera Up / Down / Left / Right | I / K / J / L. With Shift: the camera. Without Shift: the menu cursor when 0x105eed18::0x1015ecc0() is true (INFERRED "a menu is open"), else the camera |
| Move Camera, Menu/Target Cursor | unassigned |
| Zoom In / Out | . / , |
| Confirm / Cancel | Y / N |
| Change View, Autorun, Heal | V, R, H |
| Select Window, Open Main Menu | F, − |
| Toggle Walk/Run | Z |
| Hide/Show Window, Take Screenshot | ], [ |
| Turn Right / Left (type 2 only) | E / Q |

German and French clients use other defaults; for example Confirm is DIK 0x2c on QWERTZ (0x1019cdee).

## 4. xi-client behaviour and gaps

- **Confirm with no menu open:** talks to a targeted NPC; otherwise it opens the chat line. What the game does with a monster targeted is not traced.
- **Select Window** selects the log window ("menu logwindo", help 298 / title 72 "Log Window"). While it is selected,
  Up/Down scroll one line and PageUp/PageDown one page. Confirm shows the full-screen log ("menu fulllog", 366x384),
  and Cancel steps back. Pressing it again deselects. In the full-screen log Select Window steps through its channel
  tabs (Window1, Window2, Say, Tell, Party, Linkshell, Linkshell2, Assist, Unity, Shout), Up/Down move the highlighted
  line and Left/Right page (ui-format-notes §10.2). The game probably also cycles other windows (the party list);
  that is not traced.
- **Shortcut windows:** Items, Equipment, Magic, Abilities, Weapon Skills and Pet Commands open their window alone
  (no main menu under it): Magic the spell list of every type but trust, Abilities / Weapon Skills / Pet Commands
  their ability list (ui-format-notes §9.1, CODE 0x101adcf0).
- **Not implemented:** Compact 2's mouse controls, and the German/French scan-code remaps (also of the
  assignable keys, §5).
- **Placeholders (not traced):** the walk speed (0.4 × run) and the keyboard camera turn rates.
- **Camera key direction:** Right / Up turn the camera the opposite way to dragging the mouse (checked by the user
  against the original client, 2026-10-17; the game's code was not traced for it). There is no sprint: Shift does not
  change the movement speed.
- **/Reply** opens `/t <name> ` with the sender of the last tell received (chat kind 3).
- **Additions not in the game's tables:**
  - Space opens the chat line;
  - Insert opens the main menu;
  - the mouse wheel over the log scrolls it (PageUp/PageDown zoom, as in the game);
  - F11 is the debug overlay.

## 5. Key re-assignment (CODE, 2026-10-18)

Config → Misc. 2 → Key Assignment: **Movement** ("menu k1assign", help 151 / window help 277 "Assign keys for
movement, cursor, and camera controls. (Only when using compact keyboard.)"), **Commands** ("menu k2assign", help
152 / 278 "Assign keys for shortcut commands.") and **Default** (help 153; yes / no with 55650 #143 "Reset keyboard
assignment to default?", CODE 0x10196a6a -> 0x10196ad0, which restores **both** lists: 0x1019cd60 and 0x1019ca10).
Each list is one object that is both the menu and the key table (globals 0x1062fc50 Compact / 0x1062fc54 shortcuts;
open 0x1019cf50 / 0x1019cb90, close 0x1019cff0 / 0x1019cc30, input 0x1019c550, draw 0x1019c180).

**Which keyboard layouts.** The Compact list drives the key-to-pad table and only acts in Compact 1 / 2 (the table is
enabled by 0x101b7a90); its last two rows (Turn Right / Left) are listed only in Compact 2 (row count 0x1e, else 0x1c).
The shortcut list acts in every layout (Ctrl or Alt + the key). Neither touches the fixed Full-keyboard bindings (§3).

**Rows** (index: label in 55650, default):

| Compact (cfg 74 + i) | | Shortcut (cfg 106 + i) | |
|---|---|---|---|
| 0 Move Up (89) W | 15 Menu/Target Cursor Down (98) – | 0 Attack (112) A | 15 Ranged Attack (117) – |
| 1 Move Down (90) S | 16 … Left (100) – | 1 Items (113) I | 16 Call For Help (118) – |
| 2 Move Left (92) A | 17 … Right (99) – | 2 Magic (114) M | 17 Equipment (119) E |
| 3 Move Right (91) D | 18 Confirm (105) Y | 3 Abilities (115) J | 18 Change View (120) V |
| 4 Menu Cursor & Camera Up (101) I | 19 Cancel (106) N | 4 Weapon Skills (116) W | 19 Heal (121) H |
| 5 … Down (102) K | 20 Change View (107) V | 5 Pet Commands (133) B | 20 Check (122) C |
| 6 … Left (104) J | 21 Autorun (108) R | 6 Confirm (134) Y | 21 /Say (123) S |
| 7 … Right (103) L | 22 Heal (109) H | 7 Cancel (135) N | 22 /Party (124) P |
| 8 Move Camera Up (93) – | 23 Select Window (110) F | 8 Open Main Menu (140) − | 23 /Tell (125) T |
| 9 … Down (94) – | 24 Open Main Menu (111) − | 9 Select Window (139) F | 24 /Linkshell (126) L |
| 10 … Left (96) – | 25 Toggle Walk/Run (138) Z | 10 Hide/Show Window (142) ] | 25 /Reply (130) R |
| 11 … Right (95) – | 26 Hide/Show Window (142) ] | 11 Take Screenshot (141) [ | 26 /Unity (129) U |
| 12 Zoom In (137) . | 27 Take Screenshot (141) [ | 12 Toggle Walk/Run (138) Z | 27 /Linkshell2 (127) X |
| 13 Zoom Out (136) , | 28 Turn Right (198) E | 13 Zoom In (137) . | 28 /Assist (128) Q |
| 14 Menu/Target Cursor Up (97) – | 29 Turn Left (199) Q | 14 Zoom Out (136) , | |

Labels: tables 0x1037a128 (Compact, u32 per row) and 0x1037a218 (shortcuts, 0-terminated). Each Compact row 0..27
names up to four pad bits (0x1037a1a0, 0xff-padded; the Menu Cursor & Camera rows drive four bits each); rows 28 / 29
set actions 0xaa / 0xa9 (0x1019cec0). Each shortcut row names up to five action ids (0x1037a290, s16, -1-padded;
0x1019cb40 -> 0x101235b0). "Move Camera" rows are always the camera; "Menu Cursor & Camera" rows are the menu cursor
while a menu is open (without Shift) and the camera otherwise; "Menu/Target Cursor" rows are cursor keys.

**Capturing a key** (CODE 0x1019c3a0 stores the pressed DIK; 0x1019c3c0 / 0x1019cc80 apply it on the next frame to
row = cursor + scroll):

- keys without a key top (0x101af890 -> table 0x10630120, built by 0x101af8c0 per language) beep (0x10037b80), except
  Enter, Num Enter, both Ctrl / Shift / Alt and the arrows, which pass silently (jump table 0x1019c498);
- the shortcut list also beeps at the digits 1–0 (DIK 0x02–0x0b): Ctrl / Alt + digit are the macro palettes;
- pressing the key the row already holds clears the row;
- otherwise the row takes the key, and a row of the **same list** that held it is cleared (a key is in at most one
  row per list; the two lists are independent: Ctrl+K and plain K do not collide).

Assignable keys on a US client (key tops): 1–0, − =, Q–P, [ ], A–L, ; ' \, Z–M, , . — and on Japanese keyboards ¥
(0x7d), ^ (0x90), @ (0x91), : (0x92). German / French clients use pseudo codes 0xf0..0xfc for their extra keys.

**Saving.** Closing a list writes every row's DIK to its config id (Set 0x10193740; 0 = none) and re-applies the
table; where these ids are stored (per character in `cnf.dat` like the other local ids?) was not traced. There is no confirm / cancel step: changes apply as keys are pressed.

**xi-client:** `ui::KeyAssign` (rows, defaults, assign / clear / move, DIK <-> SDL scancode for the US key tops) and
`ui::Keymap(layout, keys)`; the lists are `GameUi::Kind::KeyAssign`. Each change is saved at once as `cfg_74`.. /
`cfg_106`.. in client.ini's `[ffxi-linux]` section (DIK code, 0 = none; an absent id keeps the default), shared by
all characters. Esc / Page Up / Page Down are the list's own keys here (the game
beeps). Ranged Attack sends `/ra` and Call For Help `/callforhelp` (C2S 0x01A category 5, LSB `Help`). Not mapped:
the Japanese-only keys and the German / French pseudo codes.
