# Event scripts (cutscenes and NPC dialog)

How the client runs the events the server starts (S2C 0x032 / 0x034 / zone-in), reverse-engineered from the unpacked
FFXiMain.dll on 2026-10-17. Tags: CODE = read in the disassembly, DATA = seen in game files, INFERRED = a reading
that is not proven.

xi-client runs events with `dat::EventRunner` (src/dat/event_runner.*; every participating actor's threads, §6) behind
`client::Cutscene` (src/client/cutscene.*), which moves, hides and animates the actors, runs the camera shots and fades,
and shows the dialog. `dat::EventVm` (src/dat/event.*) is the older single-thread dialog interpreter, kept for the
coverage check below.
- Messages go to the log, and Enter continues past each message window.
- Choice menus open as a list.
- Event updates (op 0x43) round-trip to the server.
- The end sends C2S 0x05B with the script's result.
- Look-at, costume and environment opcodes are skipped by their lengths (§6.5).

Checks:
- Tests in tests/test_zone.cpp: Leuveret, and the Pickpocket menu.
- Verified live: Leuveret, zone 230, event 621.

## 1. Files and lookup

- **Event file** (CODE 0x100aeab0), by EventNum (the zone):
  - zone < 256 → 5820 + zone;
  - 256..999 → 84735 + zone;
  - 1000..1999 → 56881 + zone;
  - from 2000 → 65611 + zone.

  Zone 230 is file 6050, and it is not a chunk file.
- **File:** `u32 N`, then `u32 size[N]`, then N blocks (CODE 0x100aedd0).
- **Block** (CODE 0x100bd0d0), one per actor:
  - `u32 id`
  - `u16 n` (padded to 4)
  - `u16 entryOff[n]`, `u16 entryEventId[n]`
  - `u16 m` (padded to 4)
  - `u32 const[m]`
  - `u32 codeLen`, `code[codeLen]`. Offsets are relative to the code.
- **Block ids:**
  - An NPC's block id is its UniqueNo.
  - The player's own events (zone-in, cutscenes; LSB sends the player's id) use block 0x7FFFFFF0.
  - 0x7FFFFFF1..F5 are party members.
- **Matching:** the client matches `entryEventId` against EventPara (the LSB event id). 0xFFFE is a wildcard and 0xFFFF
  means unused.
  - Every block with a match starts. Entry 0 runs first as an init thread.
  - xi-client runs only the speaking actor's matched entry.
- **Dialog text:** message ids come from the block's const table and index the zone dialog file (6420 + zone for
  English). They always match the installed client, unlike LSB's `IDs.lua` (see protocol-map, "Zone dialog ids").

## 2. Operands

A u16 "ref" (CODE 0x100af3f0 read, 0x100af260 write) is one of:

| Ref | Meaning |
|---|---|
| `0x8000 \| k` | const[k] |
| `0x0000..0x004F` | actor-local registers |
| `0x1000..0x105F` | event globals g0..g95 |
| `0x1100..`, `0x1700..` | other banks, purpose unknown |
| `0x7F00..0x7F0B` | this actor's fields (position ×1000, direction) |
| `0x7F80..0x7F8B` | the player's fields |

The globals:
- g0 = the menu choice;
- g1 = the result sent in 0x05B;
- g2..g9 = the event parameters (0x034 num[8], replaced by S2C 0x05C);
- the client resets g0, g1 and g16..g95 at event start.

Entity operands are raw u32, and jump targets are raw u16 code offsets.

## 3. Opcodes

Validated against every zone's event file: the length table decodes about 6.5 million instructions in 22,071 blocks.

| op | len | meaning | basis |
|---|---|---|---|
| 00 | 1 | end this thread | CODE 0x100afbb0 |
| 01 | 3 | goto u16 | CODE |
| 02 | 8 | `if (refA OP refB)` continue, else goto u16@+6. OP = byte@+5 & 0xF: 0 ==, 1 !=, 2 >, 3 <, 4 >=, 5 <=, 6/9 a&b, 7 !=, 8 a\|b, 10 b&~a | CODE 0x100afc40 |
| 03 | 5 | dst = src | CODE |
| 05 / 06 | 3 | dst = 1 / 0 | CODE |
| 07 08 0D 0E 0F 10 11 14 15 | 5 | += -= &= \|= ^= <<= >>= *= /= | CODE |
| 09 / 0A | 5 | set / clear bit src of dst | CODE |
| 0B / 0C | 3 | ++ / -- | CODE |
| 12 / 13 | 3 / 5 | random / random mod (src+1) | CODE / INFERRED |
| 19 | 5 | swap | CODE |
| 1A / 1B | 3 / 1 | gosub u16 (depth 8) / return (empty stack: end the thread) | CODE |
| 1C | 3 | wait ref frames | CODE |
| **1D** | 3 | message ref, the speaker is this actor | CODE 0x100b2030 |
| 1E | 5 | face entity u32. xi-client turns the NPC to the player (or the entity), overriding the server's heading until the zone changes; not yet visibly verified (Leuveret already faced the player) | CODE |
| **21** | 1 | end the event | CODE 0x100b3410 |
| **23** | 1 | wait for the message window to close | CODE 0x100b2d00 |
| **24** | 7 | menu: message ref, default ref @+3 (the option, counting hidden ones, the cursor starts on), hide-mask ref @+5 (bit i hides option i). Text = prompt, 0x07 0x0B, options separated by 0x07. The window is "menu query" (0x1015e9b0; handler object 0x1057845c, vtable 0x10335280, ctor 0x1014dc60): each prompt line replaces the last (0x1014de80 clears the options first), each shown option is a row (0x1014dec0), the default sets the cursor (0x1014e020). Drawn by 0x1014e150: the prompt at x + 4, y + 7, three rows in elements 1..3 (28,28 / 28,48 / 28,68), scrolling. Placed at the layout's (16, 208) bottom-anchored, which registry flag 0x1000 stacks 2 above the log window; 366 x 88. German / French clients widen it to the log window's width (at most 512) or the widest line + 8 (0x1014e230). Its cursor sound is off (+0x85 = 0, 0x1014dd99). The prompt is not written to the log | CODE 0x100b2520 |
| **25** | 1 | wait for the menu: g0 = option (counting hidden ones). Cancel → g0 = 0xFE, and the event aborts with EndPara 0x40000000 | CODE 0x100b2870 |
| 26 | 1 | halt | CODE |
| 2B / 49 | 7 | message with an entity u32: a named speaker / no name | CODE |
| 48 | 3 | message, no speaker | CODE 0x100b2130 |
| B0 | 12 | message ref @+10 | CODE |
| **43** | 2 | sub 0: send C2S 0x05B Mode 1 (update, EndPara = g1). Sub 1: wait for the server (S2C 0x052 mode 1 / 0x05C) | CODE 0x100b62d0 |
| 47 | 10 / 2 | sub 0: C2S 0x05C position update; sub 1: wait | CODE 0x100b61e0 |
| 3E | 7 | bit test: b = ref@+3; word = ref@+1 + (b >> 5); bit b & 31 set → continue, else goto u16@+5 | CODE 0x100b0180 |
| 44 / 82 | 5 / 7 | tests on game state (0x100b3590; 0x1007ac80/90), then a jump. xi-client falls through | CODE |
| 5B / 66 | 15 | play a 4-char motion ("tlk0") | CODE |
| 5E | 5 | motion on self | CODE |
| 6F / 70 | 1 | waits | CODE / INFERRED |

The lengths of every other opcode, including those whose length depends on the next byte, are in the `kLength`
table and in `EventVm::length` (src/dat/event.cpp). The handler addresses are in the RE notes.

## 4. Packets

- **S2C 0x032 / 0x034:** start the event. 0x034's num[8] at +8 become g2..g9 (handler 0x100a0200).
- **S2C 0x033 EVENTSTR:** start the event with strings: UniqueNo @4, ActIndex @8, EventNum @0xA, EventPara @0xC, Mode
  @0xE, String[4][16] @0x10, Data[8] @0x50 (= g2..g9). The handler (0x100a02f0) copies the four strings to the message
  strings (0x10488d08, 256 bytes apart, where 0x02A's string goes too) and to the saved names op B4 01 reads
  (0x104882b8). LSB sends it for `startEventString` (battlefield entry menus, chocobo raising, mog tablets, `!cs2`).
  xi-client: the strings go to the event's messages (dialog code 0x1C n) and the runner's saved names.
- **S2C 0x05D PENDINGSTR:** num[9] @4, String[4][16] @0x28: the handler (0x100a0040) keeps only the strings (same two
  copies); the update wait still ends with 0x05C / 0x052.
- **S2C 0x05C PENDINGNUM:** 8 u32 at +4 replace g2..g9 (the server's onEventUpdate).
- **S2C 0x052:** mode 1 clears the update-pending state; mode 2 cancels the event.
- **C2S 0x05B EVENTEND** `{UniqueNo, EndPara = g1, ActIndex, Mode (0 end, 1 update), EventNum, EventPara}`: sent at
  the end, or for an update.

## 5. Worked example: Leuveret (zone 230, `startEvent(621)`)

Block 0x010E6014; consts 0x4B, 0x1ECA, 0x1E, 0x1ECB, 0x1ECC. Entry 1 for event 621:
```
1e f0ffff7f                face the player
6f 70                      waits
5b ... "tlk0"              talk motion
1d 0180 23 1c 0280         message 7882, wait for the window, wait 30
1d 0380 23 1c 0280         message 7883 ...
1d 0480 23                 message 7884
5e "idl0" 1c 0280 21       idle, end
```
7882 is "Heh-heh! Like to know something truly stellar? …"

Menu example: The Pickpocket (0x010E6021), event 550. The script shows 8342, then menu 8343
("Well, did you?" / "Yes, I did." / "No, not yet."), then branches on g0, sets g1 and ends.

## Coverage (`xi-dat event ZONE`)

The check runs every entry of a zone headlessly: Enter at each wait, the first option at each menu, updates answered
at once.

| Zone | Events ended | Errors | Messages | Menus |
|---|---|---|---|---|
| 230 | 1511 | 2 | 8220 | 5444 |
| 231 | 1241 | 1 | 3905 | 3694 |
| 232 | 487 | 1 | 3204 | 2544 |
| 100 | 227 | 0 | 348 | 373 |
| 234 | 990 | 2 | 4885 | 4502 |
| 238 | 1544 | 0 | 4591 | 3382 |
| 245 | 1064 | 1 | 3589 | 2455 |

The errors hit the instruction budget or the gosub depth, not an unknown opcode. They are INFERRED to be polling loops
on state this interpreter does not model (frame waits are instant); the runner then finishes the event.

## 6. Cutscenes (CODE + DATA, live 2026-10-18; RE notes cutscene_re.md)

### 6.1 Threads
- **Participants:** every block of the zone's event file with an entry for EventPara (CODE 0x100aedd0; the game also
  takes 0xFFFE wildcards: xi-client only for the owner, INFERRED deviation). Block id = UniqueNo (0x7FFFFFF0 = the
  player); a missing entity is requested with **C2S 0x016** {ActIndex = id & 0xFFF} and the event waits for it (LSB
  sends cutscene-only NPCs this way).
- **Slots:** 16 per actor {priority, pc, entry, wait, move target}. Start (CODE 0x100bcd00): entry 0 at priority 0x80
  until it yields, then the event's entry at 0x10. **Each frame each actor runs only its lowest-priority slot until a
  handler yields** (CODE 0x100bcc40): actors run side by side, threads of one actor pre-empt each other.
- **Thread ops:** 0x27 {u8 prio, u32 ent, u8 entry} starts entry on ent (no-op if running; retry when full), 0x29 the
  same and waits for it (CODE 0x100b3e87, state in slot +0x3b: start result 1 -> wait; 2 full -> retry; 0, the entry
  already running -> carry on at once without waiting. The Moogle's event 30000 relies on it: entry 40 starts the
  running entry 30, which waits on 40), 0x2A {u8 prio, ent} waits until ent has no thread at priority <= prio, 0x2F {u8 b, ent}
  suspends / resumes ent, 0x00 ends the thread, 0x21 ends the event.
- **Entity operands:** 0x7FFFFFF0 / C0 / F9 the player, 0x7FFFFFF1..F5 / C1..D1 party (not run), 0x7FFFFFF8 or a
  high byte of 0 = this block's actor, else a UniqueNo (CODE 0x100afa30).

### 6.2 Time, positions, facing
- Ticks at 60 per second. Positions are refs / 1000 in operand order **x, z, y** (vertical last); direction 0..4095
  for a turn, yaw = atan2(-dz, dx) (0 = +x); LSB's heading byte = direction / 16. Walk speed 5.0 units/s, op 0x32 =
  ref / 10.

### 6.3 Opcodes run (lengths as §3)
| op | meaning |
|---|---|
| 1C ref / 6F | wait ref ticks / 16 ticks |
| 70 / 76 ent / 80 ent | wait while self / ent plays a one-shot motion; wait until ent exists |
| 37 x z y dir / 36 x z y / BA ent x z y dir | place self (with / without facing) / ent; on the ground |
| 4B ent dir / 4A ent ent / 1E ent | set facing / face another entity |
| 1F 00 x z y, 1F 01 | set a target, then walk there (ground), a step a frame, facing the way |
| 5A / 31 (+ timeout) | the same in 3-D |
| 4E b ent / 22 b | hide (1) / show (0) ent / self |
| 5B / 66 file ent ent fcc, 5F 03..06, 2C, 53 | one-shot motion; 53 waits for it. 5B: one DAT (0x7D68 + n ...). 66 (CODE 0x100d2150): n < 0x118, P = {0x7FC8, 0xEF39, 0x15711, 0x18F5F}[n / 70] + n % 70 for pieces 0 / 1, and by the body variant v (dat-format §10.8) Q = {0x800E, 0xEF7F, 0x15757, 0x18FA5}[..] + n % 70 (v 1) or {0x8054, 0xEFC5, 0x1579D, 0x18FEB}[..] (v >= 2) for piece 2. The race is the script's choice: each 70-file block is 7 groups of 10 (Hume m, Hume f, Elvaan m, Elvaan f, Tarutaru, Mithra, Galka; DATA). The fcc is a scheduler in that file whose op 0x05 motions (`ta1?` / `ta2?` ...) load from P / Q (DATA 32712 `tlk0`) |
| 5E fcc | idle motion (not drawn yet) |
| 45 file caster target fcc ref / 52 / 55 | start / stop / wait for scheduler fcc of DAT 0x77F0 + f(n) (f: n < 300 n, < 600 n + 0x6551, < 900 n + 0x9ADB, else n + 0x12FE5); siblings 62/9F/BB/C5/CD/D0/D5 use bases 0x1394, 0xC7EF, 0xDD6D, 0x1071B, 0x11323, 0x11423, 0x19031 |
| 46 01 / 00 | enter / leave the cutscene camera |
| 5C k ref / 5D ref | music slot k = track, replacing the zone's slot until the event ends (the intro: 107 in slots 0 and 1) / sound effect |

### 6.4 Camera and fades (effect schedulers)
- A shot is a scheduler (e.g. `s043`, DAT 30834) whose **op 0x04** {+8 name} runs the **Camera chunk (type 6)** of that
  name for the record's duration: payload +0x10 u32 flags (low byte = key count; bits 19 / 20 add the current / follow
  camera, not done), keys of 0x30 bytes from +0x20 {f32 eye x, y, z; zoom; look-at x, y, z; roll; t 0..1} (DATA `c043`:
  2 keys). One key = a still camera, two = linear, more = a spline (xi-client: Catmull-Rom, INFERRED). The pose holds
  after the task ends. Zoom 350 = the normal view (xi-client: tan(fov/2) x 350 / zoom, INFERRED).
- A fade is a scheduler (`fdi1` / `fdo1`, DAT 30904) with **op 0x0F** {+8 R, G, B, A bytes, 0x80 = 1}: the screen colour
  moves there over the duration (fdo1 = black, fdi1 = clear).

### 6.5 Live (zone 230, `!cs 503`, the San d'Oria intro)
The narration over the city gate and street shots, the eight cutscene-only guards marching down the gatehouse at 1.3
units/s, fades between shots; `xi-dat cutscene 230 503` runs it headless to the end (44 messages, 52 schedulers).
Narration lines start with `02 50 00 03 54 01` (codes 0x02 / 0x03 with two parameter bytes each; formatDialog skips them).

### 6.6 Head look-at (0x79 / 0x7B; CODE 0x100b8740, 0x10087e30, 0x100d5a40, 0x1002ab90)
| op | len | effect |
|---|---|---|
| `79 00 ent ent` | 10 | ent1 looks at ent2's skeleton point 6 (the face) |
| `79 01 ent ent ref` | 12 | ... at point `ref` (e.g. 10 = a hand: inspecting something held) |
| `79 02 ent ref ref` | 10 | at fixed angles (pitch, yaw offset; 4096 = 2 pi): the point 10 units out from the feet |
| `7B ent` | 5 | stop: the actor's own look-at takes over again |
`1E` / `4A` (turn to an entity) also start `79 00`. "79 00 X X" (its own face, nearer than 0.3) fades back to neutral.
Outside events the same solver aims an actor's head at the head (point 3) of what it faces while standing or sitting:
the player's current target, else the 0x00D / 0x00E facetarget index (+0x1A >> 1; LSB sets it on an NPC that turns
to speak to the player).

Solver: head (skeleton point 3's bone) then neck (point 7's), on top of the motion; each joint's limits are 3 floats
`{x, y, z}` at `points table + 4 + 26 * count + 0x48 + 12 i` (head {0.24, 0.16, 0.5}, neck {0.16, 0.06, 0.5}: up to 25.6
/ 17.7 degrees across and 17.7 / 6.8 up). The direction is projected on the plane at z and pulled onto the ellipse (x, y);
the neck gets what the head could not reach. The look direction moves 1/32 of the way per 60 Hz tick; the weight goes
up / down 0.04 per frame (30 fps), down when there is no target, it is behind (local x < -0.5), turned away (cos <
-0.1) or nearer than 0.3 across. Sitting / mounted: the head only. Implementation: `gfx::applyLookAt`; `XI_LOOK_TRACE=1`
logs weight and target.

### 6.7 Costume (0xB6; CODE 0x100b1080)
Every form ends the thread's frame. SELF = the block's entity; `16` / `17` / `10` act on the player. Values are raw look
words as in S2C 0x00D (gear masked with 0xFFF).
| sub | len | effect |
|---|---|---|
| 00 | 4 | face / single-model id |
| 01 | 4 | race |
| 02..0A | 4 | one slot: face, head, body, hands, legs, feet, main, sub, ranged |
| 0B | 20 | race + face..sub (ranged untouched) |
| 0C | 4 | become a single-model id |
| 0D / 0E | 14 / 16 | chocobo looks (§6.8) |
| 0F / 17 | 4 | scale = ref / 100 (self / player) |
| 10 | 2 | wait until the player is there |
| 11..15 | 4 / 2 / 6 | alternate model file; targeting bit (`+0x128` bit 4): advanced only |
A costume outlasts the event: nothing restores it but a new look from the server (or the script itself, e.g. the
Ancestry Moogle's race preview, zone 232 ev 832). Live: zone 230 `!cs 988` (the flower-bed quest) re-models six
sprouts with `b6 00 0x12c`.

### 6.8 The other cutscene ops (CODE, handler table 0x100bc890; 2026-10-18)
`xi-dat cutscene ZONE EVENT` prints `skipped op` (unknown) and `state-only op` lines; `xi-dat cutscene-ids ZONE` lists
the player block's events. Of 1,100 player events in zones 230-246, only `b4` (7), `ab` (2) and single 59 / 7a / 93 /
9c / a6 / ae / b5 / b7 are still unknown. These 15 run with none: the nation intros 230/503, 231/535, 234/1, 236/1,
238/11, 240/11, 241/367, and 234/502, 238/906, 243/10070, 230/0, 237/782, 232/4, 230/23, 245/86 (test_zone.cpp).

| op | len | effect | handler |
|---|---|---|---|
| 16 / 17 | 7 | dst = trunc(-r sin a) / trunc(r cos a); dst, a (4096ths), r | 0x100b0530 / 0x100b0590 |
| 3A | 7 | dst@5 = ent@1's facing, 0..4095 | 0x100b2bb0 |
| 3B | 11 | x, z, y (vertical) @5/7/9 = ent@1's position x 1000 | 0x100b2ac0 |
| 3C / 3D | 7 | set / clear bit b@3 of the word array at dst@1 (only if b >> 5 < count@5); 3E tests | 0x100b01d0 / 0x100b0220 |
| 40 / 41 | 9 | bits lo@1..hi@3: insert src@7 into dst@5 / extract src@5 into dst@7 | 0x100b6110 / 0x100b6180 |
| 33 | 2 | u8: entity +0x120 bit 21 = moves keep their height (no ground snap, as bit 20 / +0x128 bit 14) | 0x100b5970, test 0x100b3082 |
| 39 | 3 | self facing = ref x 2 pi / 4096 | 0x100b6070 |
| 6B | 9 | ent's idle motion = fcc (5E for another actor) | 0x100b6fa0 |
| 4C / 4D / 8E / 8F / 4F | 1 / 1 / 1 / 1 / 3 | self animation (+0x174) = 8 / 9 / 45 / 46 / ref + 18: doors open / close | 0x100b6880.. |
| 6C | 9 | ent's model alpha to ref@5 / 0x80 over ref@7 ticks, waiting (vtbl +0x2C / +0x30 colour). xi-client: a linear dissolve (INFERRED: the curve); 0 = hidden | 0x100b7560 |
| 6E | 7 | ent emote (ref & 0xFF; mode = next byte; 0x100a0ac0, ids < 97) once its last one finished | 0x100b7b70 |
| 99 | 5 | wait while ent's emote plays (+0x1BC) | 0x100b7c10 |
| 56 | 5 | resolve ent, yield a frame | 0x100b65f0 |
| 28 | 7 | as 27 on another player's own block (+0x120 bit 7); NPCs nothing. INFERRED: the player only | 0x100b3d80 |
| 69 | 4 | u8 off, ref mask: sound channels at once (1 = 3-D effect sounds 0x10351714, 2 = sound effects 0x10351710, 4 = ambience 0x10351718, 8 = all; 0x10 a flag 0x10351720) | 0x100b7990 |
| 6A | 7 | level@1 / 1000, ticks@3, mask@5: fade those channels (0x10035930 tasks) | 0x100b7a20 |
| 77 | 5 | hour@1 (keeps the minutes; 0xFF keep), weather@3 (0xFF keep), clock frozen (0x1018c400) | 0x100b8640 |
| 78 / C9 | 1 | real clock and weather again / the clock only | 0x100b86e0 / 0x100b8710 |
| 75 | 4 / 2 / 2 | 00 ref: load sub-map ref (0x10178810: n <= 0 does nothing; the model file n + 100, from 600 n + 83191, as a zone model id, dat::zoneModelFileId; it sets +0x38 and keeps the sub-map at +0xD8, unloading an earlier one, 0x101789d0). 01: next. 02: C2S 0x0F2 {State 2}. All wait while a sub-map loads. DATA: Northern San d'Oria events 7 / 8 / 50 (361), 36 (439), 37 (360), 38 (359, 360), 62 / 65 (359). xi-client: loaded as a second zone scene and drawn (and walked by the event's actors) with the zone (it adds to it: zone 233 sub-map 374 is furniture inside the chateau) until the event ends (INFERRED: that it with the zone (it adds to it: zone 233 sub-map 374 is furniture inside the chateau) rather than adds to it) | 0x100b8590 |
| 47 | 10 / 2 | 00 x, y (vertical), z, dir: C2S 0x05C mode 1 (EndPara = g1); 01 waits for the server | 0x100b61e0, 0x100a0510 |
| 7D | 3 | scheduler `main` of DAT 0x13F8 + ref on the player | 0x100b8a50 |
| C8 / 8B / 8A | 7 / 25 / 1 | the map window ("menu mapframe", 0x10630d68): zone@1, map@3; 8B adds a marker at x@5, z@7 / 1000 named by the 16 bytes @9 ('_' = space); 8A closes. xi-client: the map image centred with the named markers (the frame is not drawn); the event's end closes it | 0x100b92a0 / 0x100b93c0 / 0x100b92f0 |
| 9D sub | 8 / 10 (0A..10), 6 / 8 (02 / 0C), 23 (08), 9 (09) | tables and strings (CODE 0x100ba0c0). T = the code offset @2; idx @6, and for 0A..10 reset to 0 when >= the bound @8. 00 / 0A: @4 = value of the ref at T + idx x 2. 01 / 0B: string @4 = the 16 bytes at T + idx x 16. 02 / 0C: pointer slot idx (@4, bound @6, < 64) = T in this block. 03 / 0D and 04 / 0E: through pointer slot @2, a ref's value / a string at idx -> @4. 05 / 0F: the ref at T + idx x 2 = @4. 06 / 10: string @4 -> the ref at T + idx x 16. 07: gosub to the u16 at T + n@4 x 2 (return at +6). 08: string @2 vs the inline 16 bytes @5, mode @4 (0 equal / 1 different) -> next (+0x17), else jump to @0x15. 09: strings @2 / @4, mode @6, +9 or jump @7. A string is 4 work slots (locals < 0x800, globals 0x1000..), the 16th byte cleared; constants read as empty | 0x100ba0c0 |
| AB sub | 2 / 4 / 6 | self state (CODE 0x100babc0): 01..08, 0B..0E, 12 / 13, 19..1C entity flag bits (+0x120 bits 1, 2, 3, 6; +0x128 bits 1, 24; +0x130 bit 1; +0x13C bit 19); 04 waits while self's emote plays; 09 / 0A 0x10087050(1 / 0); 0F / 10 flag 0x10489900; 11 0x1035af4c = @2; 14..18 entity setters 0x1008a9b0.. with @2. xi-client: state only (04 waits) | 0x100babc0 |
| B4 sub | 20 / 6 / 4 / 2 / 12 | interface (CODE 0x100b0930; @2 is a ref, 0x100af710 / 0x100af890 write / read a 16-byte string through it, 0x100af3e0 reads a value). 00 / 13 (20): string @2 = the 16 bytes @4 (13: '@' = space). 01 / 04 (6): string @2 = / -> saved name n@4 (0..3, 0x104882b8 + n x 256). 02 (6): the target window ("menu targetwi", object 0x10578478) in event mode (+0x65 = 1, colour +0x68 0x80808080) named by string @2, gauge +0x66 = @4 when 0..100. 03 (2) closes it. 07 / 0A (4): "menu casttime" (0x1057817c, 0x1012c100: mode +0x20 = 4) / "menu eventtim" (0x10578180, 0x1012c430: mode 0) with total +0x14 = left +0x18 = @2 ticks (1/60 s, < 0 -> 0). 08 / 0B (2) close them; 09 / 0C (4) ref @2 = the ticks left. Both draw only the time, "%d:%02d" (from an hour "%d:%02d:%02d", 0x101c8f70) at y + 9: casttime mode 4 right-aligned at x + 0x69 (0x1012bf10), eventtim centred (0x1012c200); both move to x 16, y 94 while the gauge window (0x105781ec) is up, else 47. Each frame (casttime 0x1012c020, eventtim 0x1012c340) a window whose time is already 0 closes, else the time drops by the frame's ticks (0x10014cb0: [0x104568fc]+0x28, at least 1) and stops at 0; casttime also keeps (total - left) / total at +0x1C for its gauge modes 0..3 (the spell cast bar, 0x1012bf60 / 0x1012bf90). 05 / 06 flags, 0D / 0E flag 0x1047bc8c, 0F..12 two more windows. 14 (12): the map window's view (0x101fac20): @2 / @4 / @6 / @8 / 1000 = x, y, zoom, seconds (0 = at once), flags @10 (bit 7 refreshes). 15 (2): waits while the map window is open. DATA: zone 233 event 597 (view (80, -80) x1.5, then (0, 0) x1 over 0.3 s), 233 / 604, 234 / 673, 239 / 570, 252 / 276; static scan of every zone's event blocks (`xi-dat event-ops ZONE b4 SUB`): 02 only in the three chocobo trainers' blocks (Hantileon 230 / 0x10e6112, Zopago 234 / 0x10ea090, Pulonono 241 / 0x10f1043: their main event 823 / 509 / 742, "Care for your chocobo." shows the chocobo's name = saved name 0 with a gauge g3); 07 (the cast-time window) in no event; 0A in 17 blocks, among them Estiliphire's "Spot the Difference" (230, event 897 on 0x10e61fa: its clock thread on 0x10e61fb counts 3600 ticks), Ohruru's rune quiz (240 / 889 on 0x10f00f8, 600), Lilisette (136 / 12, 3600), Well-Kept Cache (263 / 128, 300). LSB scripts none of these (chocobo raising is off on this server), so they were seen with the local test hook (`localevent` script step). xi-client: all of these (GameUi::eventTarget / eventTimer / eventMapView; the timers step by the frame's clock time without the 1-tick floor; the view zooms about the window's centre and shifts by (x, y) map pixels, INFERRED) | 0x100b0930 |
| C3 | 7 | name record val@1 (0..7; else nothing) = the 16-byte string at ref @3 and the value @5 (0x10488210 + n x 20: name[16], u32). Dialog `01 05 84 82 <0x80+k> 80 80` prints record num[k]'s name, `01 05 85 ...` makes the next "[a/b]" pick by the record's value byte (flags +0x6EC / +0x6ED; CODE 0x100bdb80 cases 3 / 4, called from the text formatter 0x100c1606 for 01 xx T with T >= 0x81). DATA: the chocobo trainers (Hantileon 230 / 823) set record 0 = the chocobo's name; 11193 "Let us analyze <name>'s current condition...", 11195 "...a [male/female]!" | 0x100bbf30 |
| B6 0D / 0E | 14 / 16 | chocobo look: race = 32 + @4, body @6, head @8, feet @10, hands (tail) @12, legs @14 (0E); drawn as a ridden chocobo (kind 7) | 0x100b1080 |

State only (decoded, no client effect): 20 (u8 -> 0x10482f18), 2E / 42 (event message mode on / off: 0x104886b8,
0x10485a70; changes the message window's sounds), 30, 38 (ref | 0x2000 -> event flags 0x104808fc), 34 / 35 (ref: the
area's environment set, 0x1018b2b0, over three frames), 67 / 68 (move the log windows "rem4line" / "rem4li2" to x, y
(negative = percent from the right / bottom) and back), 61 / 84 / 7C / 81 / 86 / 92 / 94 / A4 / A5 (entity flag bits
+0x128 bit 0 / +0x12C bit 0 / +0x128 bit 17 / model +0x8B4 / +0x12C bits 3, 16, 17, 26, 11; meanings open), 95 / 96
(self model refresh), 97 (world +0x138 / +0x13C, a random interval 4 + rand % 10 by default; restored at the end), 9A
(waits for the music to load), 9E.

## Open

- `b1 04` loops and some out-of-range jumps in a few blocks.
- The default-cursor operand of 0x24.
- The 0x1100 / 0x1700 banks.
- Cutscenes: some motions still not found (`sl00`, `th1`), the targeting bit (B6 12-15), the sign of 79 02's pitch,
  camera flag bits 19 / 20, the spline kind and zoom-to-FOV formula. The meaning of the entity flag bits in §6.8
  (AB included); the event map window's frame: the map object (vtable 0x1033a8b8, draw 0x101fa550) draws its image
  (0x101ee780: the sprite at +0x90, then +0x8C, in 0xa0404040, placed by the view +0x24 / +0x28 and zoom +0x58), the
  markers (0x101fa6d0) and the grid square "(%c-%d)" into the title window; the frame sprite and the exact view anchor
  are not drawn / traced. Under the event camera (46 01 sets 0x10351230, CODE 0x10084140, which only sets up the
  camera) names and target arrows are hidden (INFERRED: the name pass was not traced to the flag); a retail capture of
  a cutscene close-up keeps the party window and the network meter, so the rest of the HUD stays.
