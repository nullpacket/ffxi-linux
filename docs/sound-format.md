# Sound formats

Implemented in `src/dat/sound.*` (decoding) and `src/audio/mixer.*` (SDL3 output). The full RE notes for this
file, with code addresses, came from a reading of FFXiMain.dll (CODE, see the addresses below), and were checked
against the shipped files (DATA).

## Effect sounds: Sep chunks (type 0x3D)

Effect schedulers start sounds with op 0x0A (at the caster) and op 0x0B (at the target), naming a `Sep` chunk. The
chunk comes from the scheduler's own file first, then the global files.

- A Sep payload is `SeSep  \0`, then +8 u32 id, +0xC u32 size, +0x10 u32 = 2, then a byte program.
- **The PC client never runs the program.** It reads only the id at +8 and plays `se%03u/se%06u.spw` for that id
  (CODE 0x10245b90 → 0x1024a6e0 → 0x10249960). Every Sep in file 0 (5001–5050) has a matching se005/se005NNN.spw
  (DATA). The program is probably a leftover PS2 sequence (INFERRED).
- The zone model files (zone + 100) hold about 800 Sep chunks each (DATA).
- **Positional playback** (CODE 0x10037000, tick 0x10036410, maths 0x10036ba0):
  - The emitter's near and far distances are the effect record's +0x18 and +0x14. When 0, near = 3.0 and far = 30.0.
  - Volume is full inside near and falls linearly to 0 at far; beyond far the sound stops.
  - The vertical difference counts ×3 in the distance.
  - Pan = 64 + clamp(k · x / max(|z|, 1) / width, −1, 1) · 64, in view space; width defaults to 512, and k is a camera
    value that is not yet identified.
  - Volume and pan run 0..127 (pan 64 = centre). A 128-entry table at 0x103889f0 maps volume to DirectSound millibels.
- At most 20 sounds play at once in one mode (the cap is variable 0x10456a94).

xi-client plays each Sep's wave at the actor, with the linear falloff and the ×3 vertical distance. Its pan is
clamp(x / max(|z|, 1)) · 0.8: the scale is INFERRED, since k is unknown.

## Waves: sound/win/se/seNNN/seNNNMMM.spw

The header is 0x30 bytes (CODE 0x1024a300; all 9,447 files, DATA):

| off | type | meaning |
|---|---|---|
| 0x00 | char[6] | `SeWave` |
| 0x08 | u32 | file size |
| 0x0C | u32 | format: 0 = PS ADPCM, 1 = PCM s16le, 2 = PCM s16 big-endian, 3 = ATRAC3 (scrambled) |
| 0x10 | u32 | id = the file number |
| 0x14 | u32 | length: blocks for format 0, sample frames otherwise |
| 0x18 | s32 | loop start in the same unit; −1 = one-shot |
| 0x1C, 0x20 | u32, u32 | sample rate = (u32@0x1C + u32@0x20) & 0x7FFFFFFF. It is 48000 in every file. u32@0x20 also seeds format 3's key |
| 0x24 | u32 | header size, 0x30 |
| 0x28 | u8 | priority index (used with flag 0x80) |
| 0x29 | u8 | flags (0x20 = fade in over 150 ticks; 0x80 = priority from the table) |
| 0x2A | u8 | channels |
| 0x2B | u8 | output bits (16) |
| 0x2C | u8 | ADPCM samples per block: 16 normally, 4 and 8 occur. A block is 0x2C / 2 + 1 bytes |

**ADPCM** (CODE 0x10246610) is exactly PS ADPCM (VAG):
- The block's first byte is filter << 4 | shift. The coefficients / 64 are (0,0), (60,0), (115,−52), (98,−55),
  (122,−60) (table 0x10388bf0).
- Within a data byte the low nibble comes first.
- sample = ((c0·s1 + c1·s2) >> 6) + ((s16)(nibble << 12) >> shift).
- There is no clamping: the output is truncated to s16 and the history keeps the full int.
- Stereo is an L block followed by an R block.

**Format 3** is ATRAC3 under an XOR scramble.
- Key: s = rotl32(u32@0x20, 11), then 100 dwords: key[i] = s; s += rotl32(s, 3). Each frame is XORed dword by dword
  from key[0]; the first byte then reads 0xA0, the ATRAC3 sync byte.
- Stereo frames are 384 bytes. Mono frames are 192 bytes, duplicated to 384 and decoded as stereo, keeping the left
  channel. Each frame is 1024 samples.
- xi-client decodes them with FFmpeg's `atrac3`: block_align 384, two channels, WAV-style extradata with **coding mode
  0** (independent channels). Joint stereo (mode 1) fails on every file tried. Checked: se000041 gives exactly its
  61,440 frames and music052 its 4,028,416 (DATA). That is 528 of the 4,685 effect waves (4,002 ADPCM, 155 PCM).

**Streaming** (CODE 0x10247c20): a 0x30000-byte ring of 16-bit PCM, refilled in 0xC000-byte chunks
(decoder dispatch 0x10246d90). At the end, playback jumps back to the loop start.

## Music: sound/win/music/data/musicNNN.bgw

`BGMStream` header: the SeWave fields with the layout shifted (DATA, 10 files checked, sizes match exactly):

| off | meaning |
|---|---|
| 0x0C | format (0 ADPCM, 3 ATRAC3, decoded through FFmpeg) |
| 0x10 | file size |
| 0x14 | id |
| 0x18 | length in blocks |
| 0x1C | loop start (blocks) |
| 0x20, 0x24 | rate = sum & 0x7FFFFFFF (44100 or 48000) |
| 0x28 | header size 0x30 |
| 0x2E | channels (2) |
| 0x2F | samples per block (16, 64, 128) |

The ids come from S2C 0x00A +0x56 `MusicNum[5]` (day, night, solo battle, party battle, mount; LSB) and S2C 0x05F
(slot u16 @4, track u16 @6). xi-client plays the battle track while engaged (the party track in a party of 2+),
otherwise day (06:00-18:00, INFERRED) or night, looping from the loop start and cross-fading over about 1 s.
Verified live: West Ronfaure plays music109.

## Lead: zone sound sets (footsteps?)

Each zone model file (zone + 100) holds about 800 Sep chunks. West Ronfaure's file 200 has 805 (DATA):
- the ids are 100001 upwards, i.e. se100/se100NNN.spw;
- 776 names start with `0`;
- the second character is one of 1, 2, 5, 6, 7, 8, 9, a, about 100 each: eight values, possibly the races and genders;
- the last two characters are 58 patterns: a digit 1..3 or more, then a variant 1..4 (`11`, `12`, `13`, `21` ...).

INFERRED: footsteps by race × ground material × step variant. Not wired. To do this, find what the client plays them
from (motion events or a walk timer) and where the floor material comes from (the collision mesh?).

## Zone ambience (sound particles)

Zone generators whose particle resource is type 0x3d play a Sep at each particle's spawn point (DATA: West Ronfaure,
DAT 200, has 212 flagged ones). They are started with the other zone effects (effect-format §Z). Live: about 3 a
second in West Ronfaure, mostly 2124 / 2159 / 2146, which are one-shots of 0.8-1.6 s. The Sep is looked up in the
zone file's own Sep chunks.

The audible range is near 10 / far 50 (INFERRED: no record sets it for particles). Looping waves play once (2024 is a
9.4 s loop), because the voice is not tied to the particle's life.

## Interface sounds and footsteps (CODE, from the session report steps_re.md)

**Interface** sounds are file 0 Seps `00NN` → se000/se0000NN.spw, played centred:

| Event | Id |
|---|---|
| cursor move | 1 (CODE 0x1011828d) |
| confirm | 2 |
| cancel | 3 |
| error / disabled item | 4 |
| menu opened from nothing; also a first target (CODE 0x10157b2d) | 9 |
| switch window; also a change of target (0x10157b26) | 10 |
| main menu; also the battle menu "menu playermo" (0x10158f08) | 11 |
| target cleared with Escape (0x10157e44) | 13 |
| tell received | 40 |
| `<callN>` | 17 + N |
| alarm | from table 0x1032a8e4 |

xi-client plays 1/2/3/4 and 9/10/11 from the menus (`GameUi::setSoundHook`; 4 when confirming a dimmed entry), 9 / 10
/ 13 on targeting, and 40 on a tell. Each Sep `00NN` names wave NN at +8 (checked for every one in file 0), and each
helper (0x10037a60 "0001", 0x10037ac0 "0002", ...) plays it at 0x40 x the SE volume. Every window carries three switches
(+0x84 cancel 0003, +0x85 cursor 0001, +0x86 confirm 0002 / 0004 when dimmed; CODE 0x101182d3, 0x1011833f, 0x1011836e),
all on by default (0x101172cd); the battle menu turns its cancel sound off (0x1012ad72). `xi-dat se-wav ID OUT.wav`
writes any se file as WAV for listening.

**`<callN>`** (CODE 0x101278c0, checked on party lines only; RE notes call_indoor_re.md): the text stays literal and
visible; the first tag of the line in the table 0x1036ebb0 (`<call>` / `<callN>` / `<ncallN>` / `<scallN>`, N 0..21,
case-insensitive) plays file 0 Sep 17 + N centred at full volume; while one plays (120 ticks) new calls are dropped.
`ncall` / `scall` differ only in the gamepad rumble. xi-client: done, not checked live (needs a party).

**Footsteps** (per-frame 0x100d4af0; done, live in West Ronfaure):
- **Who:** every actor within 30 of the listener.
- **Trigger:** a foot comes down while moving (bones 9 / 8 in the code, and 0xb / 0xa for quadrupeds). These
  numbers are skeleton **attach points** (effect-format §C.6), not bone indices: Galka's bones 8/9 are leg joints
  (bottoming at 0.30 / 0.14 above the root, bone 8 twice a cycle), while points 9 / 8 alternate and reach 0.08-0.11.
  The game steps when the point crosses root level (y-down < 0 → ≥ 0). Ours stops just short, so xi-client steps at
  the bottom of each swing: after a lift of 0.12 over the running low, the first frame it rises again, within 0.08 of
  the low, at least 0.2 s after the last step (INFERRED equivalent).
- **Name:** `'0'`, `'0'` + floor material (collision triangle, 1 path, 2 grass, 3 sand, 4 snow, 5 stone, 6 metal,
  7 wood, 8 shallow and 9 deep water), the shoe type in hex, and `'1'` + variant.
- **Footwear:** the client's resource object puts the chunk payload at +0x30, so its +0x31 / +0x32 are payload bytes
  1 / 2. Feet gear: the 16-byte `info` chunk (type 0x45; DATA: a Galka feet file holds `ff 02 02 ff ...`, shoe 2,
  variant 2). Defaults: the skeleton chunk's payload bytes 0 (foot layout, 2 = quadruped), 1 and 2, used only while
  unset. A shoe below 1 is silent.
- **Lookup:** looked up in the zone directory chunks `fses` (dry) or `fser` (rain, squall or thunder; the code also
  checks a not-under-a-roof flag, not done), then the same name with material 2, then 0.DAT `foot` (12).
  Live: grass + Galka boots → `0223` → se108006.
- **Playback:** positional at the actor, near 3 / far 30; 80% for an actor in its walk motion (speed < 3.5).

## Zone ambience loops (CODE + DATA, live 2026-10-18; RE notes ambience_re.md)

- The zone file's `weat/<weather>` block (else `suny`) holds Seps named HHMM: a name counts when it is 4 digits, the
  first '0' or '1' and the third <= '5' (`2xxx` names are generator sounds, played by the zone particles). The one
  with the latest start before the Vana'diel time of day plays (none yet today: the last in file order; CODE
  0x10189bb0). One voice, centred, full volume, looping from the wave's loop start; a change replaces it (the waves
  fade in by their header flag 0x20). The SeSep program is not run on PC.
- Indoors the `indo/*` variant plays instead. The flag is the roofed bit (placement flags +0xA4 bit 2, dat-format §7.4)
  of the first collision hit of a ray from the player down to y + 100, else up to y - 15 (CODE 0x1001444a; call
  site of the segment query that sets it). xi-client asks the collision floor under the player. Live: zone 230 at
  (-30, -65) plays se001082 indoors, the open street se001081.
- DATA: West Ronfaure 0600 = se001005 (06:00-17:59), 1800 = se001007 (18:00-05:59), indoors 1056; Jugner's rain 0000 =
  1042 (indoors 1045), thunder 1096 (1097). Live: 1005 at 10:00 and 1007 at 21:00 in West Ronfaure.
