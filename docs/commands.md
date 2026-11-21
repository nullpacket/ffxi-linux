# Text commands

The game's command list is in FFXiMain.dll's command table: 273 names, aliases included. Usage texts are in d_msg
55687 (shown by `/help`), and the client's own messages are in d_msg 55648 (101 "Spelling of name is incorrect.",
102 "Failed to execute command.", 120 "...A command error occurred."). The server's replies are standard messages:
S2C 0x009, text from file 7031 (indexed by LSB MsgStd), with `ParaN` / `stringN` arguments.

Unknown commands show "...A command error occurred."; they are not sent as chat.

## Implemented

| Command | What it does | Packet / source |
|---|---|---|
| `/say /s /shout /sh /party /p /yell /y /linkshell /l /linkshell2 /l2` | chat | C2S 0x0B5 kinds 0 / 1 / 4 / 26 / 5 / 27 |
| `/emote /em`, `/unity /u` | emote text, unity chat | 0x0B5 kinds 8 / 33 |
| `/assistj /aj`, `/assiste /ae` | assist channels (not verified live: LSB drops them unless `ASSIST_CHANNEL_ENABLED` and the character is eligible) | 0x0B5 kinds 0x22 / 0x23 |
| `/tell /t`, `/chatmode /cm` | tells; the channel for unprefixed lines | 0x0B6; client |
| `/echo`, `/timestamp`, `/names`, `/ver`, `/help /h /?` | client-side log, labels, version, usage text (55687) | client |
| `/random` | dice roll (0–999), announced by the server (MsgStd DiceRoll) | 0x0A2 |
| `/mutebgm`, `/mutese`, `/battlebgm` (`on`/`off`, else toggle) | music off, sound effects off, battle music on/off; `/playse ID` plays a wave (debug) | client (docs/sound-format.md) |
| emotes (`/bow /wave /cry … /dance1-4 /bell /jobemote /aim`, `text` / `motion` subcommands) | emote and motion | 0x05D; log text from file 7025 |
| `/target /ta`, `/targetnpc`, `/targetbnpc`, `/targetpc`, `/assist /as` | targeting (assist: the player's facetarget, 0x00D +0x18 bits 17..31) | client |
| `/attack /a`, `/attackoff`, `/ra /range /shoot /throw` | engage / disengage / ranged attack | 0x01A 2 / 4 / 0x10 |
| `/ma /magic /ninjutsu /nin /song /so`, `/ja /jobability /pet`, `/ws /weaponskill`, `/item`, `/equip` | actions (pet commands are job abilities); `/equip SLOT "item"` looks in the inventory then the wardrobes, `/equip SLOT` alone removes (ui-format-notes §10.12) | 0x01A, 0x037, 0x050 |
| `/recast "<name>"` | time until an ability or spell is ready, "Name: m:ss" (FFXiMain.dll's "%d:%02d") | client |
| `/mount [name]` | mount (the name's key item is 3072 "Chocobo companion" + MountId; no name = the first held) | 0x01A 0x1A |
| `/dig`, `/quarry /sprint /scout` | chocobo digging; Ballista actions (not in LSB) | 0x01A 0x11, 0x15-0x17 |
| `/returnfaith /refa /returntrust /retr <name\|all>` | release trusts: a "talk" action on the trust | 0x01A 0x00 |
| `/lockstyle [on\|off]` | lock today's gear look, unlock, or ask (MsgStd StyleLock*) | 0x053 modes 3 / 0 / 2 |
| `/search /sea [all] [job] [lv-lv] [name]` | players here (or everywhere with `all`), filtered; results in the log with "%d matches found" (55648 #203) | search server, TCP 54002 (src/client/search.h) |
| `/blacklist /blist [add\|delete <name>]` | edit the blacklist; alone it opens the Blacklist window (Delete on a name); chat from listed names is dropped; replies 55648 #116-119 | 0x03D; S2C 0x041 / 0x042 |
| `/mutelist` | the Search results for flags 2 = 0x20000000 (muted players) in every area; LSB answers none (ui-format-notes §7.5) | search server |
| `/seacom /sc [seekparty\|findmember\|sell\|buy\|ls\|recruit\|friends] <text>` | set (or with no text clear) the search comment; the game picks the category from a menu, here a leading word does. `/sea` results show the category | 0x0E0 SET_USERMSG |
| `/servmes /smes` | the server message (also shown once at the first zone-in) | 0x04B fragments {1 / 2, value1 1, value2 2 = English, offset}; S2C 0x04D answers 236 bytes at a time |
| `/nominate /propose <say\|shout\|party\|linkshell\|linkshell2> "question" "option"...`, `/vote [name] [n]` | polls: ask, answer (no number shows the question again), ask again to close; S2C 0x078 shows a proposal, 0x079 the closing tally | 0x0A0 {Kind 5 say / 6 shout / 1 party / 2-3 ls, Str}, 0x0A1 {Index, Name} |
| `/deliverybox /mailbox`, `/sendpost [name]` | the delivery box. Incoming: "Receive items (N)" pulls waiting items into empty cells, Enter takes one. Outgoing: pick an inventory stack to send to `name`, Enter takes back an unsent item or cancels a sent one; delivered items clear themselves with a log line. Allowed where LSB allows it (residential areas, towns with an auction house or the Mog menu) | 0x04D PBX; S2C 0x04B (see below) |
| `/garden`, `/layout`, `/remodel` | in your Mog House: the pot cursor, layout mode and the 2F style list, as the Moogle menu's elements; /layout and /remodel not while the house is open, /remodel only with the 2F (ui-format-notes §7.8) | 0x0FC..0x0FF, 0x0FA / 0x0FB, 0x0CB |
| `/equipset [N]`, `/equipset save N [name]`, `/lockstyleset N` | equipment sets kept by the client (the game keeps them client-side too) in `$XDG_CONFIG_HOME/ffxi-linux/equipsets/<Char>.txt`: equip set N in one packet (items from the inventory or wardrobes), save today's gear, list, lock a set's look | 0x051 EQUIPSET_SET {Count, Equipment[16] {ItemIndex, EquipKind, Category}}; 0x053 Mode 3 |
| `/statustimer` | time left under the status icons (55648 #316 / #317) | client |
| `/yellsw`, `/assistjsw`, `/assistesw` | show / hide yell and assist-channel lines | client |
| `/hidefaith /hidetrust`, `/ignorefaith /ignoretrust`, `/ignorepet` | don't draw trusts; Tab skips trusts / our pet | client |
| `/lsmes /linkshellmes`, `/ls2mes /linkshell2mes` | the linkshell message again (S2C 0x0CC; shown when it changes) | client |
| `/blockhelp` | alias of `/blockaid` | 0x01A 0x18 |
| `/itemsearch "<item>"` | the item's auction window (Price History / Bid) in a zone with an auction house | 0x02C {Language 1, Name[64]}; S2C 0x049 {ItemNo} |
| `/recruit /rec [on\|off]` | request-to-join flag | 0x0DC byte 3 bit 5 |
| `/bstpet [n\|name]` | Ready: lists the pet abilities (0x0AC PetAbilities, bit n = ability 512 + n) or uses one on the target; `/pet` reaches them too (not verified live: needs a pet) | 0x01A 9 |
| `/regionmap /rmap` | Conquest: who holds each region (names 55654), our conquest points, days to the tally. A list, not the game's Region Info screen | 0x05A (empty); S2C 0x05E Regions @0x1A, tally @0x8C, points @0x90 |
| `/besiegedmap /bmap` | Besieged: Astral Candescence holder, Al Zahbi's orders, each stronghold's level / orders / forces (0x05E @0xA4) | 0x05A |
| Enter on a monster | the battle menu: "menu battlemo" (Attack, Abilities, Magic, Items, Treasure, Check) or, engaged, "menu attackmo" (Switch Target = 0x01A 0x0F, ..., Disengage); items mapped by help line | 0x01A 2 / 0x0F / 4 |
| `/treasure /pool` (and the battle menu's Treasure) | the treasure pool ("menu loot"): our lot or the best lot, time left; Enter: Lot / Pass ("menu lootope"). The list is verified; the S2C 0x0D2 / 0x0D3 parsing waits for a kill to test | 0x041 lot {TrophyItemIndex}, 0x042 pass |
| `/fish` | fishing: cast, hook check, minigame (below) | 0x01A 0x0E, 0x110 |
| `/playtime /playlog` | "Time Played: h:mm:ss", from 0x00A +0xA0 at zone-in | client |
| `/heal`, `/sit`, `/jump`, `/dismount`, `/blockaid` | rest, sit, jump, dismount, block aid | 0x0E8, 0x0EA, 0x11D, 0x01A 0x12, 0x01A 0x18 |
| `/check /c`, `/checkname /cn`, `/checkparam` | examine | 0x0DD kinds 0 / 1 / 2 |
| `/pcmd add|leave|kick|leader|breakup`, `/invite /inv <name>`, `/join`, `/decline` | party | 0x06E, 0x06F, 0x071, 0x077, 0x070, 0x074 |
| `/acmd /alliancecmd add|leave|kick|leader|breakup` | alliance: the same packets with Kind 5 (leader: ChangeKind 1) | 0x06E, 0x06F, 0x071, 0x077, 0x070 |
| `/sitchair [n\|off]` | sit on chair n (others need their key item); status 63 + n. Sitting (47, 63-83) plays si0 / si1 / si2 | 0x113 {Mode, ChairId} |
| `/anon /anonymous`, `/away`, `/autotarget`, `/displayhead`, `/invite` (no name: seeking party) | config flags, on / off / toggle | 0x0DC |
| `/makelinkpearl /makeli`, `/makelinkpearl2 /makeli2` | a pearl from the equipped linkshell | 0x0C3 |
| `/follow`, `/lockon`, `/automove` | walk after a target, the target lock (Num *: target, camera and facing), autorun | client |
| `/keyitem`, `/quest`, `/mission`, `/map` | open the menus | client |
| `/lastsynth` | repeat the last synthesis | 0x096 |
| `/satchel /sack /case /wardrobe /wardrobe2-8 /bank /bank2 /storage /locker /recycle` | a container's items; Enter takes a stack out, "Put away..." moves one in (equipped items greyed out). The server decides where each container can be used | 0x029 {count, from, to, slot, 82 = a free slot} |
| `/sort`, `/clock`, `/logout`, `/shutdown`, `/wait` (macros) | | 0x03A; client |
| `/macro book B`, `/macro set S` | the macro palette's book (1-40) and set (1-10), wrapping; refused while the macro editor is open (ui-format-notes §7.9) | client |

## Target words

Action commands (`/ma`, `/ja`, `/item`, `/target` ...) take these after the name:

| Word | Target |
|---|---|
| `<me>` | ourselves |
| `<t>` | the current target |
| `<p0>`..`<p5>` | party members in list order (`<p0>` is us; an empty slot means us) |
| `<a10>`..`<a15>`, `<a20>`..`<a25>` | the alliance's other parties |
| `<pet>` | our pet (S2C 0x037 Flags2 bits 3..18 give its index) |
| `<bt>` | the nearest monster claimed by us or the party |
| `<st>`, `<stpc>`, `<stnpc>`, `<stpt>`, `<stal>` | pick with the cursor keys: anyone / players / NPCs and monsters / party / alliance; Enter runs the line, Esc drops it |
| `<lastst>` | the last pick |

A picked sub-target becomes `<id:HEX>` internally. GameUi commands target that entity. In the client's own handler
(chat, `/check`, emotes) it stands in for `<t>`. While a line waits for its pick, the rest of a macro waits too.

## Chat tokens

Chat lines expand these before sending:

| Token | Becomes |
|---|---|
| `<me>` | your name |
| `<t>` | the target's name |
| `<hp>`, `<hpp>` | your HP and HP % |
| `<mp>`, `<mpp>` | your MP and MP % |
| `<tp>` | your TP |
| `<pos>` | your map grid square, e.g. "(I-7)" |

Tells expand the same tokens.

## Auto-translate

Auto-translate phrases are in d_msg 55665: groups of `{u8[4] id, u8 length, text}`, laid out in `dat::AutoTranslate`
(tokens, item / key item escapes and "@" references: ui-format-notes §7.11).
- **Incoming chat:** a token shows as its text between the game's gaiji brackets (the green and red marks of "font
  usgaiji"); items and key items show their names. Braces a player typed stay plain.
- **Tab in the chat line:** opens the game's candidate window "menu fep" at the caret: a "▲ index/count" top row and 7
  candidates a page, the current one highlighted by the window's orange bar (no pointing hand). With a word typed it lists the phrases, items and key items starting with it; with nothing typed
  it lists the categories, then a category's phrases. Up / Down / Tab (Shift+Tab back) / PageUp / PageDown move (the
  cursor sound on each change), Enter inserts the phrase as one token (replacing the typed word), Esc goes back or
  closes.
- **Editing:** Left / Right / Home / End move the cursor, stepping over a token; Backspace / Delete remove a token whole.
  Up / Down recall sent lines.
- **Sending:** tokens go out as FD codes in every chat mode and in macros; in a command ("/ma {Cure} <t>") they read
  as their text. Typing "{phrase}" (any case) still sends that phrase's code.

The grid uses 32-pixel cells inside the map's 32-pixel border; this matches the map test, where pixel (300, 294) is
I-9.

## Not yet

- Needs a system not built yet:
  - the friend list (`/friendlist`): PlayOnline's, not the game server's (ui-format-notes §7.5); reading another
    player's search comment;
  - `/monsterskill`, `/emotefaith`;
  - `/recruitlist`, `/autogroup`;
  - `/cmap /colmap` (Campaign, Colonization).
- Later-expansion commands: `/mentor`, `/focustarget`, `/jobmasterdisp`.
- Help desk, and `/upset` (LSB has no emote id for it).

## Trust lines

Battle message 711 (LSB `trustPartyMessage`) is `(<actor>) <01 05 88 …>`: name table 0x88 is d_msg 55693, the trusts'
party lines, indexed by the message's parameter.

## Fishing

Verified live against LSB (with `FISHING_ENABLE` turned on for the test): two catches, a give-up. Again on 2026-10-25
(a test character in Port Windurst, real casts): a catch, a line break, out of time, a give-up, reeling in before
a bite with Enter and with Esc, and arrows answered with the real Right key (right arrows hit, left ones count wrong).

1. `/fish` sends ACTION 0x0E. With a rod and bait, the server sets our status to 56 (line out) and sends the hook delay
   in S2C 0x037 +0x4A (`fishing_timer`, seconds).
2. After that delay the client asks: C2S 0x110 `{u32 UniqueNo, i32 para, u16 ActIndex, i8 mode, u8 pad, i32 para2}`
   mode 2 (RequestCheckHook).
3. A bite: S2C 0x115 `{u16 stamina, arrow_delay, regen, move_frequency, arrow_damage, arrow_regen, time; u8
   angler_sense; u32 intuition}`, status 57.
4. The minigame. Left / Right answer the arrow; Enter reels in once the gauge is empty; Esc gives up.
5. The end: 0x110 mode 3 with `para2` = the 0x115 intuition value on a reel-in (0 otherwise) and `para` = the fish's
   stamina left in percent, `stamina x 100 / max` truncated (CODE 0x1008939e), or 200 give up (Esc), 201 the line
   reeled back in before a bite, 300 out of time (CODE 0x10089030). LSB reads it as: ≤ 4 reel in (the server decides
   the catch), ≤ 0x14 lost (low skill), ≤ 0x64 line break, ≤ 0x100 give up, higher lost. So Enter before the gauge is
   nearly empty breaks the line or loses the fish.

   Mode 5 asks for the "running out of time" warning line, once, when 0x1A3 ticks or fewer are left (`para` = the
   seconds left). Mode 4 (RequestRelease) follows once the result's motion (`fsh2..fsh6`) has played.
6. The catch line is S2C 0x027 TALKNUMWORK2: zone text with the player's name as string 0 and the item as number 0.

**The minigame** (traced: the loop CODE 0x10089030, the minigame 0x10089780, the fish from S2C 0x115 at 0x1009fc60;
`GameUi::fishingTick`). Times are 1/60 s ticks.
- The fish: stamina, `regen - 0x80` added each frame x the frame's ticks (4 at most), so it drains below 128 and
  recovers above; `move_frequency x 20` added to an arrow counter each tick, an arrow when it reaches 6000; the arrow's
  window `(rand % (arrow_delay x 2) + 2) x 10` ticks (x 0.7 for a gold one); the time limit `time x 60`. A 0x115
  while a fish is already on is ignored.
- An arrow is gold when the 0x115 intuition / (two rolls of 1..50) is not 0 (CODE 0x10089e10). Its side is random:
  a right arrow (`mai3` / `mai9`) is answered with Right, a left one (`mai0` / `mai6`) with Left; one press counts,
  not a held key. Right takes `arrow_damage` (x 2 gold); wrong gives back `arrow_regen` (x 0.25 gold, at least 1). A
  press between arrows (30+ ticks after the last) pulls for nothing: the frame's ticks x `arrow_regen` / 2.
  INFERRED: our frames are counted as at least one tick there (the game at 60 fps) and four at most.
- The arrow effects' particles live 40 ticks (`ar0a` / `ar0g` life 40 in 3727), so an arrow shows for about 0.8 s
  even when its window is longer; a press still counts until the window ends.
- The gauge (CODE 0x10089ba1) eases toward the stamina: 0.15 of the gap a frame, the whole gap once that step is under
  a point (`ui/fishing_gauge.h` `fishGaugeEase`; INFERRED: once a tick here).
- Line out, nothing hooked: Enter or Esc reels the line back in (mode 3, 201; Confirm / Cancel go to the fishing
  even with no menu open). For the cast's first 3 s (0xb4 ticks, CODE 0x10090ba2) neither acts: Enter is remembered
  (and the hook check is not sent) and reels in when the 3 s are up; Esc in that time is dropped. With no hook delay
  from the server, the hook check waits 900..1500 ticks (CODE 0x10090b79).
- Not modelled: the game also gives up (mode 3, 200) every 64 frames when 0x10177640 (the "can fish here" test the
  menus use) fails mid-fight.

`--auto-fish` is a test hook that answers the arrows.

**What draws and plays (goal 33, 2026-10-20).**
- The race's fishing file (u16 0x1035f5bc[race] + 1: 38604 Hume male) holds the `fsh0..9` schedulers that a status
  starts (56 line out `fsh0`, 57 `fsh7`, 58 `fsh2`, 60 `fsh4`, 62 `fsh6` ...) and the body motions `fh?1` (+ `fh?0`).
- The rod is a sub-actor of its own (the game's error text "サブアクター扱い(釣り竿)なのに、メインアクターがいません", rod
  set-up 0x100d339a..0x100d3437): file u16 0x1035f5a8[race] + (vfunc +0x348 − 5), which is the ranged look's index, the
  item's MId (rods 0..13; races 9..0x1d use Hume male's table entry). Hume male: 39307 + MId, e.g. Willow 10 → 39317,
  Goldfish Basket 12 → 39319 (`hm_k`, a basket), Ebisu 13 → 39320 (texture `ebis`); 39321 / 39322 (looks 14 / 15) are
  newer rod files with an extra `taik` section (fgl / mfs / flp / grw effects) no LSB rod uses. Each file holds the rod
  skeleton `sao ` (9 bones, about 2.15 long), its mesh `hf_s` (`sao1` section), the same `fsh0..9` scheduler names and
  the rod's own motions `fh?0` / `idl0` for bones 1..8, posed in the owner's model space (bone 1's translation is about
  (−0.39, −0.84, −0.03) in `idl0`: at the hands). So the rod is drawn at the owner's position and facing with the
  owner's current stem at the same frame (Characters::rodLook, actors_draw `drawRod`), and runs its scheduler of the
  same name with the race file's (fishing.cpp). The ranged slot models 0..21 (files 9416..) are empty placeholders: no
  rod shows outside fishing.
- The rod's schedulers carry the sounds: 6095 the cast, 8231 the splash, 8233 the bite / strain, 8232 / 8234 the ends
  (SeSep chunks in the rod file); the race file's `mai7` / `maia` play the arrow cue 0047.
- The arrows (`mai0..maib` in 3727) are screen-space particles (particle flags bits 2 + 3, effect-format §G.12): they
  sit a quarter yalm in front of the camera, left or right of the centre, not at the character. Until #100 the client
  drew them in world space behind the camera, so no arrow showed and nothing could be answered: the fish's stamina
  never fell. `ui/fishing_gauge.h` holds the arrow pick and the gauge's percentage.
- No line or float: no mesh, effect or code was found for one in the rod files (0..13), the race file (`fish`: `vib0`
  shake, `ase0` sweat drop, `hits` / `hitl`) or effect file 3727 (arrows, glow, fish shadow); the rod bends with its
  motions instead.
- 0x100cf0b0 runs `!f02` on an actor leaving a fishing status and `!f01` on entering one (bit 18 of +0x840, unless config
  0xbe = 1); those schedulers exist only in the 39321 / 39322 rod files.

LSB's `IDs.lua` fishing offset for West Ronfaure (7254) is one past this client's dialog file (7253 "You can't fish
here."). So the hook message shows as "You lost your catch." and the catch as "You don't know how much longer...":
server data, not a client bug.

## Zone dialog name tables

`01 05 <table> 82 <0x80 + slot> 80 80` names a value from another table. `01 09 <table> 82 <count slot> 80 80 82
<value slot> 80 80` is the count-aware form. Tables, read from the texts that use them (INFERRED unless a CODE address is given):

| Table | Name |
|---|---|
| 0x20, 0x23, 0x26 | item name |
| 0x21, 0x24, 0x27 | "a"/"an" + log name |
| 0x22, 0x25, 0x28, 0x35 | log plural |

Tables 0x20..0x28 share one handler (CODE 0x100beb22 in the English formatter 0x100be690): (table - 0x20) / 3 picks
where the value comes from (the text's own number, the event's numbers +0xc64, the message's numbers +0xc50) and
(table - 0x20) % 3 the form (tables 0x100bf22c / 0x100bf244), so 0x26 is named as 0x23 and 0x27 as 0x24.
| 0x29 (09 form) | "a crayfish" / "3 crayfish" |
| 0x03 | a count word, dropped |
| 0x33, 0x36 | key item name (55695) |
| 0x12, 0x17, 0x18, 0x38 | titles, weather, destinations, not resolved yet |
| 0x84 | event op 0xC3's name record num[slot] (CODE 0x100bdb80 case 3; the chocobo's name) |
| 0x85 | no text: the next "[a/b]" picks by that record's value byte (case 4; "[male/female]") |

Tables from 0x81 go to 0x100bdb80 (the formatter's code 01 at 0x100c1606, when the byte after `01 xx` has bit 7 set). Code 0x08 alone is our own name (0x100c22cd: the player actor's name), as in Hantileon's "Hello, <name>.".

## Search server

`/search` talks to LSB's search server (TCP 54002), the same one the auction house lists use. The framing is in
src/client/search.h. Two details took a capture to settle:

- **Bit order.** LSB's `packBitsLE` / `unpackBitsLE` make a most-significant-bit-first stream, despite the names. A list
  entry starts `03 42`: type 0, name length 6, then 'B'.
- **Response encryption.** LSB counts a response's cipher words in a `uint8`, so a list packet longer than about 1 KB is
  only encrypted up to that point. The client mirrors this.

## Delivery box

C2S 0x04D PBX: `{Command, BoxNo, PostWorkNo, ItemWorkNo, i32 ItemStacks, Result, ResParam1-3, TargetName[16]}`.

LSB rejects anything off-pattern:
- Result and ResParams must be 0.
- Open (0x0D outgoing / 0x0E incoming), close (0x0F), Query (0x0C) and Confirm (0x07) carry BoxNo -1.
- Recv (0x06) carries ItemWorkNo 1.

Unused fields are -1.

The flow, as the session runs it:
1. Open, then Work (0x01: one 0x04B per cell) and Check (0x05: new / delivered count in ResParam2 / ResParam3).
2. Incoming:
   - Recv into an empty cell;
   - Get (0x0A) moves a cell into the inventory.
3. Outgoing:
   - Query checks the name: Result 1 exists (ResParam1 1 = one of our own characters), Result 0xFB no such character.
   - Set (0x02 {cell, inventory slot, count, name}) is followed by Send (0x03).
   - Clear (0x0B) takes an unsent item back; Cancel (0x04) recalls a sent one.
   - Confirm clears one delivered item per call.

S2C 0x04B echoes `Command, BoxNo, PostWorkNo`, with Result @0xC (1 = done; a 2 comes first for multi-step replies). The
cell is a GP_POST_BOX_STATE at 0x10:
- Stat: 1 set, 3 sent, 5 cancelled, 7 received;
- the other name @0x14;
- ItemNo @0x30;
- Stack @0x38.

Verified live: one test character sent a Fire Crystal to another, who received and took it, and the sender's box
cleared it as delivered.

## Auction house

The auction counter's S2C 0x04C Command 2 opens the game's own windows:
- `menu auc1`: Bid / Sell / Sales Status;
- `menu auc2` and the sub-windows: categories mapped to LSB `aH` ids;
- `menu auclist`: items and stock from the search server, TCP_AH_REQUEST 0x15;
- `menu auc3` / `menu auchisto`: price history, TCP_AH_HISTORY 5 / 6;
- a number window for prices, then `menu aucyn`.

The highlighted row of `menu auclist` (and Sales Status) shows the item info window below the list, as the game's
auclist object keeps its item info window (0x10578204) up and follows the row (CODE 0x101d7e3f / 0x101d7e83).

Sorting (`menu auc3` row 3, "Rearrange the order of listed items", CODE 0x101d8f2c): the list is sorted by the search
server. The request carries up to 20 query terms after the category (count @0x12, `{u32 type, u32 bits}` from 0x18;
bits: 0 has a value, 1 descending, 2..16 the value), built like the game's list query (0x101040a0 sort, 0x10104010
filter). LSB's search server applies them (`AuctionParam`). Which window opens depends on the list (0x101d8db0):
- categories 1..15 and 48 (weapons): `menu wepsort` (0x101da5c0): Reset List (clears), By Damage (5, high first),
  By Delay (6, short first), By Level (2, high first), Job (filter 4 = main job), Race (filter 3 = our race);
- categories 16..26 (armor): `menu armsort` (0x101da780): Reset List, By Defense (7), By Elemental Resistance (8), By
  Level (2), Job, Race;
- the others: the row toggles a name sort (9, A to Z) on or off.

Sorts are stored per category in AUCSORT.DAT (the game's file, 0x574 bytes: 8 bytes, 0x42 x 20 entries `dir << 4 |
key`, the 0x42 counts; read 0x101d7420, written 0x101d73d0), kept beside the macro files, and replayed in order the next
time the list opens; job and race filters last only until the list is rebuilt (0x101d72a0 does not store keys 3 / 4).
After a choice the list is asked again and shows from its top.

Verified live (a test character in Southern San d'Oria): the info window on every highlighted row; Swords by level then by damage
and Reset List; Shields by defense, then filtered to MNK and to Hume; Misc. by name and back.

C2S 0x04E AUC is `{Command, AucWorkIndex, Result 0, Status 0, Param[12], Parcel[40]}`. Its commands:
- Bid 0x0E `{price, item, stacks 1 single / 0 stack}`;
- AskCommit 0x04 (fee), then LotIn 0x0B;
- Info 0x05 / WorkCheck 0x0A (sale slots), LotCancel 0x0C.

Verified live:
- opening the auction house, browsing categories, the crystal list with single and stack stock, the empty price history;
- a bid refused for lack of gil.

Also verified live:
- selling: AskCommit fee notice, Yes, the listing in the database;
- Sales Status and Remove: the item comes back to the inventory;
- a 100 gil bid that bought a Fire Crystal. LSB charges the bid, not the listing price.

Results are queued in the session (`WorldSnapshot::Auction::results`), because a LotIn reply and the slot update after it
arrive in the same frame.
