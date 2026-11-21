# FFXI visual effects: Generater and KeyFrame chunks (client RE notes)

> **Implementation (2026-10-17).**
> - `dat::EffectFile` holds the block tree and looks names up as in §S.2 (own block, parents, then file 0).
> - `client::EffectSystem` runs schedulers and generators at 60 ticks per second.
> - `gfx::EffectRenderer` draws two passes, alpha then additive.
> - A scheduler op 2 record's duration of 0 becomes 1 tick, so the generator emits a single burst (§G.2 "+1"). Hit
>   sparks then last about one second, as in the game.
> - Attach points are approximated: 0x31–0x33 = 0.6 × height toward the other actor, others = mid-body.

Scope: the effect "Generater" chunk (type 0x05) and the "KeyFrame" chunk (type 0x19) that
weapon-skill, spell, ability and monster-TP effect files contain, and the runtime that turns them
into particles. Chunk container basics (16-byte header, type/size encoding, Rmp blocks) are in
`dat-format.md` §1-2; the scheduler that starts generators is §10.8 there and the "Scheduler ops"
section of this file. Meshes (D3m/D3a/D3s) are covered in the "Meshes" section.

Binary: `FFXiMain.unpacked.dll`, image base 0x10000000. Evidence tags:
- **CODE** — read from disassembly/decompilation at the given address.
- **DATA** — measured on game files. The survey set is file ids 2800-5399, 37836-37899 and
  49391-49499 (2,327 files present): **34,267 Generater chunks and 34,057 KeyFrame chunks**.
- **INFERRED** — reasoning from the above; not directly confirmed.

Conventions used below: `rec+N` is a byte offset inside a bytecode record; `chunk+N` is a byte
offset from the start of the chunk (header included, so payload starts at chunk+0x10); `gen+N`
is an offset in the runtime generator object; `p+N` is an offset in a runtime particle object.
`dt` is the per-frame time step read from `[g_1047bfa8]+0xeb0` (CODE `0x1004e300`), in
scheduler ticks (INFERRED: 1/60 s; the constant 1/60 at 0x1032b160 converts it in oscillator
ops). All lifetimes, intervals and durations below are in those ticks.

---------------------------------------------------------------------------------------------

## Generators (chunk type 0x05, "Generater")

### G.1 What a generator is

A generator is an **emitter template**. When a scheduler record op 2 names it, the client
makes the generator live (CODE: scheduler op 2 handler `0x10059811..0x10059960`), and every frame
the generator may spawn **particles**. A particle is one instance of a resource named in the
generator — normally a D3m effect mesh (81% of generators) or a D3a animated mesh — or a sound
(Sep) or another generator. Each particle has its own position, rotation, scale, colour,
lifetime and a small variable block. Bytecode in the generator chunk initialises each particle
at spawn time and updates it every frame.

The chunk holds four bytecode lists:

| list | header pointer | runs | interpreter | role |
|---|---|---|---|---|
| L0 | chunk+0x80 | once per generator per frame | `Gen_Tick` 0x10049620 | animate the generator itself (emission rate, spawn-shape parameters, generator rotation, follow/attach) |
| L1 | chunk+0x84 | once per spawned particle | `Gen_Spawn` 0x1004e800 | create the particle (record op 1) and set its initial state |
| L2 | chunk+0x88 | once per particle per frame | `Gen_ParticleUpdate` 0x1004a8b0 | integrate velocities, apply keyframe curves, life |
| L3 | chunk+0x8c | when a particle's life runs out | `Gen_ParticleExpire` 0x1004e650 | chain: start another generator or scheduler, rebirth |

CODE: the list pointers are rebased at init `0x10047c10` (`gen+0xe0..0xec += gen+0x60`); each
interpreter walks its list starting at the matching pointer.

### G.2 Chunk layout

Load: vtable slot 7 `0x10047b60` (CODE). It does **not** copy the whole chunk. It copies only:

| chunk offset | size | copied to | meaning | basis |
|---|---|---|---|---|
| 0x00 | 16 | gen+0x20 | chunk header (name = generator name, e.g. `g000`) | CODE 0x10047b78 |
| 0x10 | u32 | gen+0x34 | **origin word** (§G.4) | CODE 0x10047b93; DATA |
| 0x14..0x1f | 12 | gen+0x38..0x43 | always zero | DATA (34,267/34,267) |
| 0x20 | f32 | gen+0x70 | actor-size scale weight A (used when origin bits 20-23 ≠ 0) | CODE 0x10047be2, 0x10053d90 |
| 0x24 | f32 | gen+0x74 | actor-size scale weight B (used when origin byte 3 low nibble ≠ 0) | CODE 0x10047be7, 0x10053d90 |
| 0x28 | u32 | — | not read by the client; 0 in 34,045 chunks | CODE (not copied); DATA |
| 0x2c | u32 | gen+0x98 | 1 = "an L1 op 0x7b attach-offset record exists" (the client then caches a pointer to it); 0 otherwise | CODE 0x10047bed, 0x10053d90, 0x10047e49 (`FUN_10049220(0x7b)`); DATA 42 chunks = 1 |
| 0x30..0x5f | 48 | — | **not read** (holds editor values such as 1.0 triplets) | CODE (not copied) |
| 0x60..end | rest | gen+0xc0.. | runtime block + bytecode; chunk+N lands at gen+0x60+N | CODE rep movs at 0x10047bcf |

Runtime block (chunk offsets; the client copies them, then overwrites most at run time):

| chunk offset | gen offset | type | meaning | basis |
|---|---|---|---|---|
| 0x60..0x6f | 0xc0..0xcf | — | stale editor pointer at 0x60; runtime: gen+0xc8 = elapsed ticks, gen+0xcc = emission countdown | CODE 0x1004a4c5..0x1004a6e9; DATA |
| 0x70 | 0xd0 | f32 | emission duration; always 0 in files; **set from the scheduler op 2 record's duration** (s16 at record+6, +1 if the previous value was 0) | CODE 0x100578ba..0x100578ce; DATA 34,267/34,267 = 0 |
| 0x74 | 0xd4 | s16 | random extra interval, added as `rand_u(n)` | CODE 0x1004a60a |
| 0x76 | 0xd6 | s16 | emission interval in ticks | CODE 0x1004a617 |
| 0x78 | 0xd8 | u32 | generator flags, see below | CODE |
| 0x7c | 0xdc | u32 | runtime state (init clears u16 gen+0xdc, recomputes bytes 0xde/0xdf) | CODE 0x10047c10 |
| 0x80..0x8c | 0xe0..0xec | 4 × u32 | byte offsets **from chunk start** of lists L0, L1, L2, L3 | CODE 0x10047c10; DATA (L0 always 0x90) |
| 0x90.. | 0xf0.. | — | the four lists, back to back, each ending with an op-0 record | DATA |

Generator flags (chunk+0x78 → gen+0xd8):

| bits | meaning | basis |
|---|---|---|
| 0-8 | burst count minus 1: each emission spawns `(flags & 0x1ff) + 1` particles. L0 ops 4/0x17 overwrite these bits from a keyframe | CODE 0x1004a680..0x1004a6d4, 0x10049aa4 |
| 9 (0x200) | set at run time by L0 op 0x11: the generator matrix follows its attach point every frame and particles are re-attached | CODE 0x1004a463, 0x10044b0a |
| 10 (0x400) | single-shot: spawn exactly one particle for the generator's whole life | CODE `0x10053910` |
| 11 (0x800) | runtime "stopped/started" state | CODE 0x10053100 |
| 12 (0x1000) | at init: skip actor attach for some origin modes | CODE 0x10047c10 (`bVar6`) |
| 14-27 | written at run time by the scheduler (`(value << 14) & 0xfffc000`) | CODE 0x100578d4..0x100578e9 |
| 29 (0x20000000) | one particle per emission (ignore burst count); also makes L1 ops 2/6/8 and L2 op 3 no-ops (no velocity setup) | CODE 0x1004a5ee, `0x1004e3a0` |

DATA: flag values seen: 0 (24,717), 0x1 (2,405), 0x200 (2,149), 0x2 (1,595), 0x20000000 (950),
0x3, 0x4, 0x5, 0x7, 0x201, 0xa, 0x204, 0x9, 0x1000.

### G.3 Bytecode record format (all four lists)

Every record is a whole number of dwords. Word 0:

```
bits 0-7    op            (op 0 = end of list)
bits 8-12   length in dwords, including word 0
bits 13-18  slot: dword index into the particle's variable block (L1/L2 only)
```

CODE: list advance `rec += ((w >> 8) & 0x1f) * 4` (0x100522f6, 0x1004b7b5, 0x1004a493); slot
`(w >> 11) & 0xfc` bytes (`0x1004e3c0`) = `((w >> 13) & 0x3f) * 4`.

DATA: each opcode has one fixed length in all 34,267 generators (except L2 op 0x5f: 4 dwords
979×, 3 dwords 54×), so a parser can skip unknown ops by the length field alone.

**Name references.** Records that reference another resource hold a 4-character name and a
handle slot that starts as 0 in the file. At link time (`Gen_Link` 0x10048150, called from
0x10071da4) the client fills the handle slot by looking up `(type, name)` with `0x100721c0`.
For keyframe references the name is at rec+8 and the handle at rec+4, and the type is 0x19
(CODE 0x100481aa.., 0x100483f7..). Lookups go through the resource tree, so a name may resolve
outside the current file (INFERRED: 37836 `g004` names D3m `hit2`, which 37836 does not contain).

**Particle variable block.** At spawn the client allocates `classSize + rec1[0x20] * 4` bytes
(CODE `0x10052990`); the variable block starts at `p + p[0x176]` (s16, = class size, 0x194
or 0x198). The `slot` field of L1/L2 records addresses dwords in this block. Its layout is
entirely data-defined: an L1 record writes initial values into a slot and the paired L2 record
reads the same slot every frame. Two slot layouts occur:
- vector slot: 3 × f32 (velocity, angular velocity, scale velocity) or 4 × s16 (colour velocity);
- keyframe channel: `{u32 kfHandlePtr, u32 mode, f32 base}` (`0x10052a70/80/90`).

### G.4 Origin word (chunk+0x10): where the generator sits

The generator's world matrix comes from `Gen_AttachMatrix` 0x1003bd40, which runs on gen+0x30
(so `ECX+4` = gen+0x34 = chunk+0x10). Two actors are available: **A** (`0x1003b600`, the
scheduler's first actor; INFERRED the caster) and **B** (`0x1003b630`, INFERRED the target). The
scheduler copies them into the generator when op 2 runs (CODE 0x10059925..0x10059938).

```
mode      = (bit16 << 4) | (w & 0xf)
pointA    = (bit18 << 6) | ((w >> 4) & 0x3f)     point id on actor A
pointB    = (w >> 10) & 0x3f                      point id on actor B
sizeModeA = (w >> 20) & 0xf                       scale by actor size, see below
sizeModeB = (w >> 24) & 0xf
```

| mode | matrix | basis |
|---|---|---|
| 0 | identity | CODE (default → vtbl+0xc4) |
| 1 | at A.pointA, yaw = A's facing | CODE case 1 |
| 2 | at B.pointB, yaw = B's facing | CODE case 2 |
| 3 / 0x16 | at A.pointA, oriented toward B.pointB (yaw+pitch); returns the distance | CODE case 3, `0x1003b8b0` |
| 4 / 0x17 | at B.pointB, yaw = A's facing | CODE |
| 5 / 0x18 | at A.pointA, yaw = B's facing | CODE |
| 6 / 0x19 | at B.pointB, oriented toward A.pointA | CODE |
| 7 / 8 | at A.pointA / B.pointB, rotation from global matrix `[g_1047bfa8]+0xc10` | CODE |
| 9 / 10 | full bone/point matrix of A.pointA / B.pointB (actor vtbl+0x1d0) | CODE `0x1003bcc0` |
| 11 / 12 | point matrix of A / B, yaw-only toward the other actor | CODE |
| 13 / 0x1a | line from A.pointA to B.pointB (variant flag 1) | CODE |
| 14 / 15 | special camera-relative matrices (`0x10186da0` / `0x10186dc0`) | CODE; meaning not traced |
| 0x10 / 0x11 | between pointA and pointB of the same actor (A / B) | CODE |
| 0x13 | B point 2 + 0.18 in Y, no rotation | CODE |
| 0x14 / 0x15 | matrix from `0x10177300` (0x15 with translation cleared) | CODE; meaning not traced |

Modes 0x16-0x1b are 3-6/0xd/0x12 with actor B replaced by A's current target
(`0x10084510`).

DATA, mode frequency: 2 (19,976), 1 (11,527), 0 (1,362), 4 (534), 3 (310), 6 (205), 9 (170),
0x12 (118), 13 (40), 10 (16), 11 (9).

**Point ids.** A point id goes to actor vtbl+0x1c4(id, out) and the result is added to the
optional offset. Special ids (CODE `0x1003b950`):

| id | position |
|---|---|
| 0x30 | ground under the actor (raycast from 6 above to 1 below) |
| 0x31, 0x32 | the point among ids 0x0d..0x14 of the actor that is nearest the other actor (a "hit location") |
| 0x33 | nearest of points 0x0d..0x14 to the camera position (`0x10015210`+0x44) |
| 0x34 / 0x35 | actor fields at +0x63c / +0x64c (stored positions) |
| 0x36..0x3c | remapped to 0x7e, 0x7f, 0x66, 0x67, 0x68, 0x69, 0x6a |
| other | actor point table (skeleton points; not traced here) |

DATA: 37836 `g000` uses mode 2, pointB 0x31 (spark at the hit location on the target); spells
2801/3901 use mode 2 with pointB 21.

**Size scaling** (CODE `0x10053d90`). If sizeModeA is 1-4 the size comes from actor A, if 5-8
from actor B. With actor size s (vtbl+0x98 / +0x9c) and weight W = chunk+0x20 (1.0 if 0), the
client sets `scaleA = (s - 1) * W + 1`:
- `mode & 3 == 0`: uniform, using max(s98, s9c);
- bit 1 set: Y uses s9c;
- bit 0 set: X and Z use s98.

The result is stored in gen+0x78..0x80. sizeModeB with chunk+0x24 does the same into
gen+0x84..0x8c. For line modes the returned distance replaces gen+0x78 (X scale), so a
unit-length mesh stretches from A to B (CODE 0x100540a1, 0x10053bb0). Where gen+0x78..0x8c are
multiplied into the particle transform was not traced (INFERRED: generator matrix scale).

**Actor size** (CODE actor vtable +0x98 = 0x100d0810, +0x9c = 0x100d08e0): from the skeleton's bounds set 0 (the
pointer 0x10035180 returns, after the attach points) {top, bottom, x max, x min, ...}: +0x98 = (x max - x min), +0x9c =
(bottom - top), each times the model scale (vtable +0x110, else +0x760 / +0x75c, else +0x754) times 0.5882353
(1 / 1.7, DATA 0x10331394), so a 1.7-tall model is size 1. No skeleton: 1.0.

xi-client (issue #83): `client::fxActorSize` and `EffectSystem::sizeScale`; set A scales the generator matrix, and the
camera-facing particle paths (flags bit 0, bits 6-8 = 3, bits 13-14) keep that scale on the particle (INFERRED). Set B
is not applied. DATA: the Ghrah orb's element core (2461..2466 `gc00` / `gp00` / `gp01`, origin 0x44001d1, weight 1;
mesh scale 0.2) is ~1.9x larger on the 3.3-tall orb, bigger than the orb's inner sphere `hh_c`, so its colour shows
through the gaps between the plates; at size 1 it was hidden inside that sphere.

### G.5 Generator lifetime and emission (CODE `Gen_Tick` 0x10049620, tail 0x1004a4c5..)

- `t_gen = elapsed / duration` (gen+0xc8 / gen+0xd0), clamped to ≤ 1. It is 0 when duration = 0.
  L0 keyframe ops evaluate their curves at `t_gen`.
- **Single-shot** (flags bit 10, or the L1 header's life = 0): one particle is spawned while
  gen+0x9c (spawn counter) = 0.
- **Periodic**: `countdown -= min(dt, 10)`. While countdown < 0 and elapsed < duration:
  - spawn `(flags & 0x1ff) + 1` particles, or 1 if bit 29 is set;
  - then `countdown += rand_u(interval_rand) + interval + 1`.
  The countdown starts at 0, so the first burst happens on the first tick.
- `elapsed += dt` every tick. The generator ends when `duration ≠ 0` and `elapsed ≥ duration`
  (returns 1 to the scheduler). Duration 0 means it runs until the scheduler stops it.
- Byte gen+0xde bits 1/2 (set at init for some file ids and names) scale dt by a global speed
  factor (`0x10002d80`). Ignore this for a first implementation.

DATA, interval (chunk+0x76): most common values 99, 59, 0, 79, 1, 2, 3, 9, 5, 119, 4, 49, 7, 100.
Random interval (chunk+0x74) is 0 in 33,218 chunks.

### G.6 RNG (CODE)

MT19937 (`0x1006bd50`, default seed 0x1105). Helpers:

| helper | returns |
|---|---|
| `rand_f(r)` `0x1006beb0` | `(mt % 100000) * 1e-5 * r`, in [0, r) |
| `rand_sym(r)` `0x1006bf00` | `((mt % 200000) * 1e-5 - 1) * r`, in [-r, r) |
| `rand_u(n)` `0x1006be90` | `mt % n` (unsigned; 0 if n = 0) |
| `rand_s(n)` `0x1006be70` | `(int)mt % n` (signed) |

### G.7 Particle object (runtime; fields read and written by the bytecode)

Base class ctor `0x100447c0` (vt 0x1032b018, size 0x194); D3m class ctor `0x10052e30`
(vt 0x1032ad80, size 0x198); D3a class `0x10052bc0` (vt 0x1032b238).

| offset | type | meaning | basis |
|---|---|---|---|
| 0x54 | vec3 | local position (generator space) | CODE (L1 op1 rec+0x14, L2 op2) |
| 0x60 | mat4 | local matrix | CODE 0x10045c70 |
| 0xa0 | mat4 | world matrix used for drawing | CODE 0x10045c70 |
| 0xe0 | vec3 | rotation X, Y, Z (radians, wrapped to ±π) | CODE L1 9/0xa, L2 5 |
| 0xec | vec3 | scale X, Y, Z | CODE L1 0xf, L2 8 |
| 0xf8 | 4 × u8 | colour in D3DCOLOR byte order: [0]=B, [1]=G, [2]=R, [3]=A | CODE 0x1003e1b0, L2 0x18-0x1b |
| 0xfc | ptr | owning generator handle | CODE 0x100449d0 |
| 0x104 | ptr | resource handle (mesh/texture) | CODE 0x100512e9 |
| 0x108 | ptr | snapshot of the generator matrix at spawn (header flag 0x800000) | CODE 0x10051112.. |
| 0x10c | u32 | header flags (L1 op 1, rec+4) | CODE 0x100512dd |
| 0x110 | f32 | remaining life (counts down by dt) | CODE 0x100513a5, 0x10044a10 |
| 0x114 | f32 | total life; 0 = immortal | CODE 0x100513f0 |
| 0x118 / 0x11c | f32 | flipbook frame timer / frame duration | CODE L1 0x1d, L2 0xd |
| 0x120 / 0x124 | f32 | texture U / V offset (texture-transform matrix _31/_32) | CODE 0x10040321.. |
| 0x128 | f32 | depth/sort bias (INFERRED); values -0.1 … -100 | CODE 0x1004500f; DATA |
| 0x16c | u16 | blend mode (default 0x48) | CODE 0x1005140c, 0x10046720 |
| 0x170 | u16 | flipbook frame index | CODE L2 0xd |
| 0x176 | s16 | offset of the variable block | CODE 0x100447db |
| 0x179 | u8 | L1 header rec+0x28 (always 0 in data); non-zero selects an alternate draw path | CODE 0x10052900, 0x10040ad0 |
| 0x187 | u8 | "matrix dirty" | CODE `0x1004e390` |

Particle age used by L2 curves: `t = 1 - remaining / total` (0 when total = 0).
CODE: L2 op 0xe at 0x1004d5ed, `0x1004e380`.

Particle frame (CODE `0x10044a10`):
1. If total ≠ 0: `remaining -= dt`.
2. If remaining < 1: run L3. If remaining is still < 1, the particle dies, unless its header flag (p+0x10c, L1 op 1)
   bit 29 is set and its generator still exists (then it stays alive with remaining = 0; once the generator is gone
   the link p+0xfc is null and it dies). A kept particle with an attached child (p+0x164) whose count (+0x18 & 0x7fff)
   is below 2 dies too (not done in xi-client). Before issue #76 xi-client read the generator's flag bit 29 and kept
   such particles forever (Hundred Fists' glowing fists stayed behind).
3. If generator flags bit 9 (L0 op 0x11) is set, re-attach to the generator matrix.
4. Run L2.

### G.8 L1 op 1 — particle header (always the first L1 record; 12 dwords)

DATA: present and first in 34,267/34,267 generators. CODE: 0x10050c56 .. 0x10051444.

| rec offset | type | meaning | basis |
|---|---|---|---|
| +0 | u32 | `0x0c01` (op 1, 12 dwords) | DATA |
| +4 | u32 | particle flags → p+0x10c (table below) | CODE 0x100512d3 |
| +8 | u32 | handle slot (0 in the file) | CODE |
| +0x0c | char[4] | resource name, e.g. `0110` | CODE/DATA |
| +0x10 | u32 | 0 | DATA |
| +0x14 | vec3 | initial local position → p+0x54 | CODE 0x1005130f |
| +0x20 | u8 | variable-block size in dwords | CODE `0x10052990` (`size + n*4`) |
| +0x21 | s8 | resource chunk type (below) | CODE 0x10050c56 |
| +0x22 | s16 | particle life in ticks (0 = immortal; also makes the generator single-shot) | CODE 0x10051399, `0x100492e0` |
| +0x24 | s16 | random life added: `+ rand_u(n)` | CODE 0x100513ab |
| +0x28 | u8 | → p+0x179 (always 0 in data) | CODE; DATA |
| +0x29..+0x2f | — | always 0 | DATA |

**Resource type (rec+0x21) and the particle class** (CODE jump tables 0x10052624/0x10052634 and
0x1005264c/0x10052684; resource lookup `0x100494a0`):

| rec+0x21 | looked up as | class / ctor (alloc size) | DATA count |
|---|---|---|---|
| 0x0b, 0x0c, 0x1f | 0x1f D3m mesh | flags & 0x200000 → 0x10052dc0 (0x19c); & 0x100000 → 0x10052e50 (0x198); flags byte 3 bit 0 → (0x1c0); else **0x10052e30 (0x198)**. If no D3m is found, falls back to an Mmb (0x2e) of the same name in the parent chain | 27,775 (type 0x0b) |
| 0x0e, 0x21, 0x39 | 0x21 D3a | in edit mode 0x10052c00 (0x231); flags & 0x200000 → 0x10052b60; else **0x10052bc0 (0x194)** | 3,211 (0x0e) |
| 0x1d | 0x25 D3b | flags & 0x100000 → 0x10052d50; else 0x1003faa0 (0x1c0) | 241 |
| 0x22, 0x23, 0x24 | same type | dedicated classes 0x10050f5e / 0x1005102c / 0x10050fdb | 656 / 1 / 320 |
| 0x3d | Sep (sound) | sound emitter 0x1005104d | 679 |
| 0x47 | — (no resource) | **point light** (ctor 0x10056220, vtable 0x1032b758; §Z) — earlier read as a generator clone | — |
| 0x57 | — (no resource) | 0x100530b0 (0x194) | 1,373 |
| 0x5a | — | 0x10053020 (0x1e0), attached to actor B | 5 |
| 0x5c | — | 0x10050fb2 | — |
| 0x01 | — | 0x10047890 (0x1d9) | 6 |

**Particle flags (rec+4 → p+0x10c).** Bits whose meaning is tied to code:

| bit | effect | basis |
|---|---|---|
| 0 (0x1) | billboard: world = camera-facing matrix built from the local matrix (vtbl+0x74), unless any of bits 6-8 is set | CODE 0x10045c70 |
| 2 + 3 | screen-space: world = the local matrix (position p+0x188, no generator matrix) x diag(1, -1, 1) (0x10045cd7..0x10045d24), and the view is `[g_1047cf7c]+0x810` (0x10045b80), set to the identity at 0x1006ce6c: the particle sits in camera space (x right, y down, z ahead) under the scene's projection. Used by the fishing arrows (3727 `ar*` / `al*`, about (±0.09, −0.025, 0.25), scale 0.02). xi-client: `EffectSystem::build` places it through the camera basis (#100) | CODE |
| 2 alone | world matrix from the local matrix plus position p+0x54 only | CODE |
| 6-8 (0x1c0) | disable the full billboard; uses the view matrix from 0x10045b80 | CODE |
| 13, 14 | pick the view-alignment matrix: 0 → `bfa8+0xb90`, 1 → `+0xd10`, 2 → `+0xd50`, 3 → `+0xc10`; also do not multiply by the generator matrix | CODE 0x10045b80; INFERRED: the matrices are full, Y-locked and other camera billboards |
| 17 (0x20000) | use the local matrix directly, no generator transform | CODE |
| 18 | z-write / alpha-test variant in draw | CODE 0x10040cb0 |
| 22 | p+0x12c = p+0x128 directly (no depth term) | CODE 0x10045005 |
| 23 (0x800000) | snapshot the generator matrix at spawn into p+0x108: the particle stays where it was emitted when the generator moves | CODE 0x100510f6.. |
| 26 (0x4000000) | position is transformed and its Z negated at spawn | CODE 0x1005132d |
| 27 | depth-key adjustment | CODE 0x1004506e |
| 28 | draw variant | CODE 0x10040cd5 |
| 29 (0x20000000) | keep the particle alive at life 0 | CODE 0x10044a10 |

DATA, most common flag values: 0x1 (11,066+), 0xc1, 0x800001, 0x0, 0x4000, 0x1000, 0xd, 0x8000c1,
0xc0, 0x20001, 0x800000, 0x201.

### G.9 L1 ops (spawn-time), traced

`slot` is the record's variable-block slot. "rec.xyz" means three f32 at rec+4, +8, +0xc.

| op | len | count | effect | basis |
|---|---|---|---|---|
| 0x02 | 4 | 9,896 | velocity: slot.xyz = rec.xyz (0 if generator flag bit 29) | CODE 0x100514cd |
| 0x03 | 4 | 2,640 | slot.xyz += rand_sym(rec.xyz) per component | CODE 0x1005151a |
| 0x06 | 4 | 1,186 | spawn on a sphere: position += R(rand_sym(π), rand_sym(π)) · (rand_f(rec+4) + rec+8) along one axis (divided by gen+0x90 if non-zero). Skipped if bit 29 | CODE 0x10051dd4 |
| 0x07 | 8 | 993 | like 0x06 but the direction is also shaped by a scale matrix (rec+0xc, +0x10, +0x14) and rotated by rec+0x18/+0x1c | CODE 0x10051e83 |
| 0x08 | 2 | 5,518 | radial velocity: slot.xyz += normalize(p.pos − header pos) · rec+4. Skipped if bit 29 | CODE 0x100521eb |
| 0x09 | 4 | 33,587 | rotation p+0xe0 = rec.xyz (radians) | CODE 0x1005156f |
| 0x0a | 4 | 10,500 | rotation += rand_sym(rec.xyz), wrapped to ±π | CODE 0x1005157a |
| 0x0b | 4 | 13,220 | angular velocity slot.xyz = rec.xyz (rad/tick) | CODE 0x100516a1 |
| 0x0c | 4 | 4,466 | angular velocity += rand_sym(rec.xyz) | CODE 0x100516cf |
| 0x0f | 4 | 33,588 | scale p+0xec = rec.xyz | CODE 0x10051b01 |
| 0x10 | 4 | 1,098 | scale += rand_f(rec.xyz) per axis | CODE 0x10051b22 |
| 0x11 | 2 | 4,434 | scale.xyz += one rand_f(rec+4) (uniform) | CODE 0x10051b62 |
| 0x12 | 4 | 16,577 | scale velocity slot.xyz = rec.xyz | CODE 0x10051b9e |
| 0x13 | 4 | 387 | scale velocity += rand_sym(rec.xyz) | CODE 0x10051bcc |
| 0x16 | 2 | 33,588 | colour = rec+4. The file's u32 bytes are R, G, B, A in that order; 0x80 = 1.0 (FFXI 2× modulate, INFERRED) | CODE 0x10051c21, 0x1003e1b0 |
| 0x17 | 2 | 2,383 | R += rand_u(rec[4]), G += rand_u(rec[5]), B += rand_u(rec[6]) (bytes, wraps) | CODE 0x10051cdf |
| 0x18 | 2 | 799 | R, G, B += one rand_u(rec[4]) | CODE 0x10051d2c |
| 0x19 | 3 | 5,504 | colour velocity: slot = 4 × s16 (R, G, B, A) in 1/256 units per tick | CODE 0x10051d66, 0x1003e430 |
| 0x1a | 3 | 75 | colour velocity s16 R, G, B += rand_s(rec s16 at +4, +6, +8) | CODE 0x10051d7f |
| 0x1d | 2 | 3,208 | flipbook: frame duration = life / frameCount, where frameCount = s16 at resource+2 (D3a/D3m frame count); frame index starts at 0 | CODE 0x100522ae |
| 0x1e | 2 | 11,697 | blend mode p+0x16c = u16 rec+4 (§G.12) | CODE 0x10051c35 |
| 0x1f | 12 | 6,924 | spawn on an ellipsoid shell or ring: yaw = rec+0x20 + rand_sym(rec+0x24); second angle = evenly spaced `2π·(spawnIndex mod (n+1))/(n+1) − π` when s16 rec+0x2c = n ≠ 0, else rand_sym(π); shape scale rec+0xc/+0x10/+0x14; orientation rec+0x18/+0x1c; radius rand_f(rec+4) + rec+8; rec+0x28 bit 0 = relative to the generator matrix; result added to position | CODE 0x10051fb1..0x100521e6 |
| 0x21-0x23 | 4 | 298/1277/28 | keyframe channel position X/Y/Z: slot = {kf(rec+8), mode = rec+0xc, base = current pos.c} | CODE 0x100503f1.., link 0x100483f7 |
| 0x24-0x26 | 4 | 163/709/280 | keyframe channel rotation X/Y/Z (base = rot/π) | CODE 0x10050429.., 0x10052aa0 |
| 0x27-0x29 | 4 | 10,742/10,705/1,467 | keyframe channel scale X/Y/Z | CODE 0x100504dc.. |
| 0x2a-0x2c | 4 | 1,201/1,754/918 | keyframe channel R/G/B (base = byte/255) | CODE 0x1005058f.. |
| 0x2d | 4 | 29,670 | keyframe channel alpha (base = A/255) | CODE 0x10050bb2 |
| 0x2e, 0x2f | 4 | 868/598 | keyframe channels for texture U / V offset | CODE 0x100509b5, 0x10050a06 |
| 0x30 | 2 | 7,324 | p+0x128 = f32 rec+4 | CODE 0x10051c45 |
| 0x3b | 4 | 7,186 | rotation += spawnIndex · rec.xyz, wrapped (fans particles around an axis) | CODE 0x1005028b |
| 0x3c, 0x44, 0x53, 0x6a | 3/3/3 | 648/456/321/97 | name a child generator (type 5) at rec+8; the child runs per particle (paired with L2 0x25/0x33/0x46) | CODE link 0x100483a9/0x100483d0 |
| 0x41 | 2 | 3,040 | slot.xyz += normalize(p.pos − header pos) · rand_sym(rec+4) | CODE 0x1004f270 |
| 0x45 | 1 | 3,616 | child generator inherits actor A/B from the parent particle's generator | CODE 0x1004f815 |
| 0x72 | 3 | 18,012 | if actor B is a PC-model actor (class 0x10330ebc): p+0x128 = ((B.size − 1) · (rec+8 or 1) + 1) · rec+4 | CODE 0x10051c53 |
| 0x7b | 7 | 53 | attach offsets for §G.4: two vec3 (position offset at rec+4, rotation at rec+0x10) passed to `0x1003bd40` | CODE 0x10053d90, 0x10053bb0 |

Keyframe-channel L1 ops not in the table (same `{kf, mode, base}` slot form; link type 0x19 at
CODE 0x10048445..0x10048e58): 0x33-0x39, 0x50-0x52, 0x59-0x5f, 0x60-0x66, 0x68-0x70, 0x74-0x78,
0x7c, 0x7d, 0x80-0x85, 0x8b-0x8d, 0x93, 0x95-0x97, 0x69, 0x82. 0x82 (1,033, 6 dwords) pairs with
L2 0x5f. Other resource references: 0x4e/0x4f/0x54 use type 0x3e (Vtx), 0x6b uses 0x4a (Sph),
0x55 uses 0x0a (TexInfo, texture swap).

Untraced L1 ops seen in data (op:count/len): 0x31:22/2, 0x32:1312/3, 0x3a:320/10, 0x3d:419/1,
0x3e:159/3, 0x3f:30/3, 0x40:350/3, 0x42:177/1, 0x43:349/2, 0x46:63/2, 0x47:120/1, 0x48:43/1,
0x49:58/1, 0x4a:12/1, 0x4c:679/4 (sound generators), 0x56:1251/2, 0x57:1/4, 0x67:218/2,
0x79:5/1, 0x87:8/1, 0x89:4/2, 0x8f:114/1, 0x90:12/1, 0x91:192/2, 0x94:6/1, 0x9b:15/1, 0x9e:1/1.
Handlers: jump table 0x100523ac (op − 1, 0..0x9d).

### G.10 L2 ops (per particle per frame), traced

| op | len | count | effect | basis |
|---|---|---|---|---|
| 0x02 | 1 | 9,895 | position += slot.xyz · dt | CODE 0x1004d67b |
| 0x03 | 4 | 3,416 | slot.xyz += rec.xyz · dt (acceleration, e.g. gravity; Y is down in FFXI so up is −Y). No-op if generator flag bit 29 | CODE 0x1004d6b8 |
| 0x05 | 1 | 13,220 | rotation += slot.xyz · dt | CODE 0x1004d858 |
| 0x06, 0x09 | 4 | 3,201/6,710 | slot.xyz += rec.xyz · dt (acceleration of any vector slot) | CODE 0x1004da07 |
| 0x08 | 1 | 16,577 | scale += slot.xyz · dt | CODE 0x1004d969 |
| 0x0b | 1 | 5,504 | colour += colour-velocity slot: `byte = clamp((byte << 8) + s16·dt, 0, 0xffff) >> 8`. The fraction is dropped every frame | CODE 0x1004d738, 0x1003e430 |
| 0x0c | 3 | 274 | slot s16[0..3] += rec s16 · dt2 (dt2 = `bfa8+0xeac`) | CODE 0x1004d752 |
| 0x0d | 1 | 3,208 | flipbook: timer −= dt; on underflow add the frame duration and increment the frame index | CODE 0x1004debb |
| 0x0e | 1 | 31,888 | compute particle age `t = 1 − remaining/total` for the following ops (put it first) | CODE 0x1004d5ed |
| 0x0f-0x11 | 1 | 298/1,277/28 | pos.x/y/z = base + kf(t) with key 0 forced to 0 | CODE 0x1004b8a5.. |
| 0x12-0x14 | 1 | 163/709/280 | rot.c = wrap(fmod(kf(t; key 0 := base), 2) · π) | CODE 0x1004b9c8, 0x1003ddb0 |
| 0x15-0x17 | 1 | 10,742/10,705/1,467 | scale.c = kf(t) with key 0 := base | CODE 0x1004ba40.. |
| 0x18-0x1a | 1 | 1,201/1,754/918 | R/G/B byte = kf(t; key 0 := base) · 255 | CODE 0x1004bb99.. |
| 0x1b | 1 | 29,670 | A byte = kf(t; key 0 := base) · 255 (≤ 0 → 0) | CODE 0x1004bc80 |
| 0x1c, 0x1d | 1 | 868/598 | U / V texture offset from a keyframe | CODE 0x1004c78d.. |
| 0x25 | 1 | 456 | tick the particle's child generator (`Gen_Tick` on p+0x164) | CODE 0x1004b500 |
| 0x26 | 4 | 446 | rotate the vector in slot by Euler(rec.xyz) | CODE 0x1004c485 |
| 0x27 | 2 | 3,395 | U offset += rec+4 · dt (texture scroll) | CODE 0x1004c4c0 |
| 0x28 | 2 | 3,607 | V offset += rec+4 · dt | CODE 0x1004c4ed |
| 0x29-0x2b | 4 | 159/30/350 | sine wobble: pos += sin((age · rec+4 / 60) + rec+8) · slot amplitude · dt along X/Z (0x29, 0x2b) or Y (0x2a) | CODE 0x1004c507.. |
| 0x2c | 3 | 4,832 | drag: slot.xyz *= pow(rec+4, dt) | CODE 0x1004c5fb |
| 0x5f | 4 (3) | 1,033 | distance/visibility test against p+0x12c and the camera (rec+8 threshold); paired with L1 0x82 | CODE 0x1004c009; semantics INFERRED |
| 0x60 | 7 | 405 | p+0x12c threshold test with plane normal p+0x188; INFERRED a ground or plane reflection | CODE 0x1004ab03 |

Untraced L2 ops (op:count/len): 0x1e-0x22 (pair with L1 0x33-0x37), 0x24 (L1 0x39), 0x2e:72/4,
0x2f:21/1, 0x34:12/1, 0x35-0x37 (L1 0x59-0x5b), 0x3b:14/1, 0x41:2/1, 0x44:2/1, 0x54-0x58
(L1 0x74-0x78), 0x64:8/5, 0x65:5/7, 0x69:192/2 (L1 0x91), 0x6a:2/1. Handlers: jump table
0x1004df6c (op − 1, 0..0x6d).

**Pairing rule** (DATA, counts match exactly): every L1 op that sets up a slot has an L2 op that
consumes the same slot, e.g. L1 0x02 ↔ L2 0x02, 0x0b ↔ 0x05, 0x12 ↔ 0x08, 0x19 ↔ 0x0b,
0x1d ↔ 0x0d, 0x21+k ↔ 0x0f+k (k = 0..0x0e), 0x33-0x37 ↔ 0x1e-0x22, 0x44 ↔ 0x25, 0x53 ↔ 0x33,
0x59-0x5b ↔ 0x35-0x37, 0x74-0x78 ↔ 0x54-0x58, 0x82 ↔ 0x5f, 0x91 ↔ 0x69.

### G.11 L0 ops (generator per frame) and L3 ops (on particle expiry)

L0 (CODE `Gen_Tick` jump table 0x1004a744, ops 4..0x17). Keyframe ops have
`{+4 handle, +8 name, +0xc mode}` and evaluate at `t_gen`:

| op | len | count | effect |
|---|---|---|---|
| 0x04 | 5 | 367 | emission interval gen+0xd6 = kf (clamped to 0x7fff); burst count bits from a second value |
| 0x05 | 5 | 1 | L1 op 8's radial speed = kf |
| 0x06 / 0x07 | 5 | 4 / 35 | L1 op 0x1f record +4 / +8 (radius random / base) = kf |
| 0x08 / 0x09 | 5 | 2 / 7 | L1 op 0x1f record +0x18 / +0x1c = kf · π, wrapped |
| 0x0a | 4 | 73 | camera-distance cull: hide or stop when the distance from the generator to the camera is above rec+4 (0 = global draw distance) or below rec+8; rec+0xc bit 0 = stop the generator |
| 0x0b-0x0d | 5 | 5 / 22 / 0 | L1 header spawn position X/Y/Z (rec+0x14..) = kf |
| 0x0e-0x10 | 5 | 2 / 83 / 2 | rotate the generator matrix about X/Y/Z by kf · π (L1 header life used to normalise) |
| 0x11 | 2 | 26,820 | follow: set flags bit 9 and blend the generator matrix toward the attach matrix each frame. rec+4 bit 0 = translation, bit 1 = rotation, bits 2-9 = blend factor/255. DATA: 0x3fd (translation, factor 1.0) 21,950×; 0x3ff 4,471× |
| 0x12-0x14 | 5 | 0 / 0 / 97 | L1 op 2 initial velocity X/Y/Z = kf |
| 0x15 | 7 | 37 | per-frame test/spawn using a matrix built from rec+0xc..+0x14 (not traced) |
| 0x17 | 5 | — | like 0x04 but evaluated at a global time `DAT_10629ff4` |
| 0x5d | 1 | 276 | not handled by `Gen_Tick` (default case); not traced |

CODE for each: 0x10049940 (4), 0x10049aea (5), 0x10049b50/0x10049bb6 (6/7), 0x10049c1c/0x10049cd5
(8/9), 0x100496de (0xa), 0x10049eba.. (0xb-0xd), 0x10049fe6/0x1004a166/0x1004a2e6 (0xe-0x10),
0x1004a463 + 0x10053bb0 (0x11), 0x10049d8e.. (0x12-0x14), 0x100497ec (0x15), 0x10049a04 (0x17).

L3 (CODE 0x1004e650, jump table 0x1004e7e4). Called by the particle tick when remaining < 1:

| op | len | count | effect |
|---|---|---|---|
| 0x01 | 3 | 445 | handle at rec+4 → generator: run one immediate tick of it with this particle as parent (a burst at the particle's position) |
| 0x02 | — | 0 | handle at rec+4 → scheduler: start it with the same actors A/B |
| 0x04 | 1 | 12 | call `0x10063a50` on the object whose handle is at variable-block slot + 0x1c (INFERRED: stop an attached sound) |
| 0x05 | 1 | 25 | rebirth: remaining = total (the particle loops forever) |
| 0x06 | — | 0 | jump to the L3 list of the generator at rec+4 |
| 0x07 | 1 | 4 | not handled (default) |

### G.12 Rendering

**World matrix** (CODE `0x10045c70`). In the default path:

```
world = local(p+0x60) × genMatrix(rotation only, unless flags 13/14) × viewAlign(0x10045b80)
world.translation = p+0x188
```

- The billboard variant (flags bit 0) instead builds the matrix with vtbl+0x74.
- p+0x188 is the world position: the local position transformed by the generator matrix
  (CODE 0x10044fed).
- When the result's Z axis is short (|z| < 0.2), the client rescales it to 0.2 (CODE).
- The local matrix comes from scale, rotation and position. Rotation order: INFERRED as
  D3DX yaw-pitch-roll (device vtbl+0x58 is called with three angles). The other agent's
  Meshes section should confirm it.

**Blend mode** (p+0x16c, CODE `0x10046720` switch on the low byte, D3D8 SetRenderState
vtbl+0xc8; 0x13 = SRCBLEND, 0x14 = DESTBLEND):

| code | SRCBLEND / DESTBLEND | meaning | DATA (explicit op 0x1e) |
|---|---|---|---|
| 0x48 (default) | SRCALPHA / ONE | additive, alpha-weighted | 2,032 explicit + 22,570 by default |
| 0x49 | SRCALPHA / ONE | additive | — |
| 0x44, 0x03 | SRCALPHA / INVSRCALPHA | normal alpha blend | 9,503 |
| 0x46, 0x47 | ZERO / INVSRCALPHA | darken (multiply by 1 − α) | 21 / 1 |
| 0x00 | ONE / ZERO | opaque | — |
| 0x64 | SRCALPHA / INVSRCALPHA; TFACTOR alpha byte (0x1047bc0f) = min((mode >> 7) & ~1, 255) | alpha blend at a fixed alpha | 3 (0x7f64) in spell files; Port Windurst's windmill sails (zone file 340, `fan1`..`fan4`, D3m `_wi_`): 0x8064 |
| 0x68 | SRCALPHA / ONE; same TFACTOR alpha | additive at a fixed alpha | 7+4 (0x8068, 0x7f68) |
| 0x41, 0x42, 0x43, 0x62 | special path `0x10046480` (not traced); 0x62 first sets the TFACTOR alpha byte to the high byte | — | 2 / 105 / 10 |
| other | render state left unchanged | — | 0x4c: 6, 0x74: 1 |

The switch reads the **low byte only** (`mode & 0xff`); the high byte is a parameter, so 0x8064 is blend 0x64 (#103:
xi-client compared the whole u16, drew 0x8064 as the additive default, and the windmill sails came out see-through).
**TFACTOR alpha for 0x64 / 0x68 / 0x62** (CODE): the draw first writes the particle's colour, alpha times its fades,
to TFACTOR (0x1047bc0c, at 0x10045fc8), then the blend switch overwrites its alpha byte (0x1047bc0f), so the particle's
own alpha and fades do not apply: 0x8064 draws with TFACTOR alpha 0xFF, which a D3m's stage 1 MODULATE4X saturates
(§M.2), so its sails are opaque wherever the texture is. xi-client: `EffectSystem::fxBlendClass`,
`fxBlendElementAlpha`. Every particle draw also sets ALPHATESTENABLE 1, ALPHAFUNC GREATER, ALPHAREF 0x60 (CODE
0x10046917..0x10046947); xi-client does not alpha-test effects yet (INFERRED: the faint texels it would cut are mostly
invisible under the blend anyway).

Colour: vertex/material colour is the particle's colour bytes. INFERRED: as elsewhere in FFXI,
0x80 means 1.0 (2× modulate), so `80808080` is "white, full alpha" and alpha-curve values
around 0.5 (k000 peak 0.502) give alpha 0x80.

### G.13 Worked example: 37836 (Galka Fast Blade), `g000` (DATA, decoded with the rules above)

- Origin 0xc402: mode 2 (actor B, B's facing), pointB = 0x31 (hit location on the target).
  Interval 61, burst 1.
- L0: op 0x11 = 0x3fd (follow the target's translation).
- L1:
  - header: resource D3a `0110` (type 0x0e), flags 0x800001 (billboard + detach at spawn),
    28 var dwords, life 20, position (0, −0.3, 0.3);
  - rot 0; scale (0, 0.2, 1);
  - scale velocity in slot 3 = (0.04, 0, 0);
  - colour 80808080;
  - alpha curve `k000`;
  - flipbook over the D3a frames.
- L2: age; scale += slot3·dt (X grows 0 → 0.8 over 20 ticks); flipbook; alpha = k000(t)·255.

Result: a billboarded slash streak at the target's hit point that widens and fades over 20
ticks. It is spawned when the scheduler's `hit1`/`hit2` (or `main`) op 2 starts `g000`.

Spell/mob samples (DATA): 2801 `prn1`: D3s-type `ring` (0x24), velocity −Y 0.041 with random
spread, slow spin, per-index yaw fan (op 0x3b), X/Y scale curve `prx0`, alpha `pra0`.
3901 `k201`: D3m `k2  ` on an ellipsoid shell (op 0x1f), radial velocity with drag 0.97
(L2 0x2c), random roll and spin, normal alpha blend (0x44). 2944 `gs00`: a sound generator
(type 0x3d `3032`).

### G.14 Implementation notes for the Linux client

1. Parse the chunk and keep chunk+0x10, 0x20, 0x24, 0x74, 0x76, 0x78 and the four list offsets.
2. Implement the list walker with the length field; ignore unknown ops (lengths are stable).
3. Minimal useful op set: L1 1, 2, 3, 6, 8, 9, 0xa, 0xb, 0xc, 0xf, 0x10, 0x11, 0x12, 0x16, 0x19,
   0x1d, 0x1e, 0x1f, 0x21-0x2d, 0x30, 0x3b, 0x72; L2 2, 3, 5, 6, 8, 9, 0xb, 0xd, 0xe, 0xf-0x1b,
   0x27, 0x28, 0x2c; L0 0x11; L3 1, 5. By count this covers more than 95% of op instances.
4. Attach: modes 1, 2 (and 3 for beams) with point ids from the skeleton. Until skeleton point
   ids are mapped, use the actor's position plus a fixed height for 0x31 and 1/21 (INFERRED).
5. Blend: implement 0x48 (additive) and 0x44 (alpha); treat the rest as 0x44.

### G.15 Open items (Generators)

- Actor point table (vtbl+0x1c4 ids 0..0x2f and 0x66-0x7f): which skeleton points they are.
- The four view-alignment matrices at `[g_1047bfa8]+0xb90/0xc10/0xd10/0xd50`.
- Where gen+0x78..0x8c (actor-size scale) enters the particle transform.
- Exact meaning of p+0x128/0x12c (depth bias vs plane height) and L2 0x5f/0x60.
- Ops listed as untraced in §G.9/G.10, the 0x41-0x43 blend path, and the classes for resource
  types 0x22-0x24 and 0x57.
- Whether `dt` is exactly 1/60 s per unit.

---------------------------------------------------------------------------------------------

## KeyFrames (chunk type 0x19, "KeyFrame")

### K.1 Layout

Load is the default chunk copy (vtable 0x1032c5fc slot 7 = `0x10071d30`), so obj+0x30 is the
payload (CODE). The payload is a list of key pairs:

```
struct Key { float t; float value; };   // repeated
```

- The first key has t = 0.0 (DATA 34,057/34,057).
- t never decreases (DATA, no exceptions).
- The list ends at the first key with t ≥ 1.0. That key always has t = 1.0 exactly (DATA).
- After it the chunk is zero-padded to a 16-byte boundary (0 or 8 bytes; DATA).
- Key count is 2..21. The most common counts are 3-6 (DATA).
- Values range from −7.86 to 363 (DATA). Their meaning depends on the channel: alpha/colour use
  0..1 (scaled by 255), rotation uses units of π, scale and position use world units.

Example 37836 `k000`: (0, 0), (0.675, 0.502), (1.0, 0). The alpha rises to 0x80 at 67.5% of the
particle's life, then falls to 0.

### K.2 Evaluation (CODE)

`KF_EvalLinear` `0x10054430(t)`:
1. Clamp t to [0, 1].
2. Walk keys from key 1 until `key.t ≥ t`.
3. With the previous key a and the found key b, return
   `a.v + (b.v − a.v) · (t − a.t)/(b.t − a.t)`. If a.t == b.t, return b.v.

`KF_EvalSmooth` `0x100546d0(t)` (no clamp) uses the same segment search, then
`u = (t − a.t)/(b.t − a.t)`:

```
u <  0.5:  w = 2u²            → v = a.v·(1 − w) + b.v·w
u >= 0.5:  w = 2(1 − u)²      → v = a.v·w + b.v·(1 − w)
```

This is a quadratic ease-in-out per segment.

Wrappers `0x10054400(t, base)` (linear) and `0x10054620(t, base)` (smooth) evaluate the curve
with **key 0's value temporarily replaced by `base`** (they swap obj+0x34). Particle channels use
this so a curve starts from the particle's spawn value. Which one runs depends on the channel's
`mode` (L1 record +0xc): `mode & 0xf` = 0 → linear, otherwise smooth (CODE 0x1004b8bd,
`0x1004e410`, `0x1003ddb0`). Position channels pass base = 0 and add the stored base afterwards.

### K.3 How keyframes are referenced

By 4-character name from generator records (name at rec+8, resolved by `Gen_Link` 0x10048150 with
type 0x19 into rec+4). The input t is:
- particle age `1 − remaining/total` in L2 channel ops;
- generator progress `elapsed/duration` in L0 ops;
- a global time in L0 op 0x17.

KeyFrame vtable 0x1032c5fc slots 8/9 (`0x100543b0`, `0x100543c0`) were not needed for
evaluation and were not traced.

---------------------------------------------------------------------------------------------

# Meshes, scheduler ops and melee hit effects

Written 2026-10-17 by the mesh/scheduler pass (same binary and tags as above; several vtable-only functions
have no Ghidra function object and were read from an offline `objdump -d -M intel` listing).

Conventions for this part:
- "payload" = chunk data after the 16-byte header. The client copies the whole chunk to `obj+0x20`, so
  `obj+0x30` = payload+0. Offsets are payload offsets unless written `obj+`/`rec+`/`elem+`.
- 4CC names are little-endian as stored (`'b0a?'` = bytes 62 30 61 3F). A final `?` (0x3F) is a wildcard
  (lookups mask the name with 0xFFFFFF).
- Colours are PS2-scaled: 0x80 = 1.0.

## Meshes

Three chunk types carry effect geometry. None is drawn on its own: a generator element (particle, see
"Generators") holds the mesh resource at `elem+0x104` and draws it with the element's colour, matrix (G.12)
and blend mode (CODE: `0x10040ad0` → `0x10040150`; `0x100432f9`/`0x10043439` → `0x100405a0`).

Vtables (CODE, .rdata; slot 7 = load, 8 = init, 9 = release):

| Type | Name | Vtable | Load (7) | Init (8) | Release (9) |
|---|---|---|---|---|---|
| 0x1F | D3m (`CMoD3mSpecularElem`) | 0x1032c74c | 0x10071d30 (copy chunk) | 0x1003fe10 | 0x100400f0 |
| 0x21 | D3a (`CMoD3aLightmapElem`) | 0x1032c720 | 0x10071d30 | 0x1003e5e0 | 0x1003e810 |
| 0x25 | D3b | 0x1032c6f4 | 0x10071d30 | 0x1003f350 | 0x1003f360 (ret) |
| 0x54 | Afb (weapon trail) | 0x1032c778 | 0x10071d30 | 0x10071950 → 0x1001ced0 | 0x10071960 → 0x1001cf10 |

### M.1 Texture references (all mesh types)

A mesh names its texture with **16 bytes**, the same 16 bytes as a texture chunk's header name (payload+1 of a
type 0x20 chunk, dat-format §4): 8-char category + 8-char name, space padded. DATA: in 37836 the D3m `0112`
references `"0011    0112    "`, which is the header name of texture chunk `0112` in the same file.

Lookup `0x10075450` (CODE): exact 16-byte match first (`0x1003ae40`); if none, a second pass compares **only
bytes 8..15** against textures that are global (`+0x2c & 0x100`) or have refcount 1. Implementation: resolve by
full name within the same DAT, then by the last 8 bytes across loaded textures.

### M.2 D3m (type 0x1F): textured triangle list

CODE `0x1003fc10`/`0x1003fc20`/`0x1003fca0`/`0x1003fe10`. DATA: all 2,762 D3m chunks in files 0, 2800–3099
and 37836–37899 match this layout exactly (size = computed size + <16 bytes padding).
```
+0x00 u16  bits 0..3 = format (6 in every chunk; code accepts 5, 6, 7)
           bits 4..15 = rewritten at init with the bounding radius; file value irrelevant
+0x02 u16  flags: bit 0 => alpha test (ALPHATESTENABLE, ALPHAFUNC=GREATER, ALPHAREF=0x60) in the
           0x100405a0 path; 0x8080 = runtime "built" marker. 0 in all data.
+0x04 u8   nTex    textured draw groups (0..3 seen)
+0x05 u8   nPlain  untextured draw groups (0..1 seen)
+0x06 u16  nTri    total triangles; vertex count = 3*nTri
+0x08 u16  triCount[nTex+nPlain]   triangles per group in draw order (sum == nTri in all data)
           padding: with k = nTex+nPlain the names start at 8 + 2*(k & ~3) + (k & 3 ? 6 : 0)
           (CODE 0x1003fc69..0x1003fc8b; the pad bytes are garbage, e.g. 00 00 65 00)
+names     char texName[nTex][16]
+verts     Vertex[3*nTri], 36 bytes (= D3D FVF 0x152, XYZ|NORMAL|DIFFUSE|TEX1):
             f32 x, y, z;  f32 nx, ny, nz;  u8[4] colour (D3DCOLOR order, DATA c8 c8 c8 80);  f32 u, v
```
- Primitive: `0x10043e70(firstVertex, primCount, 4, 0, -1)`, 4 = TRIANGLELIST (CODE). Group i draws
  `triCount[i]` triangles from vertex `3*sum(triCount[0..i-1])` with `texName[i]` on stage 0; untextured groups
  come after the textured ones.
- Init (CODE `0x1003fe10`): if the option `[0x105ff404]+0x54` is set, each vertex colour channel becomes
  `min(2c, 255)` before upload. Bounding radius `r = 0.5*|AABB max − min|`, stored as `ftol(r + 1)` clamped to
  4095 in bits 4..15 of +0x00 (`0x1003fcd0`). Format 5/6 chunks with bad data are switched to format 7 (an
  alternative layout not seen in data).
- Texture stages (CODE; lists of (stage, D3DTSS, value) applied by `0x1006aec0`; TFACTOR = element colour
  `0x1047bc0c`, set with SetRenderState 0x3C):
  - textured (`0x103524b8`): st0 colour = MODULATE2X(diffuse, tex), alpha = MODULATE2X(diffuse.a, tex.a);
    st1 colour = MODULATE2X(current, TFACTOR), alpha = MODULATE4X(current.a, TFACTOR.a).
  - textured with `elem+0x10c` bit 28 (`0x103523e0`): st0 alpha = SELECTARG1(diffuse) (texture alpha ignored).
  - untextured (`0x10352358`): colour = MODULATE2X(diffuse, TFACTOR), alpha = MODULATE4X(diffuse.a, TFACTOR.a).
  - In blend mode `'D'` (0x44, element byte p+0x16c) a TFACTOR alpha of 0x7F or more is forced to 0xFF (CODE
    0x10040170..0x1004018c in the D3m draw 0x10040150; also 0x10040605.. in the second-texture variant 0x100405a0).
    TFACTOR is the particle colour p+0xf8 with its alpha byte times the fades p+0x134 · p+0x138, truncated
    (0x10045fa0 -> 0x1003e260). Untextured with blend 0x44, the list 0x103523c4 then sets stage 0's alpha op to
    MODULATE2X (not 4X). Lighting and fog: the particle draw turns D3D lighting off (0x1003e981) and fog off only when
    the particle flags have bit 25 (0x10046ee6; otherwise fogged with the zone's fog, which xi-client does not do yet).
  - **Net, implementation-ready** (all inputs 0..1, PS2 scale; Direct3D clamps each stage to 0..1):
    textured `rgb = sat(sat(2·vtx·tex)·2·elem)`, `a = sat(sat(2·vtx.a·tex.a)·4·elem.a)`; untextured
    `rgb = sat(2·vtx·elem)`, `a = sat(4·vtx.a·elem.a)` (2· with blend 0x44). xi-client (issue #83 follow-up):
    `EffectSystem::fxStages` / `fxCombine` / `fxElementAlpha`, fx.frag. Before, it multiplied everything and clamped
    once, which washed a vertex colour above 0x80 (D3m `c8 c8 c8`, doubled D3b colours) toward white, and it did
    not force the 0x44 alpha, so the Ghrah orb's core and shell stayed half transparent at their brightest.
    Not done: bit 28's texture-alpha-off list `0x103523e0` (alpha = SELECTARG1(diffuse)).
- Blend: from the element (G.12 table). Additions from this pass (CODE `0x10046710`): ALPHABLENDENABLE (0x1B)
  is set TRUE before the switch; modes 0x41/0x42/0x43/0x62 go to `0x10046480`, whose fallback (caps bit
  `[0x1047bfa8]+0xdec & 0x800`) is SRCALPHA/ONE with BLENDOP (0xAB) = REVSUBTRACT, i.e. **subtractive**; the
  main path copies the back buffer (screen-size textures) — INFERRED distortion/subtractive. Mode 0x62 also
  sets the TFACTOR alpha to the high byte (not doubled). The default 0x48 is written at element creation
  (CODE 0x1005140c); a generator record sets it (handler 0x10051c35, = G.* op 0x1e).
- D3m drawn by an element in mode `'I'` (0x49) uses another pass in `0x10040150`: TFACTOR = colour |
  0x00FFFFFF, SRCBLEND = DESTCOLOR, DESTBLEND = SRCALPHA (CODE).

### M.3 D3a (type 0x21): textured quad list, multi-group

CODE `0x1003e5e0`. DATA: all 222 D3a chunks in files 0, 2800–3099, 37836–37899 walk exactly.
```
+0x00 s16  uvMode (0/1; always equal to byte +0x07 in data)
+0x02 s16  nGroups (1..58)
+0x04 u8   noGeometry: if nonzero only the 16-byte name at +0x08 is kept, no vertex buffer (0 in data)
+0x05 u8   INFERRED frame count (usually == nGroups)
+0x06 u8   INFERRED loop length (16, 24, 32 when +0x05 = 16)
+0x07 u8   uvNormalized: if uvMode > 0 and this is 0, u/v are divided by the texture width/height
           (never triggered in data: UVs are already 0..1)
+0x08 char texName[16]
+0x18 Group[nGroups]:  u8 ?, u8 ?, u8 nQuad, u8 ?   (DATA 01 00 n 00)
                       Vertex[6*nQuad], 24 bytes: f32 x, y, z;  u32 colour (80 80 80 80);  f32 u, v
```
- Each quad is 6 vertices = 2 triangles (triangle list). Copied into a 36-byte FVF 0x152 buffer with normal
  (0,0,1) (CODE).
- DATA 37836 `0110`: 1 group, 1 quad, x −4..3.94, y −1..0.94, z 0.
- INFERRED: groups are animation frames chosen by the element at draw time (not traced). First
  implementation: draw group 0, or cycle groups.

### M.4 D3b (type 0x25): morphing indexed mesh (CODE + DATA, 2026-10-18; RE notes d3b_re.md)

CODE: builder 0x1003f7d0, draw 0x1003f370; particle class 0x1003faa0 (generator resource type 0x1d). DATA: all 57
chunks (56 in files 2800-3099, Selbina `kamo`) decode with these rules.
```
+0x00 u16 1     +0x02 u8 bits 0..6 = frames (morph targets, 1..5), bit 7 = alpha test (GREATER 0x60; unused in data)
+0x03 s8  64 KiB bias for the index offset: idxOff = u16 @+0x08 + (s8 @+0x03 << 16)
+0x04 s16 nPos   +0x06 s16 nNrm   +0x0A u16 nTri   +0x0C u16 colour offset (u32 per corner)
+0x0E u16 UV offset (2 f32 per corner; + 0x10000 when colOff >= uvOff)   +0x10 char texName[16] (spaces: untextured)
+0x20 frames: f32 xyz[nPos], then u32 normals[nNrm] packed 10:10:10 signed / 512
idxOff: u16 posIdx[3 nTri], then u16 nrmIdx[3 nTri]
```
- A plain triangle list: corner c = position[posIdx[c]] (blended over the frames), UV and colour from its own arrays.
- Colours: every byte doubled and clamped on the CPU (0x80 -> 0xFF), then the textured D3m combiner (§M.2).
- **Morph:** the particle's five weights (L2 ops 0x1e-0x22, each from the keyframe channel set by L1 ops 0x33-0x37,
  {+8 keyframe, +0xC mode}; t = fmod(n x age, 1) with n = mode bits 5..13, else the age) blend the frames,
  normalised; all zero = frame 0.
- DATA: `kamo` (Selbina, the seagull; 2 frames, wings up / level, 29 tris), 2802 `kn1 `, 2811 `cyou` (a wave fan),
  2843 `p1  ` (Protect's hexagonal shell), `kyu` 2984-2988 (4 frames).

### M.5 Afb (type 0x54): weapon trail

Played by scheduler op 0x2C. Looked up by the **first 3 characters** of the record name in the **caster's**
loaded files (`0x100ce3b0(actor, 0x54, name & 0xFFFFFF, 0xFFFFFF)`, then actor vtable+0x1e8), i.e. in the
weapon-skill motion file (fileA, dat-format §10.8), not in the scheduler's file. CODE `0x1005a362..`.

Layout (CODE `0x1001ced0`, `0x1001cf30`, `0x1001cf80`, `0x1001bbb0`; DATA 37836 `b0a0`):
```
+0x00 u8   nSamples (5)
+0x01 u8   samplesPerSecond (30 => one sample per motion frame); sample i is at frame first + i*30/this
+0x02 u8,u8 ? (41 40)
+0x04 f32  scaleX (1/128)   +0x08 f32 scaleY (1/128)   +0x0C f32 scaleZ (1/128)
+0x10 u8   firstFrame (14)
+0x11 u8   ? (18)
+0x12 u8   motionFrames (44)   converts the record duration into motion frames
+0x13 u8   ?
+0x14 u32  offset of edge-A array (0x20), from payload+0; made absolute at init (+0x1C := 1)
+0x18 u32  offset of edge-B array (0x48)
+0x1C u8   runtime "relocated" flag (0 in file)
+0x1F u8   version; must be 0 ("Afb Version Error")
+0x20 EdgeA[nSamples]: s16 x, y, z, w(128)  -> (x*scaleX, y*scaleY, z*scaleZ, 1)
      EdgeB[nSamples]: same
```
Runtime (`0x1001bbb0`, CODE unless marked):
1. `t += frameDelta * rate`, `rate = (2*motionFrames / duration) * speed`, speed 0.5 or `u16 rec+0x56 × 0.005`
   (0x1005a4b9..). `t` runs through the motion's frames while the record's duration (= the strike motion's
   duration) elapses.
2. Each sample whose frame has passed is transformed by the actor's world matrix (`0x100d4950`) and pushed into
   an 80-entry ring (edge A at +0x40, edge B at +0x540, 16 bytes each). INFERRED: A/B are the two long edges of
   the swept blade (base and tip), in model space.
3. Between samples the ribbon is subdivided 4× by a spline (`0x1001c810`, step 0.25).
4. Each ring entry has RGBA per edge (+0xA40 / +0xF40, floats). Edge A starts at the record's colour A, edge B
   at colour B; each tick alpha A −= rec+0x24 and alpha B −= rec+0x34; after rec+0x40 ticks the RGB fades too
   (INFERRED exact order).
5. Render (CODE 0x1001ca2a..): SRCBLEND = SRCALPHA, DESTBLEND = ONE (**additive**), CULLMODE = NONE; coloured
   quad strip between edge A and edge B, no texture seen.

---------------------------------------------------------------------------------------------

## Scheduler ops

### S.1 Container (type 7)

- Payload header (DATA, every scheduler seen): `+0x10 u32 0x40`, `+0x14 u32` = record start + 0x10 (always 0x50
  → records at payload+0x40), `+0x18 u32` = payload size, `+0x1C u32` = total length in ticks (= sum of all
  record delays), `+0x31 u8 = 1`.
- Record: `u8 op`, `u8 lenDwords` (`(w >> 8) & 0x1F`), `s16 +4 delay`, `s16 +6 duration`, op data from +8.
  Record length is fixed per op (table, DATA). Op 0 ends the list; op 1 starts every list (no-op).

**Timing — correction to dat-format §10.8** (CODE driver `0x10057d5e..0x10057ed3`). Each frame:
`wait -= frameDelta` (`[0x1047bfa8]+0xeb0`). While `wait <= 0` and records remain:
`wait += record.delay * rate; execute(record); advance`. So **a record executes as soon as it is reached and its
delay is the gap before the next record** (post-delay). A handler may return 1 (task deleted) or 2 (stay on the
current record pointer, used by flow-control ops). `rate` = `task+0x9c`, 1.0, or 0.7 for multi-target
sub-schedulers started by op 0x73. Handlers receive `duration * rate` (`0x1005e4c0`).

Check (DATA): with post-delay timing the 37836 `main` starts the strike motion `b02?` (dur 70) and the trail
`b0a?` (dur 70) on the same tick, which matches the trail code mapping its duration onto the motion's 44
frames. With pre-delay timing the trail would start 20 ticks late.

### S.2 Caster, target and name lookup

- A scheduler task holds **caster** (`+0x0C`, `0x1003b600`) and **target** (`+0x10`, `0x1003b630`). Handlers use
  `0x100626a0`/`0x10062700`, which skip `CXiDollActor` proxies to the real actor (CODE). The parsed 0x028
  action (`task+0x90`) is shared with sub-schedulers.
- Name lookup (`0x100721c0` → `0x100721f0`, CODE): siblings in the scheduler's own Rmp block, then each
  enclosing block to the file root — each one **searched depth-first through all its sub-blocks** (the block just
  searched is marked at +0x38 and skipped), so file order within the subtree (2026-10-20: a Lower Jeuno lamp's
  `_l00/ligh/evst` starts `lton` from `_l00/lt08`; searching only the enclosing blocks' own resources fell through to
  the first `lton` in the file, so every lamp started the same `RR10` / `sw10` glow), then the global root (`[0x1047d128]+0x40008`) holding every loaded DAT,
  including the always-loaded **common effect file 0** (ROM/0/0.DAT, block `syst`: `effe` = 132 schedulers,
  320 generators, 45 D3m; `acti` = action logic). A WS file's own `hit1` therefore shadows file 0's `hit1`.
- "In the caster/target" lookups search the actor's loaded model/motion files (`0x100ce3b0(actor, type, name,
  mask, index, flag)`, actor vtable+0x1ec/+0x1e8).

### S.3 Op table

Len = record length in dwords (DATA, ~85k records in files 0, 2800–3099, 3400–3699, 3900–4099, 4412–4599,
4912–4999, 8392–8499, 9673–9699, 33227–33482, 37836–38089; "?" = op not seen in that set). Dispatch: jump
table 0x1005db4c (op − 2). No handler (no-op): 0x31, 0x33–0x3A, 0x3E, 0x4B–0x50, 0x61, 0x63, 0x66, 0x68, 0x69,
0x6C, 0x6D, 0x74, 0xA2 (0x31, 0x3E, 0x69 serve as labels).

**Generators, schedulers, flow**

| Op | Len | Fields | Meaning | Basis |
|---|---|---|---|---|
| 0x00 | 2 | | end of list | CODE |
| 0x01 | 2 | | start marker | DATA |
| 0x02 | 4 | +8 generator name, +0xC cache | **Start generator** with (caster, target): own block → global lookup. An existing instance is re-triggered (`0x10053930(c,t,1)`), else `0x100535f0(c,t,1)` spawns it. Attachment (caster/target/bone) and offsets are in the generator (G.4). Names `coll`, `sd03`, `sd01` are skipped when `task+0x146 == 1` | CODE |
| 0x1E | 4 | +8 name | **kill** that generator's running instances now (`0x10053810`, `0x10053930(..,0)`); which instances: see §C.3 (#113) | CODE |
| 0x2D | 4 | +8 name | **stop emitting** (`0x100537c0(0)`; live particles finish); same instances as 0x1E | CODE |
| 0x3F | 7 | +8 name1, +0x10 name2 | stop generator 1, start generator 2 | CODE (partial) |
| 0x6F/0x70 | ? | +8 name, +0x10 | task (0x4C) on a running generator, flag 1/0 | CODE, meaning unknown |
| 0x03 | 4 | +8 name, +0xC cache | **start sub-scheduler** with the same (caster, target) (`0x10056ae0`), in parallel | CODE |
| 0x73 | 4 | as 0x03 | start via `0x10056ba0(c, t, 1, action)`: rate 0.7 when the action has >1 target (type ≠ 14/15) | CODE |
| 0x09 | 4 | +8 name | start scheduler found in the **target's** files (else global) with caster/target **swapped** | CODE |
| 0x57 | 4 | +8 name | start scheduler found in the **caster's** files (vtable+0x1ec, `0x100ce3b0`, else global), same roles | CODE |
| 0x85 | 4 | +8 name | as 0x57 with root (non-doll) actors (`0x10056e20`) | CODE |
| 0x3C | 4 | +8 name | start from the caster's files if caster vtable+0x118 == 1, else own file; then yield | CODE |
| 0x3B | ? | +8 name | start scheduler (`0x10056ba0`, roles swapped) and yield | CODE (partial) |
| 0x5F | 4 | +8 name | kill every running instance of that scheduler (`0x10056de0`; corrected in §C.3) | CODE |
| 0x30 | ? | +8 name | run the scheduler once **per action target** (AoE) | CODE |
| 0x24 | 2 | | choose and start the caster's **attack scheduler** for the current result (`0x100d6780`; see Melee) | CODE |
| 0x5D | ? | | same, 8-direction `'at'` variant | CODE |
| 0x5C | 3 | | start counter scheduler `cni0`/`cnr0`/`cnb0`/`cnl0`/`cnf0` from the caster (4CCs stored as floats) | CODE |
| 0x76/0x77 | ? | +8 mode | start `lcNN`/`lsNN`, NN from caster vtable+0x3d8 | CODE |
| 0x84 | ? | +8 s16, +0xA s16 | start draw/sheathe `in10`/`ot10`/`in20`/`ot20` + sheath-style digit | CODE |
| 0x19, 0x87 | 3 | +8 u32 | caster vtable+0x2c4(rec+8, target, `'main'`); INFERRED: run `main` of another effect | CODE (call) |
| 0x31 | 2 | | label for 0x32 | CODE |
| 0x32 | 2 | | **next result**: advance the action's result index (`0x1018cd20`; byte +3 over a flat (target, result) list, stride 0x34, count at +0x1C); if another result exists, set the target to it (`0x100627b0`), take back 0x32's own delay, add the 0x31 label's delay and continue after the label; else wait = 0 and fall through (handler `0x1005d80d`). Spells loop `tgt0` over every target this way (2801 `main`: 0x31, 0x03 `tgt0`, 0x32), so AoE targets are 4 ticks apart; melee loops hits | CODE |
| 0x3D/0xAF | 2 | | **random branch**: execute one random record among those up to the next 0x3E (`0x1006be90(n)`) | CODE |
| 0x52 | ? | +8..+0x14 | timed wait on a sound clock; returns 2 | CODE (partial) |
| 0x64/0x65, 0x67, 0x69, 0x6A, 0x6B | 2,2,2,2,3/4 | | expression VM (S.5) | CODE |

**Motions and actors**

| Op | Len | Fields | Meaning | Basis |
|---|---|---|---|---|
| 0x05 | 10 | +8 motion; +0x10 f32; +0x14 f32; +0x18 u16 blend-in; +0x1C u16 blend-out; +0x1E u8&0x7F; +0x1F>>4 mode | **caster plays motion** (dat-format §10.8); speed scaled so it fills `duration` | CODE |
| 0x06 | ? | as 0x05 | **target plays motion** | CODE |

Client (xi-client): op 0x2B (file 0 `mdam`, `ldad`) releases the current result's battle messages; finishes of types 2-6, 11 and 13 hold theirs until then (4 s at most), melee rounds log at once. Verified live: Berserk's line appears 1.05 s after the packet, with `mdam`. Coverage (DATA, following ops 0x03/0x30/0x3C/0x57/0x73 within the file only): 298 of 300 magic `main`s reach 0x2B or `mdam` (not 2920, 3094), 280 of 287 abilities (not 4608, 4615-4618, 4681, 4692), 63 of 87 items (not 4928-4935 ...; some may reach it through the caster's files, which this count does not follow). A finish whose effect fails to start logs at once.

Client (xi-client): ops 0x21/0x25 trigger the damage flinch (instead of the 0x028 packet's arrival), so a melee target flinches at the hit's contact through file 0 `dada` → `dam0` → its own `damg` (op 0x09, roles swapped) → op 0x21 (~0.58 s after the packet, live; file 0 `hit1` is only sparks) and spell targets when `tgt0` lands; heals (Cure's `tgt0` has no 0x25) do not flinch.

Client (xi-client): ops 0x05/0x06 queue a motion (stem = the name without its `?` piece wildcard) that the actor plays once, for item use and job abilities only (types 5/6; melee, WS, casting and TP moves animate from their own state). Weapon skills and TP moves read the same op 0x05 records from their file's `main` up front (dat-format §10.8): every motion plays, each at its tick, stretched to its duration (#112). DATA: job abilities 4412+ use `cm0?` → `cm1?` (152/149), `gu0?`–`gu2?` (34); items 4912+ `mi3?`, `ri0?`. Verified live: Berserk plays `cm0` then `cm1` (Galka, 3 pieces each).
| 0x28 | 3 | +8 f32 | caster motion chosen by `0x100c8ae0` | CODE (partial) |
| 0x21 | 9 | +8 f32 (× factor), +0xC f32, +0x10 int, +0x14 f32, +0x18 f32, +0x1C dir | **damage flinch on the caster**, facing the target. +8..+0x18 go to the motion player `0x100cd850` (INFERRED: speed, ?, loop, blend-in, blend-out) | CODE 0x1005a5e6 |
| 0x25 | 9 | same | **damage flinch on the target**, facing the caster. Motion `dfi?` (front) / `dbi?` (behind); `dfm?`/`dbm?` when actor `+0xf8` is set (INFERRED engaged). dir 0 = by facing (`0x100ce0b0`), 1 = front, 2 = back. +8 × {0, .25, .5, 1}[n] (0x1035d748), n = `0x1005e280()` clamped to 3 | CODE 0x1005a636, 0x100cdc90 |
| 0x5A/0x5B | ? | +8, +0xC, +0xE, +0x10, +0x14 | caster guard motion `gdi?`/`gdm?`. DATA: the defender's `pary`/`gurd` (run by file 0 `dam0` with op 0x09, roles swapped) are just this op; `sway` (miss) only plays the voice `vswy`, `damg` = op 0x09 `chit`, 0x57 `sdam`, **0x21** (flinch), 0x57 `vdam`. Galka files hold `gdm1` (26353/26354) and `gdi6` (26355/26356): what picks the `?` digit is unknown; xi-client loads stem `gdm`/`gdi` by engagement (INFERRED, not seen live) | CODE |
| 0x5E | 6 | +8 s16, +0xA s16 level, +0xC f32 dist/60, +0x10, +0x14 | **knockback** of the caster: motion `bf0?`/`bb0?`, distance from table 0x1035bc70 by level, or explicit | CODE |
| 0xBF | ? | as 0x5E | knockback as a timed task | CODE |
| 0x62 | 3 | +8 f32 deg/tick | caster turns to face the target | CODE |
| 0x26/0x27 | ? | +8 Vtx (0x3E) name, +0x14 KeyFrame name, flags +0x10 | move caster/target along a path (`CMoPathDriveActorTask`) | CODE |
| 0x7A | 12 | +0x10 vector, +0x20 | caster→target movement task (jumps) | CODE (partial) |
| 0x29/0x2A | 4 | +8 u8 R, G, B, A (0x80 = 1.0) | **tint caster/target** from its current colour to this over `duration` (`CMoActorColorDriveTask`). Channel order from the added-effect flashes `atpr` starts by var 0x2F (LSB subEffect): wind `hit9` `80 a8 80 80` green, earth `hit8` `ca ab 61 80` yellow, water `hi10` `60 80 c0 80` blue, each back to `80 80 80 80` at t40 (DATA, INFERRED order). Alpha: `kesu` → A 0 (vanish), `pop1` 0 → 0x80. Client: multiplies the character's material colour; A < 0x80 dithers it out | CODE |
| 0x46/0x47, 0xA0/0xA1 | 4 | as 0x29 | colour drives through the same task (`CMoActorColorDriveTask`, 0x1005f5f0) with a slot and a restore flag. 0x29 / 0x2A: slot 0, the body colour (actor +0x660, vfunc +0x2c get / +0x30 set). **0x46 / 0x47 (caster / target, skeleton actors only): slot 1, actor +0x74c (vfunc +0x40c / +0x60), beside the texture overlay's +0x750: the overlay's colour, not the body's** (CODE 0x1005b15b / 0x1005b1d4). 0xA0 / 0xA1: slot 0 with +0x86 = 1, so the task's end (0x1005f8c0) sets the colour it saved. DATA: Hundred Fists (4446 `scal`) fades its `hit3` overlay in (`808080ff`) and out (`00808080`); teleports (2952 `tsp0`, 2953 `trk0`) colour their `ice ` overlay. xi-client: 0x46 / 0x47 colour the overlay pass (INFERRED: how the draw applies it, multiplied here); before issue #76 they multiplied the body colour and Hundred Fists left the caster invisible | CODE |
| 0x42/0x43 (caster), 0x44/0x45 (target) | 4 | +8 f32 | interpolate an actor float over `duration` (vtable+0x414 / +0x41c); INFERRED alpha / scale | CODE (call) |
| 0x40/0x54 (caster), 0x41/0x55 (target) | 4/5 | +8 TexInfo name, +0xC | texture overlay on the actor for `duration` (`0x100616e0`) | CODE, INFERRED |
| 0x07/0x08, 0x1F/0x20, 0x59, 0x2E/0x2F, 0x88/0x89, 0x48/0x49 | 2–4 | | duration-bound caster/target control tasks (vtables 0x1032bb54, 0x1032bb0c, 0x1032c048, 0x1032bfb8, 0x1032c000, ...). Meanings since traced (Coverage tables): 0x07 / 0x08, 0x1F / 0x20 / 0x56 and 0x59 the actor busy count, 0x2E / 0x2F motion hold, 0x48 / 0x49 the refraction pass, 0x88 the colour reset on early delete | CODE |
| 0x56 | 3 | | op 0x20 for every action target | CODE |
| 0x15/0x22 (caster), 0x16/0x23 (target), 0x9C/0x9D | 2/3 | | **doll** of caster/target (CXiDollActor, not drawn); 0x22/0x23 flag 1 = follows the actor's position | CODE |
| 0x17/0x18 | 2 | | remove caster/target doll | CODE |
| 0x1A | ? | +8 name, +0x18 f32 xyz, +0x24 f32 yaw, +0x28 file id | spawn a model actor from a DAT | CODE |
| 0x1B/0x1C | | +8 name | show / remove it | CODE |
| 0x75/0xA3 | 4 | +8, +0xC s16 | caster `0x100d5e40`/`0x100d6010` | CODE: +8 != 0 hides part `wep0`+N, 0 shows it |
| 0x79, 0x8C, 0xA4, 0xA5, 0xAB, 0x8D–0x9A, 0x9E, 0x9F, 0xA7, 0xA8, 0xAC, 0xAD, 0xB7–0xBD, 0xC1, 0xC2 | | +8 | caster property setters (virtual calls / tables 0x1032b804…) | CODE |

**Sound**

| Op | Len | Fields | Meaning | Basis |
|---|---|---|---|---|
| 0x0A | 8 | +8 Sep (0x3D) name, +0x10 flags (bit 8), +0x14 f32, +0x18 | **sound at the caster** (`0x10037000(pos, +0x18, +0x14, 0, 64.0, 0, 0, 0x7F)`); for `shit`/`skaz` the last byte becomes `'0' + (actor vtable+0xa4 & 3)` if that variant exists | CODE |
| 0x0B | 8 | same | **sound at the target** | CODE |
| 0x53 | 8 | same | at the action target nearest the caster | CODE |
| 0x60 | ? | +8 name | non-positional sound | CODE |
| 0x8A/0x8B | ? | +8 name, +0x20 slot | start sound into slot 0..7 / stop slot | CODE |
| 0x78, 0x7B, 0x7C, 0x7D, 0xAE, 0xC0 | | | other sound-system calls | CODE (partial) |

INFERRED: +0x14 is the audible distance (DATA 30.0, 60.0). Sep lookup: own file first, then global.

**Screen, camera, environment**

| Op | Fields | Meaning | Basis |
|---|---|---|---|
| 0x0E | +8 u8×4 colour | screen colour fade over `duration` (singleton 0x1047bf20; from `[0x104568fc]+0x32c`) | CODE |
| 0x0F, 0x51, 0x72 | +8 u8×4 | three more screen-colour channels (0x1047bf28/2c/30) | CODE |
| 0x10 | | screen-capture effect (render-target copies; INFERRED blur) | CODE |
| 0x0C/0x0D | +8 f32 x, y, z; +0x14 u32 part | door part translation (0x0C) / rotation (0x0D, radians) of the caster's door: part i goes linearly from its current value to (x, y, z) over `duration` (tasks `0x10043ae0` / `0x100438f0`, tick `0x10043a30`; dat-format §5.8). Not camera tasks | CODE, live (zone 230 doors) |
| 0x82/0x83 | +8..+0x14 | save + drive camera parameters / restore them | CODE |
| 0x58 | | reset view (FOV constant 280 or 350) | CODE |
| 0x7E | +8 Weather (0x2F) name | pass that Weather payload to `0x1018af20` (INFERRED: switch zone lighting/fog) | CODE (call) |
| 0x11–0x14, 0x1D | +8 | unknown target/caster tasks | CODE |

**Action results**

| Op | Len | Meaning | Basis |
|---|---|---|---|
| 0x2B | 3 | **commit the next result**: mark it shown and call `0x1009dfc0` (battle message, damage number, HP). The common `mdam` scheduler is only this op | CODE `0x1018c720` |
| 0x71 | 3 | commit with kind +8 (DATA: 1 after an added effect, 2 after spikes) | CODE `0x1018c780`, INFERRED kinds |
| 0x80 | ? | if the first result's +0x18 ≠ 0: `0x100a8d60` | CODE |
| 0x2C | 22 | weapon trail (S.4) | CODE |

### S.4 Op 0x2C record (CODE `0x1005a362..0x1005a5cc`)
```
+0x08 name     first 3 chars select the Afb in the caster's files ('b0a?' -> b0a0)
+0x0C          cache slot
+0x10 u8[4]    colour of edge A (r,g,b,a; 0x80 scale)
+0x14 u8[4]    colour of edge B
+0x18 f32[4]   per-tick fade, edge A (rgb, a)       -> trail +0x14ec
+0x28 f32[4]   per-tick fade, edge B (rgb, a)       -> trail +0x14fc
+0x38 f32      length threshold (vs |A−B|)          -> +0x150c
+0x3C f32      second threshold                      -> +0x1510
+0x40 f32      hold ticks before the RGB fade        -> +0x14c8
+0x44 u32      mode (1 in data; 0 skips the second pass at 0x1001ca6c)
+0x48 f32[3]   ? (0.1, 0.1, 0.1)                     -> +0x1520
+0x56 u16      speed × 0.005 (0 => 0.5)
duration       ticks over which the Afb's motionFrames play
```
Creates a 0x1550-byte trail object (vtable 0x1032a308) on the caster and a 0x7C task (`0x10060a60`) that
removes it after `duration`. A missing Afb logs the string at 0x1035395c.

### S.5 Expression VM (ops 0x64–0x6B)

Used by file 0's `dam0`, `atpr`, `crtl`, `nrm0` (CODE case 0x6B at 0x1005d18e; variables `0x10062830`).
- 0x69 opens a block, 0x6A closes it, 0x67 = else; 0x64/0x65 pop a value and if it is 0 skip to the matching
  0x67/0x6A; 0x6B pushes a token, sub-op = u16 +8: `0x1C` load (u16 +0xA: 1 = int at +0xC, 2 = float,
  3 = variable index at +0xC), 5–8 arithmetic, 9 `<<`, 10 `>>`, `0x14..0x18` stores. Comparisons (CODE, jump
  table `0x1005deb0`, setcc in `0x10068e20`..`0x100690f0`; left = the deeper stack entry): `0x0B` !=, `0x0C` ==,
  `0x0D` >=, `0x0E` <=, `0x0F` >, `0x10` <. `0x11` is a **bitwise** AND (`and esi, eax` at `0x1005d611`), `0x12` OR.
- DATA: `atpr` = `wapr` if var 0x2F > 0, then `hit3`/`hit5`/... by var 0x2F == n; `crtl` = if (var 0x2B & 2)
  { var 0x38 ? `hi14` : `hi29` }; `dam0` = `sb0n` by var 0x33, `cnt0` if var 0x33 == 0x3F, then by var 0x28
  (0 `damh`/`damg` by var 0x3B, 1 `sway`, 2 `gurd`, 3 `pary`, 4 `gur1`), so **var 0x28 is LSB's resolution**.
- Variables: 0x11–0x20 task locals; 0x21–0x25 action header; 0x27 target count; **0x28–0x36 fields of the
  current result** r (= target entry + 8, `0x1018cdd0`): 0x28 u32 r+0, 0x29 r+4, 0x2A r+6 (animation, CODE),
  0x2B r+8, 0x2C r+0xA, 0x2D u32 r+0xC, 0x2E u32 r+0x10, 0x2F r+0x18, 0x30 r+0x1A, 0x31 u32 r+0x1C, 0x32 r+0x20,
  0x33 r+0x22, 0x34 r+0x24, 0x35 r+0x26, 0x36 r+0x28; 4 = rand(10)−5; 0x27 target count (action +0x1C). State (getter `0x10062880..`, CODE): **0x38** = the action's actor is a PC (its entity +0x78 server id has top byte 0; `0x1018c850`), **0x39** = the caster's id top byte is 1 (zone NPC / monster), 0x3A = caster vtbl+0xA4 & 3, 0x3B = caster vtbl+0x254, 0x3D / 0x3E = caster entity +0x12C bits 13 / 22 (0x3D also on vtbl checks). So `crtl` plays `hi14` for a PC's critical and `hi29` for a monster's.
- Mapping to the 0x028 result (protocol-map §7.2), INFERRED from field order and use (only r+6 is confirmed):
  r+4 kind, r+6 animation, r+8 info (low 5 bits of LSB "speceffect"; 2 = critical), r+0xC param,
  r+0x10 message, **r+0x18 added-effect effect**, r+0x1C its param, r+0x20 its message,
  **r+0x22 spikes effect**, r+0x24/+0x26/+0x28 spikes info/param/message.

### S.6 Worked example: 37836 (Galka Fast Blade), post-delay timing (DATA)

`main` (t = tick the record executes):

| t | op | effect |
|---|---|---|
| 0 | 0x20 dur 100; 0x59 dur 90; 0x2E dur 90 | target/caster control tasks |
| 1 | 0x05 `b00?` dur 35 | windup b000+b001+b002 |
| 10 | 0x03 `eis1` | file 0 `eis1` (block `eisy`): generators `ge1s`, `ge10`–`ge17`, `g018`, `g019` — the WS charge glow |
| 35 | 0x05 `b02?` dur 70 | strike b020+b021+b022 |
| 35 | 0x2C `b0a?` dur 70 | trail from Afb `b0a0`; colour A (8A 8A 8A 40), B alpha 0; alpha −2/tick; hold 20 |
| 55 | 0x0A `8050` | swing sound at the caster (Sep in this file) |
| 60 | 0x02 `g004`; 0x23; 0x03 `hit1`; 0x0B `8051` | generator, target doll, **this file's** `hit1`, impact sound at the target |
| 65 | 0x03 `hit2` | this file's `hit2` |
| 80 | 0x03 `mdam` | file 0 `mdam` = op 0x2B: damage number/message appear |
| 100 | 0x03 `proc` | file 0 `proc` (empty) |
| 152 | end | = header +0x1C |

`hit1`: t0 op 0x25 (target flinch; 0.5, 1.0, 1, 2.0, 10.0, dir 1), `g002`, `g000`, `g003` (dur 10); t5 `g001`.
`hit2`: t0 op 0x25 (0.7, 1.0, 1, 2.0, 32.0, dir 2), `g013` (dur 10), `g012`, `g010`; t7 `g011`. The D3m
`0112`/`0113` and D3a `0110`/`0111` are drawn by these generators.

---------------------------------------------------------------------------------------------

## Melee hit effects

There is **no per-hit effect file** for auto-attacks (0x028 type 1: `0x100a8c20` returns base 0 for it, CODE).
A round is driven by schedulers in the weapon-class motion file, the weapon model file and file 0 (CODE + DATA):

1. **Attack selection.** The weapon-class battle file (dat-format §10.6; Hume male 9673 + class) has `atk0`:
   op3 `hwat`, op57 `vatk`, **op 0x24**, op 0x32. Op 0x24 (`0x100d6780` → `0x100d6870`) builds the swing name
   and looks it up in the caster's files:
   - byte 0 `'a'`; `'b'`/`'c'`/`'d'` when r+4 == 1 and animation (r+6) == 1/2/3 (INFERRED off-hand, kicks);
     animation 4 → `ls05`;
   - byte 1 `'t'`, or `'1'`/`'2'`/`'3'` from `"t123t123"` by `actor+0x70 → +0x124 >> 1 & 7` (values 1–3, 5–7);
   - byte 2 `'i'`; when `actor+0xf8` is set, `'f'`/`'r'`/`'b'`/`'l'` from `actor+0x598`;
   - byte 3 random among existing variants (`rand() % count`, `0x100ce6b0` counts them).
   Op 0x32 then loops to the next result (multi-hit rounds).
2. **Swing** (e.g. 9674 `ati0`): op 5 `at0?`; op 0x20 holds the target; at the contact tick op 0x57 `skaz`; at
   the end op 3 `dada`.
3. **Weapon model file** (Hume male 8392–8903, all 512, DATA): `skaz` (swing sound `skaz`), `ef h` = op3 `hit1`,
   `se h` = sound `shit` (impact; material digit appended), `sinr`/`sotr` (draw/sheathe sounds). **Every weapon
   uses the same `ef h → hit1`**; only the sounds differ.
4. **File 0, block `acti`** (DATA):
   - `dada` = op3 `atpr`, op3 `crtl`, **op 0x2B** (damage number/message), op3 `dam0`.
   - `atpr` (added-effect visual) on var 0x2F (INFERRED added-effect id): 0 `wapr`, 1 `hit3`, 2 `hit5`,
     3 `hit9`, 4 `hit8`, 5 `hit4`, 6 `hi10`, 7 `hit6`, 8 `hit7`, 9 `hi15`, 10 `hi16`, 11 `hi17`, 12 `hi18`,
     13 `hi19`, 14 `hi20`, 15 `hi22`, 16 `hi21`, 17 `hi23`, 18 `hi24`, 19 `hi23`, 20 `rifu`, 21 `hi26`,
     22 `hi30`, 23 `hi31`.
   - `crtl` (critical): if var 0x2B == 2 → var 0x38 ? `hi14` : `hi29`.
   - `dam0` on var 0x33 (INFERRED spikes id) 1..10 → op 9 `sb00`..`sb09` (spike visuals, ending with op 0x71
     kind 2); then by var 0x28/0x3B → op 9 `damh` or `damg`.
   - `damh` (op 9 swapped roles, so its caster is the hit actor): op 9 `chih`, op 0x57 `sdam`, op 0x21 (flinch
     of the hit actor; 1.0, 1.0, 2, 1.0, 24.0, dir 0), op 0x57 `vdam`.
   - `chih` (roles swapped back, caster = attacker): op 0x57 `efhl`, `sehl` from the attacker's files.
     INFERRED: actor vtable+0x1ec maps `efh?`/`seh?` (l/r hand) onto the weapon's `ef h`/`se h`.
   - `hit1` (block `hit1`): generators `g012`, `g010`, `g011`, then `g013` at t10 — the hit spark.
     `hi10` also tints the target (op 0x2A `60 80 c0 80` → `80 80 80 80`).

xi-client (2026-10-18) runs this chain: the type 1 cmd_arg (`atk0`) from the attacker's files (a PC's weapon-class
battle files first), op 0x24 picks `atf?` (else `ati?`; `b/c/d` for kind 1 animation 1-3, `ls05` for animation 4) at
random among the existing digits, op 0x32 loops the results. `chit` / `chih` ask for the weapon model's `ef h` /
`efh?`: a PC's main weapon model file is now among its effect files (BotA's 27921: `skaz` se006042, `se h` →
`shit` se006043, `ef h` → `hit1`, verified live); without it they fall back to file 0 `hit1`. A parry ran the
defender's `pary` → op 0x5A → `gdm` (1 piece) live. Verified live: BotA (Galka) `atk0`
from 28954 → `atf0` → `dada` (+0.73 s) → the hare's `damg` (flinch, `sdam` se006003, a random `dam?` cry) and
`chit` → `hit1` (+1.07 s); the hare's own swing plays `skaz` (se006062). The old per-result `hit1` + `dada` path
remains for attackers whose files are unknown.

**PC voices, impact and draw / sheathe sounds (2026-10-19).** The op 0x57 names a PC's schedulers ask for live in
more of its model files than the race / battle / weapon ones (DATA, every race): the **face** file (slot 0, e.g.
Galka 26360 + face) holds `vatk` (attack voice, from `atk0`), `vdam` (hurt voice, from `damg`), `vswy` (dodge voice,
from `sway`) and `vded` (from `dead`); the **body** file (slot 2, e.g. Galka 26648..) holds `sdam`, the impact sound
when the PC is hit (its armour's: only body files carry it, 255 of the Galka slot ranges). The battle files hold
`in N` / `out N` (op 5 `inc?` / `otc?` draw / sheathe motion, ops 0x2F 0x2E 0x07 0x1F, op 0x57 `sinr` / `sotr`), and
the weapon model file `sinr` / `sotr` (op 0x0A, the Sep of the same name). The status machine starts `in` + '0' +
max(0, actor +0x87c) on engaging (CODE 0x1008b681 -> 0x100d2e00, then vtable +0x298) and `klbt` (stops `bntg`),
`out` + digit, `hwat` on disengaging (0x1008b7b5 / 0x1008b895 -> 0x100d2e20). xi-client: a PC's effect files are
now race + battle + weapon + face + armour (head..feet; the order after the weapon is INFERRED), and the engage /
disengage change runs `in N` / `out N` (not on dying); their motion op stays with the animator, so they add the
sound. Verified live (BotA, disk audio): `sinr` se006065 at the draw, `sotr` se006066 at the sheathe, `atk1` /
`atk2` voices on its swings, `sdam` se006010 + `dam2` when hit, `swy2` on a dodge. Before, a PC hit by a monster
played only the monster's `shit`, and drawing / sheathing were silent.

Minimal melee implementation: per result of a type 1 action, play the swing motion; at the contact tick spawn
file 0 `hit1` (g010–g013) at the target, flinch the target (`dfi?`/`dbi?`), play the weapon's `shit` sound and
show the damage (op 0x2B). Add `hi14`/`hi29` on criticals, and the `atpr`/`dam0` tables for added effects and
spikes.

### Coverage and the ops traced 2026-10-18 (CODE, FFXiMain.unpacked.dll)

`src/client/effect_ops.h` gives every (list, op) a status: handled, game no-op, no visual, unhandled. `xi-dat fxops
FILES|FROM-TO` counts every op in the files' schedulers and generators against it (battle set 0, 2800-3099 ...: 1,722
files); `xi-dat fxsched FILE NAME|gen:NAME|*` dumps records. xi-client logs `fx: unhandled ops: ...` at exit.

| Op | Meaning | Basis |
|---|---|---|
| sched 0x15 / 0x16 (0x22 / 0x23) | doll of the caster / target (0x100abdd0): copies position, rotation, 3 floats and the 48 attach points; the scheduler's caster / target becomes the doll, so what starts next stays where the actor was. 0x22 / 0x23 (and 0x9D) set the doll's follow flags +0x424 / +0x425 / +0x426 and register it with the actor manager (0x10074920, global 0x1047d140) so it is updated each frame: its update (vtable 0x1032f938 +0x20 = 0x100ac020) copies the source's x, y, z and moves the 48 attach points by as much, so effects on it go with a moving actor (pose and rotation stay as copied). The doll has no model and is never drawn (corrected 2026-10-19; earlier read as an after-image). 0x17 / 0x18 restore the actor | CODE 0x100589fd, 0x100588a4 |
| sched 0x1F / 0x20 / 0x56 | task (0x78 bytes, vtable 0x1032bb0c) that raises a busy counter on the actor's entity (+0x11c, timeout 0x708 ticks) for the duration; read by 0x10083250 / 0x10095780 (state gates) | CODE 0x10060120, 0x100a4070 |
| sched 0x59 | the same task kind on the actor (+0x80 via vtable +0x160 / +0x164) | CODE 0x10062580 |
| sched 0x2E / 0x2F | motion hold: actor +0xfc counter (vtable +0x30c) for the duration, then the stance motion restarts (0x100ca8e0: idl / dl1-3 / btl / bt1-3 / cor / rx1) | CODE 0x10062240, 0x100621f0 |
| sched 0x28 | play the stance motion chosen by 0x100c8ae0 (idle / battle / rest by state) | CODE 0x1005aff8 |
| sched 0x75 / 0xA3 | +8 hide (!= 0) / show (0) the caster's model part `wep0`+N, N = s16 +0xC (monster `init` / `ini1..3`: weapon variants; 115 monster models carry several `wepN`). +8 != 0 calls 0x100d6010, 0 calls 0x100d5e40; both set a per-part byte through 0x1002c660 (0x75 byte 0, 0xA3 byte 1) to 1 / 0, and any byte set reads as hidden (0x1002b3a0, used by 0x100cf05e to run `!w02` instead of `!w01`). DATA: treasure chest `init` (model 258) shows the box parts 3 / 4 and hides the variants 5..7; Aern `init` (model 1373, `hiof` / `hion`) hides the body, then shows it 60 frames later; `rgon` / `rgof` show / hide the wrist crystals and knife (wep6 / wep5) | CODE 0x1005bd44 / 0x1005be24, 0x100d5e40 / 0x100d6010 |
| sched 0x0E | screen colour task (singleton 0x1047bf20): +8 colour, +0xC f32 drive [0x104568fc]+0x32c / +0x330 over the duration; drawn as a full-screen quad of a captured image with UV rect from +0x330 and alpha x 2 | CODE 0x10058cc4, 0x10060260, 0x1001385d |
| sched 0x10 | freeze frame: half-resolution capture of the screen (0x10456910), drawn over the scene with alpha = remaining / duration x 255 (SRCALPHA / INVSRCALPHA): a dissolve | CODE 0x10058ba2, 0x1005ff00, 0x10012f00 |
| L1 0x31 | slot.xyz = one rand_sym(rec+4) | CODE 0x10051724 |
| L1 0x32 | particle vfunc +0x34 (rec+8): the strength of type-0x22 `dist` distortion particles | CODE 0x10051a7b |
| L1 0x3c | one Gen_Tick of generator rec+4 / +8 with this particle as parent (a burst at it) | CODE 0x1004ed3e |
| L1 0x44 / 0x53 / 0x6a | a new instance of generator rec+8 attached to the particle (+0x164; ticked by L2 0x25 / 0x33 / 0x46) | CODE 0x1004ed7a |
| L1 0x3d, 0x3e-0x40; L2 0x29-0x2b | wobble: slot+3 = -atan2(dz, dx) of the spawn offset; amplitudes rec+4 + rand_sym(rec+8); per frame w = sin(ticks lived x rec+4 / 60 + rec+8) x amp x dt along (cos a, sin a) in X / Z, Y, or (sin a, cos a) | CODE 0x1004f01b, 0x1004c507 |
| L1 0x07 | 0x06's direction, x (rec+0xc, +0x10, +0x14), turned by Euler(rec+0x18, rec+0x1c, 0) | CODE 0x10051e83 |
| L1 0x72 | p+0x128 (draw bias; sort key +0x12c = it - view depth) = rec+4, scaled by a PC target's size | CODE 0x10051c53, 0x1004500f |
| L2 0x26 | slot vector x Euler(rec.xyz) each frame (vfunc 0x58 takes z, y, x) | CODE 0x1004c485 |

### Coverage, part 2 (2026-10-18; CODE, FFXiMain.unpacked.dll)

| Op | Meaning | Basis |
|---|---|---|
| file 27 `damv` 2nd generator `dam1` | the pop-up glow: the same number at scale 1.4 x 1.6, colour 96 26 26 (file bytes R G B), no op 0x1e so the default blend 0x48 (additive); drawn by the text particle 0x10041a20 -> 0x10041b50 (screen quads, FVF 0x144, TFACTOR = the particle colour) | DATA, CODE |
| L0 0x04 | emission rate v = kf(t), t = gen+0xc8 / gen+0xd0 (INFERRED elapsed / duration), rec+0x10 the curve base: interval = 60 / v ticks, extra per emission = v / 60 (flags bits 0-8), countdown = min(countdown, interval); v <= 0: countdown 32767 | CODE 0x10049940 |
| L1 0x3a | class 0x24 `ring`: p+0x194 4 radii, +0x1a4 4 D3DCOLOR colours, +0x1b4 segments, +0x1b5 rec+0x25 + 2 rings (max 4). Draw 0x10043650: per band a strip of (sin a r, cos a r, 0) vertices, colour x particle colour >> 7; 0x10042c30 sets each vertex's UV to its own screen projection over the captured scene (DAT_1045666c+0x194): the ring re-draws what is behind it, tinted | CODE 0x10051a8b, 0x10043650, 0x10042c30 |
| L1 0x32 | class 0x22 `dist`: strength for its draw (0x10043040), which also maps the scene with projected UVs; the `dist` resource (type 0x22) is not parsed, so xi-client draws a displaced camera-facing quad (INFERRED look) | CODE 0x10051a7b, 0x10043040 |
| L1 0x43 | class 0x200000 meshes (ground-projected, draw 0x10040da0): the blend of the projected pass (p+0x198, vfunc +0x38 / +0x3c) | CODE 0x100500e6 |
| L1 0x47 / 0x48 / 0x49 | a child particle's rotation = the parent particle's (p+0xe0), colour = the parent's (0x48, p+0xf8; corrected in part 7D), scale = the parent's (p+0xec) | CODE 0x1004fc92.. |
| L1 0x4c | sound particle (class 0xe): p+0x1b4 = rec+4 (the silent distance), +0x1b0 = rec+8 (full volume within; order settled in part 7D), volume 0x7f | CODE 0x10050201 |
| L1 0x68, L2 0x43 | the sound particle's level from a keyframe (p+0x1be); xi-client plays each sound once, at its starting level | CODE 0x10050381, 0x1004d5ab |
| L1 0x55, 0x59-0x5b, L2 0x35-0x37 | mesh class 0x1c0 (header flags bit 24): a second texture (TexInfo chunk rec+0x10) in stage 1 with colour rec+0x20 and flags rec+0x24; its texture matrix is RotYawPitchRoll(-angles) (p+0x19c..0x1a4, keyframed); xi-client draws an additive pass with view-plane UVs (INFERRED) | CODE 0x10051765, 0x100411e0 |
| L1 0x56 | the burst count of classes 8 / 9 / 0xa / 0xd (+0x44 = generator flags & 0x1ff); no effect on other classes | CODE 0x10051871 |
| L1 0x82, L2 0x5f | vibration: kf x slot+2 x falloff (full within rec+4 of the camera, none past rec+8) x 255 added to the pad rumble (0x10455c20+0xdb4) unless option 0x92 is off; with len 4 and rec+0xC != 0 the camera also shakes by it x rec+0xC (0x10021ad0) | CODE 0x1004c009 |
| L2 0x60 | screen flash: the particle colour x a depth window (rec+0xC..0x10) x camera-distance falloff (rec+4..8), added into [0x104568fc]+0x324 (saturating) | CODE 0x1004ab03, 0x10014f50 |
| L2 0x50 | alpha x the weather cross-fade weight of the owning YmCombineWeather (p+0x138) | CODE 0x1004da69 |
| L1 0x87, L2 0x64 | slot = the position (L1); per frame (L2) d = pos - slot.xyz, last = slot+3..5, turn = atan2(d.z, d.x) - atan2(last.z, last.x) (game space; the code rotates d by -atan2(last.z, last.x) and takes its atan2), then rotation X / Y / Z = wrap(turn x rec+4 / +8 / +0xC) for each non-zero factor, slot = pos, slot+3..5 = d. Selbina `kamo` has (-30, 0, 0): a roll about the gull's body axis X against the turn. Rotation is Euler X then Y then Z (math table vtbl+0x58 = 0x1006dc20). The game runs it once a frame (2 ticks at its 30 fps); xi-client scales the turn to a 2-tick step (INFERRED) and skips frames without movement (corrected 2026-10-19: the sign was flipped) | CODE 0x1004fda9, 0x1004b179 |
| sched 0x3B | start scheduler `name` with the same actors, from this file up or else from the caster's own files | CODE 0x1005da3b |
| sched 0x78 | play the Sep "0a" + hex(actor+0x879) + ('1' + actor+0x87a) from the caster's files at the caster, volume 0x7f; xi-client plays the first `0a..` Sep (the actor bytes are not known) | CODE 0x1005bea2 |
| sched 0x0E | draw: this frame's image, UV rect from z = +0x330 (driven to rec+0xC), colour +0x32c with alpha byte x 2, SRCALPHA / INVSRCALPHA | CODE 0x1001385d |

xi-client's screen pass: when a frame has screen-sampling batches or layers, the scene pass stores depth, ends after the
ordinary effects, the colour target is copied (and kept as the freeze image when op 0x10 starts), and rendering
resumes to draw them (`gfx::EffectRenderer::copyScene / drawScreen`).

### Coverage, part 3 (2026-10-18; CODE, FFXiMain.unpacked.dll)

- **L2 0x4e / 0x4f** (CODE 0x1004dce6 / 0x1004db1b; jump table 0x1004df6c): record + 8 holds n RGBA entries
  (bytes R, G, B, A into the particle colour p+0xf8 via 0x1004e190 / 0x1004e1a0 / 0x1004e1b0 / 0x1004e1c0). 0x4e:
  n = 8, index = the weekday (0x1018bca0: day % 8), the colour is set to the entry. 0x4f: n = 12, index = the moon
  phase (0x1018bcf0), the colour is multiplied by entry / 128 (0x1032a778 = 1/128, 0x1003e000). While the day's
  seconds ([0x10629ff8], beside the day fraction 0x10629ff4) are under 3600 the entry is lerped from the previous
  one by seconds / 3600 (0x1032b164 = 1/3600). The game's 0x4e lerp reads the neighbouring byte of the previous entry
  (R from prev R to cur G, ...: an off-by-one in the game, INFERRED a bug; xi-client lerps channel to channel). DATA:
  the moon halo `kasa` (file 0) uses both. xi-client takes the weekday as LSB's (day % 8 of the same Vana'diel clock).
- **L0 keyframe ops that rewrite L1 records** (CODE 0x10049aea..0x10049fe1): each evaluates its curve at the
  generator's age and stores the value into the first L1 record with the named op (0x10049220 walks gen+0xe4, the L1
  list): 0x05 -> L1 0x08 +4; 0x06 / 0x07 -> L1 0x1f +4 / +8; 0x08 / 0x09 -> L1 0x1f +0x18 / +0x1c (x pi, wrapped);
  0x0b-0x0d -> L1 0x01 +0x14 / +0x18 / +0x1c; 0x12-0x14 -> L1 0x02 +4 / +8 / +0xc. xi-client keeps a per-generator
  copy of the payload once one of these runs. L0 0x0e-0x10 (rotate the generator matrix by fmod(kf, 2) * pi about
  X / Y / Z, CODE 0x10049fe6 / 0x1004a166 / 0x1004a2e6): absolute, done in part 7D.
- **L1 0x4e / 0x4f** (CODE 0x1004e9ea): +4 a Vtx (type 0x3e) name; pos += point i, i random (0x4e, 0x1006be90) or the
  generator's spawn count % n (0x4f, gen+0x9c). rec+0xc bit 0 first turns the point by the attach matrix (not done);
  under generator flag bit 29 another path runs (not done).
- **L1 0x67** (CODE 0x1004ef92): pos += velocity slot x remaining life (p+0x110), then the slot is negated: the
  particle starts where it would end and flies back (converging effects).
- **Scheduler 0x42-0x45** (CODE 0x1005ba68 / 0x1005bab4 -> task 0x100614e0, update 0x10061560): only on PC-model
  actors (class 0x10330ebc); lerp an actor float from its current value to rec+8 over the duration, fmod 2, through
  vfuncs 0x410 / 0x414 (0x42 / 0x44) or 0x418 / 0x41c (0x43 / 0x45). The draw set-up (0x100d6dc8) copies those
  floats to 0x10457100 / 0x104570f8, which build the environment-map texture matrix (0x1002b918): the ops scroll the
  actor's reflection. Not done (env maps are not drawn).
- **L1 0x8f** (CODE 0x1004f33e): positions from an entity found through the generator's actor (vfunc 0x188,
  0x101888d0); not traced further.
- Live (BotC WHM 30, West Ronfaure, standing still): Cure, Protectra, Dia and a Banish start each logged
  `fx: unhandled ops: none`; the map server's watchdog cut the longer run short.

### Coverage, part 4 (2026-10-18, live)

- Live (BotC WHM 30, West Ronfaure, standing still): Protectra 2927, Curaga 2807, Paralyze 2858, Diaga 2833,
  Dia 2823, Banishga 2838, Banish 2828, melee rounds and the Goblin Digger's TP moves (3900, 4235, 3308): no unhandled
  op in any of them. West Ronfaure's ambience file 1724 starts sched 0x07 (`init`) and L1 0x8f (`mok1`) at every
  login: both unhandled.
- **Scheduler 0x40 / 0x41** (CODE 0x100616e0): a task that, when the actor has a model object (0x10061650), calls
  its vfuncs +0x64 (the TexInfo), +0x7c (the duration) and +0x4c(1) (on). The model side is not traced.
- **L1 0x8f** (CODE 0x1004f33e): looks an entity up from the generator's actor (vfunc 0x188 -> 0x101888d0), adds two
  offset vectors (0x10186e10 / 0x10186e20 + 0x10186e30) and compares the two candidates' distances through the object
  at 0x1047cf7c (vfunc +0x38); scaled by the record's byte +0xfa. Not traced to the end.

### Coverage, part 5 (2026-10-18)

- **Scheduler 0x07 / 0x08** (CODE 0x1005a1b0 / 0x1005a1fd; the scheduler switch indexes table 0x1005db4c by op - 2):
  when the caster (0x07, 0x100626a0) or target (0x08, 0x10062700) exists, a 0x7c-byte task (ctor 0x10060710, vtable
  0x1032bb54) holds it for the record's duration and calls the actor's vfunc +0x160 at the start: the same busy count
  as 0x59. Nothing is drawn (NoVisual). West Ronfaure's `init` pairs it with 0x1F over 188 ticks.
- **L1 0x8f / 0x90** (CODE 0x1004f33e / 0x1004f4dc; L1 table 0x100523ac, op - 1): not a position op. The light colour
  v = diffuse + ambient of a light object, taking the second light instead when it is active (0x10188a20) and longer
  (squared length, 0x1047cf7c vfunc +0x38); then the particle's colour bytes R (+0xfa), G (+0xf9), B (+0xf8) are
  multiplied by v.x / v.y / v.z, truncated (below 0 -> 0, low byte kept). 0x8f takes the light object of the
  generator's actor (vfunc 0x188 -> 0x101888d0), 0x90 the scene's (0x10629fe4). xi-client uses the actor / scene
  light set from the weather record (INFERRED mapping of the two lights to light 0 / light 1).
- **L1 0x4b** (CODE 0x1004e86a): only on particle class 0xc; rec+4 / rec+8 -> +0x194 / +0x198 (not traced), the
  R G B A bytes at rec+0xc -> +0x19c (D3DCOLOR order). The flare pass (0x10054ab0..0x10054c86) reads +0x19c (0 ->
  0x80808080) as the **glare**: with the body's screen position s (from the centre, in half-screens), k = max(0,
  1 - 2/3 |s|) x 0.5 x (visible count >> 3) / 128, every byte x k, then added into the frame's screen-flash colour
  (0x10014f30 -> 0x10014f50, as L2 0x60). xi-client: the visible part 0..1 for the count term (INFERRED).
- Live: four West Ronfaure logins (BotC), each `fx: unhandled ops: none`; `xi-dat fxops 1724`: 0 unhandled.

### Coverage, part 6 (2026-10-18; CODE, live)

| op | what it does | CODE |
|---|---|---|
| sched 0x0E | (revised) the screen colour is the game's global state (0x104568fc +0x32c colour, +0x330 z), not a layer that ends with the task: the task drives the colour from the current one to rec+8 and z between the current value and rec+0xC over `dur` (the game's lerp starts at the target and eases toward the start, then snaps to the target), and both stay afterwards. Drawn while alpha > 0: this frame's image, source u and v from 1 - z to z (z = 0.98: a 4 % zoom), colour rgb, alpha byte x 2, normal blending. DATA: `blon` (30904: 0xa0a0a0, alpha 0x30, z 0.98) / `blof` (alpha 0, z 1): a soft glow over the cutscene held between them | CODE 0x10058cc4 set-up, 0x10060260 tick, 0x10013806 draw |
| sched 0x60 | play a Sep named rec+8 (this file first, then the type 0x3d search), volume = a global level x 127, flag 0x40; xi-client plays it at the caster like 0x0A (INFERRED). 1,625 records in 111 cutscene files (e.g. 30812 `bash` '8158') | CODE 0x1005b720 -> 0x10036e30 |
| L0 0x15 | visibility box: a matrix with rec+0xC / +0x10 / +0x14 on the diagonal, translated to the generator's position (plus its actor's), through the generator matrix (gen+0xa4) and the view; the zone object's visibility query (vtable +0x8c, 8 corners; rec+0x18 bit 1 a screen-area test via 0x1006c1b0). Unseen: the generator's tick is skipped (its clock still advances). xi-client: the box's 8 corners against the frustum (no occlusion part) | CODE 0x100497ec, 0x1004a7a0, 0x1004a4b2 |
| L1 0x6b | sound particle (class 0xe): rec+8 the handle of a resource named rec+4; its point (+ the camera offset) becomes the particle's position: an ambience sound on something moving (DATA: zone 230 `se04` on `kiji`). xi-client: the first live particle of the same file's generator of that name (INFERRED) | CODE 0x100500f9 |
| L2 0x53 | visibility: a sphere of size rec+4 at the particle; rec+8 == 0: alpha 0 when the visibility query says unseen, else alpha x the seen fraction (0x1004e560) x rec+8. xi-client: the frustum plus the collision ray from the camera (as the lens flares) | CODE 0x1004aee4 |
| L2 0x59 | rotation z = fmod(key x rec+4 + rec+8, 2 pi) - pi, key = p+0x12c = the draw bias - the view depth (CODE 0x10045068): lamp glows turn as the camera moves | CODE 0x1004aeb5 |
| L2 0x5a | scale = rec+4..0xC, plus (key - rec+0x10) x rec+0x14 on each axis when key > rec+0x10 | CODE 0x1004ae46 |
| L3 0x04 | release the object in the dying particle's slot (record bits 13..18; refcount down, deleted at zero: a `line` trail, a sound). Nothing to release in xi-client (NoVisual) | CODE 0x1004e6da -> 0x10063a50 |
| L1 0x8e | (done in part 7D; the footprints, not the target marker) place the particle at its actor: the actor's matrix into p+0x60 and position p+0x54 = actor +0x63c..0x644. Only `fmrk` in file 0 (the target marker; goal 16) | CODE 0x1004f69b |

Live (BotC, `--shot-on-layer OP:SEC` captures SEC s after camera op OP starts):
- `!cs 503` (Southern San d'Oria intro): `ovl1` (30904) op 0x10 freeze frames between shots, 1 s each; the capture shows the cross-dissolve.
- `!cs 4` in Port San d'Oria: `blon` / `blof` (op 0x0E) around a conversation; held at alpha 0.38 for 1.7 s, a soft doubled image.
- The sun's flare and glare in frame: West Ronfaure 08:00, `--yaw 90 --cam-pitch 20`: the sun at (0, -0.31), visible 1, glare 0.40.
- Seagulls were not seen: `kamo` is started by zone schedulers `s001`.. (Selbina 348) after a sched 0x52 time window
  and a 9,000-tick (150 s) delay; a 175 s run at 12:00 at the Selbina zone line started none.
- Each zone run logged `fx: unhandled ops: none` except file 0's `fmrk` (L1 0x8e) and the cutscenes' sched 0x60
  (before it was added).

Zone ambience still unhandled (`xi-dat fxops 300-399`, 100 files, 1,187 records): L2 0x48 (217 records, 20 files),
L1 0x91 / L2 0x69 (251, 7), L1 0x74 / L2 0x54 (94, 6), L1 0x75 / L2 0x55, L1 0x88, L1 0x7c, L2 0x5b, L1 0x4a,
sched 0x6f / 0x70, L1 0x8d / L2 0x68 / 0x6c / L1 0x96, L2 0x40 / L1 0x64, and single files' others. Cutscene files
(30704-31004): sched 0x72 / 0x8a / 0x8b / 0xa6 / 0x51 / 0xb0 / 0x7e / 0x27, L1 0x92 / 0x77 / 0x8a, L2 0x57.

### Coverage, part 7C (2026-10-19; CODE, paired particle ops)

The paired L1 / L2 ops of the full survey (`xi-dat fxops 0-70000`, 61,747 files). Each L1 op fills a slot of the
particle's variable block and its L2 partner reads the slot every frame. The L1 handlers are mostly the generic slot
setters, either `{curve, mode}` (CODE 0x1005032a, 0x1005034b, 0x1005036c, 0x10050381..0x100503d4) or
`{curve, mode, base}` (as L1 0x21). The curve evaluators are 0x10054430 / 0x100546d0, which use the curve's own first
key, and 0x10054400 / 0x10054620, which replace key 0's value with the given base (CODE: they swap curve+0x34 around the
call). "Half turns" means `wrap(fmod(v, 2) x pi)` (CODE 0x10047ae0 / 0x1003de40). "Day" is `DAT_10629ff4`, the time of
day that L2 0x3c-0x3f already use (CODE 0x1004e240). A colour byte is stored as 0 when ≤ 0, else truncated with the low
byte kept (CODE 0x1004e150).

| L1 / L2 | what it does | CODE (L1 / L2) |
|---|---|---|
| 0x91 / 0x69 | L1 keeps the colour dword (p+0xf8). Every frame, L2 sets R, G, B to the kept bytes × the scene light: `0x10186e10()` + `0x10186e30()` of `DAT_10629fe4`. When `0x10188a20()` holds, a second light set is compared by distance (not traced; xi-client uses the scene light only). Alpha is kept. DATA: 534 files, e.g. 101 `wa03` | 0x1004f687 / 0x1004a958 |
| 0x74, 0x75 / 0x54, 0x55 | U / V scroll from a rate curve at the age: `uv = fmod(uv + dt × curve, 2)`; sets p+0x17b bits 0 / 1 (texture matrix on) | 0x1005032a / 0x1004c88a, 0x1004c90d |
| 0x76-0x78 / 0x56-0x58 | spin X / Y / Z: `rot += dt × halfTurns(curve(age))`, wrapped | 0x1005034b / 0x1004c99b.. |
| 0x83-0x85 / 0x61-0x63 | the same spin with the curve at the time of day | 0x1005032a / 0x1004cb42.. |
| 0x8b-0x8d / 0x66-0x68 | `rot = halfTurns(curve(day))` | 0x1005034b / 0x1004cca4.. |
| 0x64-0x66 / 0x40-0x42 | `scale = curve(day)` | 0x1005036c, 0x10050381, 0x1005039a / 0x1004d1af.. |
| 0x6d-0x70 / 0x4a-0x4d | R, G, B, A byte = `curve(day) × 255` (0x4a R = colour+2, 0x4b G, 0x4c B, 0x4d A) | 0x100503d4.. / 0x1004d44f.. |
| 0x95-0x97 / 0x6b-0x6d | `pos = base + curve(day; key 0 = 0)`, with base = the position at spawn | 0x100503f1.. / 0x1004b746.. |
| 0x39 / 0x24 | `dist` strength (class 0x22, vfunc +0x34) = `curve(age; key 0 = base)`, with base = the strength at spawn (p+0x194; another class reads p+0x1b6) | 0x10050af6 / 0x1004c635 |
| 0x5c-0x5f / 0x38-0x3b | the class 0x1c0 overlay colour (p+0x1ac, set by L1 0x55 from rec+0x20 R, G, B, A) per channel = `curve(age; key 0 = byte / 255) × 255`. DATA: 33106 `cry1` | 0x100508c3.. / 0x1004bd30.. |
| 0x7c / 0x5b | point light intensity p+0x1a0 = `curve(age; key 0 = intensity)` | 0x100506a9 / 0x1004c6c3 |
| 0x7d / 0x5c | point light range p+0x19c = `curve(age; key 0 = range)` | 0x100506e1 / 0x1004c728 |
| 0x80, 0x81 / 0x5d, 0x5e | point light intensity factor p+0x1a4 / range factor p+0x1a8 = `curve(age)` | 0x1005032a / 0x1004be64, 0x1004beb3 |
| 0x50-0x52 / 0x30-0x32 | L1 sets `{curve, mode}`. If the L2 list has an op 2 (velocity), L1 also copies that slot's word +1 into its word +2, as the game does. L2 sets velocity component k = `base + curve(age; key 0 = base)`. With no op 2, nothing happens (DATA: every 0x51 generator, 222 `b k1`) | 0x1004e8b6.. / 0x1004c24c.. |
| 0x69 / 0x44 | L1 points slot+8 at the first L2 0x2c (drag) record. L2 writes that record's factor (+4) = `curve(age)`. The game writes the shared record; xi-client keeps the value per particle. DATA: 0 `gn30` | 0x10050a57 / 0x1004bf0f |
| 0x93 / 0x6a | L1 points slot+8 at the first L2 0x65 record and L2 writes its +0xC = `curve(age)`. **No visual**: the tick sends L2 0x65 to its end (jump table entry 0x1004b7ae). The only other code that reads 0x65 records is the linker (CODE 0x10048eef: +4 = the type-7 resource named at +8), so nothing reads the value. DATA: 5036 `jk01` | 0x10050aa0 / 0x1004bf8c |

Point light draw (CODE 0x10056330): `range = p+0x19c × p+0x1a8` and
`intensity = max(p+0x1a0, 0) × p+0x1a4 × p+0x138`. L1 0x58 sets the two factors to 1 + rec+0xC / rec+0x10. Bibiki
Bay's lights (file 104, `fu1`, `ran1`.., rec+0xC = rec+0x10 = -1) therefore start with factor 0. They are lit by the
door entity `_040` ("Test LIGHT", door id 0x3034305F, LSB data/zones/bibiki_bay/npcs.yaml, animation open_door): its
`ints` / `evst` start `ligh` `on01`, which runs scheduler ops 0x6F / 0x70 on each light (intensity factor to 1.0 over
300 ticks, range factor to 1.0 over 5); `inte` / `eved` start `off1`. Seen live 2026-10-19 (BotC at (470, -5,
735)): `fx: door _040 appears, animation 8: true`, `on01` started, the lamp's light pool on the ground.

Tests: "a point light's intensity follows its L1 0x7c / L2 0x5b curve (zone 5 plk1)" and "L2 0x69 tints a particle by
the scene light every frame". Survey after this part: 168 → 107 unhandled kinds and 54,752 → 30,238 records. None of
the remaining kinds is a paired op of this part.

### Coverage, part 7D (2026-10-19; CODE, DATA)

Goal 7, the particle / generator ops left after part 6 that are not L1 / L2 slot pairs (the pairs are part 7C). Survey
`xi-dat fxops 0-70000` (61,747 files): all of these are now handled, no visual, or game no-ops. The particle class
checks are vfunc +0x30(k): 9 = class 0x23 `vlin`, 0xb = point light, 0xe = sound, 0xf = D3b morph.

| op | what it does | CODE |
|---|---|---|
| L1 0x42 | onto the ground: the particle's world point (through the spawn matrix 0x10047320), a collision segment from 1 above to 6 below (0x101814a0 on the zone, 0x1032a3e8 = 6.0); on a hit the point moves there, back through the inverse matrix (3,096 records: smoke, dust, ground rings) | 0x1004f706 |
| L1 0x46 | the parent particle's velocity: the slot of the parent generator's first L1 0x02 (found by 0x10049260) x rec+4 into this slot, slot+3 = 1.0 | 0x1004fbb5 |
| L1 0x48 | (corrected) colour = the parent particle's (p+0xf8). Part 2's "rotation +=" is L1 0x79 | 0x1004fd87 |
| L1 0x4a | texture offset U / V = the parent's (p+0x120 / +0x124) | 0x1004fe58 |
| L1 0x79 / 0x7a | rotation += / scale += the parent's (p+0xe0 / +0xec) | 0x1004fcf3 / 0x1004fd3d |
| L1 0x7b | the attach offsets of 0x10053d90: position rec+4..0xC, rotation rec+0x10..0x18 into Gen_AttachMatrix 0x1003bd40 (when the generator has one, gen+0x98, CODE 0x10047e8b); the spawn handler itself skips it. xi-client: matrix x T(pos) x R(rot), INFERRED frame | 0x100522ef, 0x10053dbb |
| L1 0x7e / 0x7f | point lights: intensity (p+0x1a0) / range (p+0x19c) = the parent's (L1 0x58 sets them, setters 0x10052fc0 / 0x10052fb0) | 0x1004fe84 / 0x1004febc |
| L1 0x88 / 0x9c | p+0x160 = rec+4 (DATA: a light generator's name: `plp1`, `bl13`) / the parent's. Nothing in the binary reads p+0x160: no visual | 0x1005149d / 0x100514ab |
| L1 0x89 | rec+4 a resource handle, not linked (CODE 0x10048325 default): 0 clears the slot; non-zero copies a field of it into the next L1 0x4e / 0x4f. DATA: 5 records 0, 2 stale editor pointers (ignored here) | 0x1004e97d |
| L1 0x8a | sound particle: p+0x1e8 = 1: beyond the far distance it keeps playing at volume 1 / 127 (0x10036cc9) instead of stopping | 0x100501a4 |
| L1 0x8e | footprints (`fmrk`, file 0, started per step on sand / snow / shallow water): the particle's matrix x the actor's ground-aligned matrix (0x100d58d0: actor vfuncs +0x210 / +0x1c0), position = actor +0x63c. xi-client: at the actor's position and facing (tilt not done; which stored position +0x63c is INFERRED) | 0x1004f69b |
| L1 0x92 / 0x9e | sound particle distance mode p+0x1e9 = 1 (plain) / 2 (across the ground only); default 0 = the vertical part x 3 (0x10036c5c, 0x10329d38 = 3.0). L1 0x4c sets 1 for world-placed generators (origin mode 0, CODE 0x1005023d) | 0x100501c1 / 0x100501e1 |
| L1 0x4c | (corrected) rec+8 -> p+0x1b0 = the full-volume distance, rec+4 -> p+0x1b4 = the silent one (0x10036ba0 tests the far one first; DATA `seu1` 40 / 20); 0 = 3 / 30 | 0x10050247 |
| L1 0x94 | the parent generator's first L1 0x4e Vtx name into this generator's L1 0x4e (spawn on the parent's points) | 0x1004fe03 |
| L1 0x98 | while the parent's remaining life != 0: remaining / total = the parent's, and the slot +0 / +3 = the parent's L1 0x3e slot (wobble amplitude X, heading) | 0x1004f056 |
| L1 0x9b | re-attach this generator with the parent generator's actors (0x1003b6e0 / 0x1003b760, 0x10053d90) and add the parent particle's world position, in this generator's frame (/ gen+0x78), to the position: spawn at the parent particle. Every child generator in xi-client is already placed at its particle | 0x1004ff20 |
| L1 0x9d | draw key p+0x128 = the parent's (and p+0x13c, the last view depth, which the draw rewrites, 0x100450a0) | 0x1004fef4 |
| L1 0x57 | class 9 only (type 0x23 `vlin`, 1 record in all data, 5024 `g012`): p+0x1dc..0x1e4 = rec+8, +0xC, +4. The class's draw (vtable 0x1032b178 +0x48 = 0x100640b0) is `ret`: no visual | 0x10051a4a |
| L1 > 0x9e | past the jump table (op - 1 > 0x9d): the default (DATA: file 6068's one 0xff record is a misparse of a non-effect file) | 0x1004e857 |
| L2 0x2e / 0x48 | one handler: d = vfunc +0x44 (0x10041970: a mesh point through p+0x60, length via the math object 0x1047cf7c +0x94; INFERRED the camera distance); d == 0: alpha factor (p+0x138) 0. 0x2e: full within rec+4, fading to 0 at rec+8, 0 beyond. 0x48: 0 below rec+4, fading in to rec+8, full to rec+0xC, fading out to rec+0x10, 0 beyond (DATA `umsh` 1000 / 1200 / 5500 / 6000: the far sea). When 0: rec+0xC bit 0 (0x2e) / rec+0x14 bit 0 (0x48) ends the particle (remaining = 1) | 0x1004af82 |
| L2 0x6e | the 0x48 band weight w (d == 0: nothing) into D3b morph weights p+0x194 / +0x198 = w / 1 - w (class 0xf: grass `gr01`..: frame 0 near, frame 1 far) | 0x1004b0b1 |
| L2 0x2f | slot = R(rotation) (|slot|, 0, 0): the velocity turned along the particle's facing (matrix 0x1047cf7c +0x58 from p+0xe0..0xe8). xi-client turns it with the draw's euler order | 0x1004c3bf |
| L2 0x23 / 0x47 / 0x65, > 0x6e | the jump table's "next record" entry 0x1004b7ae, or past the table: game no-ops | 0x1004a91a |
| L0 0x0e-0x10 | each: the attach matrix again (0x10053d90), then turned about X / Y / Z (math +0x60 / +0x64 / +0x68) by a = fmod(kf(t), 2) x pi wrapped to +-pi, around its own position (translation kept), t = elapsed / (L1 header life (0x100492e0: header +0x22) + duration) up to 1. Absolute every frame, so the last such op wins. DATA: 0x0f in 304 records (spell rings `roy1`, `snr1`), all with L0 0x11 | 0x10049fe6 / 0x1004a166 / 0x1004a2e6 |
| L0 0x17 | the 0x04 emission rate from a keyframe at the time of day (0x10629ff4; no base, the curve's own first key): `time`, `tims`, `wabi` curves on birds and lamps (433 records, 11 zone files) | 0x10049a04 |
| L0 < 4, > 0x17 | the default case (op - 4 > 0x13) | 0x100496cb |

xi-client: `EffectSystem::setGroundRay` (the client's collision, game space) for L1 0x42; child generators keep a
copy of their burst parent (`Generator::parentSnap`) or read the live one; `FxSound::distanceMode` / `keepFar` and
`SoundSystem::playSep` for the sound modes. Tests: "fork 7D's particle and generator ops all have a coverage status",
"zone sound particles hear by L1 0x4c's distances, near first, plain distance", "an effect sound's distance by mode",
"a spell whose generators turn by L0 0x0f runs with finite particles".

### Coverage, part 7A (2026-10-19; CODE, DATA; actor ops)

Scheduler ops that act on the caster / target actor (xi-client `src/client/effects_actor.cpp`; the full survey
`xi-dat fxops 0-70000` found them in 61,747 files). "Caster" = 0x100626a0, "target" = 0x10062700. Actor vtable
0x1032d710; "model" = actor +0x70.

| op | what it does | CODE |
|---|---|---|
| 0x40 / 0x41, 0x54 / 0x55 | texture overlay on the caster / target for `dur`: +8 the texture's chunk name (type 0x20), 0x54 / 0x55 the blend at +0x10 (0x48 additive, 0x44 alpha), 0x40 / 0x41 0x48. Actor +0x750 texture, +0x734 bit 0 on; the task's end clears it if still its own. Drawn (0x100d6dc4 -> 0x1002b885) as a second pass of the model: render states SRCALPHA / ONE, no z write, texture coordinates through a matrix built from the view with 0.5 entries plus the scroll as translation. xi-client: a second skinned pass, uv = 0.5 + the camera-space normal / 2 + scroll (INFERRED: the exact matrix). DATA: 1311 `aper` `nami` | 0x1005bb81 / 0x1005bb73 / 0x1005bb58 / 0x1005bb2f -> 0x100616e0, end 0x10061660 |
| 0x42 / 0x43 (caster), 0x44 / 0x45 (target) | the overlay's scroll: current -> +8 f32 over `dur`, fmod 2.0; 0x42 / 0x44 actor +0x73c, 0x43 / 0x45 +0x740 (vfuncs +0x410..+0x41c). Only skeleton actors (class 0x10330ebc) | 0x1005ba68 / 0x1005bab4 -> 0x100614e0, tick 0x10061560 |
| 0x48 / 0x49 | refraction pass: actor +0x744 current -> +8 over `dur`, flag +0x734 bit 1 (cleared at the end when 0). The draw adds a pass sampling a screen-sized render target (device +0x194) with colour (0x80 - the actor's alpha) \| 0x808080, both scroll entries = +0x744: a refraction outline as the actor fades (DATA 0 `pop1`, 56695 `dis0`..). `pop1` is started by S2C 0x05B mode 6 MATERIALIZE (protocol-map §5.4; mode 3 POP starts the model's `pop0`); seen live 2026-10-19. xi-client (2026-10-19): `SkinnedRenderer::drawRefraction` after the mid-frame screen copy: the model again with the copy at its projected position + the strength on both axes, alpha 1 - the actor's alpha (blend SRCALPHA / INVSRCALPHA INFERRED) | 0x1005b377 -> 0x10061170, draw 0x100d6e14 / 0x1002b947 |
| 0x4A | op 0x0B's sound at the target, only when the target is the player (vtable +0x304). DATA 3310 `main` `0007` | 0x1005b50a |
| 0x5C | counter: the caster's scheduler `cni0` (not engaged, +0xf8), else by actor +0x598 2 `cnr0` / 3 `cnb0` / 4 `cnl0` / `cnf0`. xi-client: `cnf0`, else `cni0` (engagement not known there, INFERRED) | 0x1005a784 |
| 0x5E | knockback of the **target** from the caster: +8 s16 0 = by level (result var 0x2C r+0xA >> 2, table 0x1035bc70: speed / decay / hold per level 1..7, e.g. 1: 1/12, 0.075, 5; 7: 1/6, 0.05, 45), else explicit (speed +0x10, decay +0xC / 60, hold +0x14, 0 = 1); +0xA != 0 pulls (speed negated). Motion `bf0?`, `bb0?` from behind (swapped when pulled). Tick: `hold` ticks at full speed, then the velocity loses `decay` of itself per tick until < 9e-6; a step that ends within 0.81 of the source is skipped. xi-client: the slide is drawn as an offset until the next server position; level direction (INFERRED: the game normalises in 3-D) | 0x1005aa93 -> 0x100aa810 / 0x100aa870, tick 0x100aaaa0 |
| 0x79 / 0x8C (0xA4 / 0xA5) | the caster's battle / idle (run / walk) motion set by +8: tables 0x1032b804 `btl bt1 bt2 bt3`, 0x1032b814 `idl id1..`, 0x1032b824 `run ru1..`, 0x1032b834 `wlk wl1..` -> actor +0x7d4 / +0x7c8 / +0x7d0 / +0x7cc. DATA: bombs 1582 `ini1`..`ini3` (growth). xi-client: `EffectSystem::stance`, the character animation's idle / battle / walk / run (0xA4 / 0xA5 are part 7B's; the state takes them) | 0x1005bf76 / 0x1005bfaa / 0x1005c011 / 0x1005c068 |
| 0x84 | draw / sheathe sub-scheduler from the caster's files: `in1N` / `ot1N` (+0xA 0, main) or `in2N` / `ot2N` (sub), +8 0 in / 1 out, N = '0' + the sheath style (actor +0x87c main / +0x87f sub, max(0, s8)). DATA 40815 `in 0` / `out0`. xi-client: the main weapon's style for both (INFERRED for sub) | 0x1005a9c3 |
| 0x89 | no head look-at for `dur` or until the caster moves more than a yalm in x or z: actor +0x840 bit 1, which makes the look-at target none (0x100d5ac0). DATA: ranged shots `ls01`..`ls06` | 0x1005b07c -> 0x1005f380, tick 0x1005f470 |
| 0x8D, 0x8E-0x96, 0x98, 0x99 | the look of effects-only NPC model files (194 files, e.g. model 361 = 1661 `mith`): 0x8D race (model +0xef), 0x8E..0x96 slot words face, head, body, hands, legs, feet, main, sub, ranged (model +0xfc[i] via vtable +0x108(i, v); the getter returns word \| i << 12, as S2C 0x00D), 0x98 slot 0 = another NPC model id (2060 `ura_` -> 790), 0x99 model +0x204 = scale (1.5). xi-client: `dat::effectLook`, applied when the model loads (these NPCs were invisible before) | 0x1005c15b..0x1005c39b -> 0x100846b0, 0x10084840, 0x100848b0 |
| 0x9A | +8 1: the actor's model generators do not start by themselves (model +0x130 bit 1, read through vtable +0x124 by 0x10084f90). That function starts every type 5 resource of the model's files with flag 0x1000 whose origin mode m = (bit 16) << 4 \| (& 0xF) is 1, 2, 4, 5, 7..12 or 16..19 (switch 0x10085114), re-triggering running ones; called when models load (0x100c41fa, 0x100cf669, 0x100d6225 ..). xi-client: `EffectSystem::startModelGenerators` when an actor appears, after its `init`; they run while the actor exists | 0x1005c4bb -> 0x10084910 |
| 0x9E / 0x9F | skeleton point ids: 0x9E actor +0x8a6 (the target arrow's anchor, vfunc 0x27c = 0x100d0620), 0x9F +0x8a7 (vfunc 0x278 = 0x100d03f0, the other projected anchor; INFERRED the name) | 0x1005c639 / 0x1005c669 |
| 0x86, 0xBE | model +0x128 \|= 0x80 / model +0x13c bit 18: no reader of either bit in the binary (the 0xBE getter 0x10088170 has no caller): NoVisual | 0x100595bf -> 0x1005e640, 0x1005ca1f -> 0x10088180 |
| 0x88 | a timed task whose destructor sets the caster's colour to 0x80808080 only if deleted before its time ran out (+0x78): NoVisual | 0x1005b10e -> 0x1005f250, 0x1005f320 |

### Coverage, part 7B (2026-10-19; CODE, DATA)

Scheduler ops from the full survey (`xi-dat fxops 0-70000`). The scheduler's jump table 0x1005db4c is indexed by op - 2
(dispatch 0x10057f08; default 0x1005abc6). Actor vfuncs are slots of the CXiSkeletonActor vtable 0x10330f40 (the same
targets in 0x1032d710). Task classes are named by their vtable's slot 0 (CMoTask records). Left after this part:
`sched 0x6E` and `0x7F`, finished in part 7E (the rows below).

| op | what it does | CODE |
|---|---|---|
| 0x19 | `main` of effect file 2800 + rec+8, caster -> target, started once the file is loaded (vfunc 0x2c4 = 0x100a9630: 0x100a9800(id + 0xaf0, target, 'main')). DATA: file 0 `spop` 0x268 -> 3416, `sdep` 0x269 -> 3417; monster files 1316.. | 0x1005834c |
| 0x51 | screen colour kept in 0x104568fc +0x334 (copied to +0x31c every frame, 0x100119c0): bytes R, G, B, A at +8 from the colour now over `dur` - 1 ticks (task vtable 0x1032b8bc, slot 0x1047bf2c, tick 0x10060510, end 0x10060680 -> 0x10014eb0). Drawn by 0x1000a820 after the scene: nothing at 0x80808080; with A = 0x80 the alpha becomes A x B / 128 and the colour black; alpha > 0x80: added (SRCALPHA / ONE), else the frame x alpha (INVSRCALPHA / SRCALPHA). DATA: 616 `lat0` 00 00 00 80 (to black), 26 `mov1` 08 12 30 80 (x 0.19) | 0x10058ebf, 0x1000a820 |
| 0x72 | the same into +0x328 (-> +0x324 each frame, plus the brightness boost 0x10350174 above 0x80): drawn by 0x10013270 when R, G or B is set, added over the frame (SRCALPHA / ONE), alpha byte x 2. DATA: zone 116 door `stb3` white flash 80 80 80 80 / 80 80 80 00 / 0 | 0x10058fb8, 0x10013270 |
| 0x6F / 0x70 | a light generator's (resource 0x47, isa 0x0b) particle +0x1a4 intensity scale (0x6F) or +0x1a8 range scale (0x70; L1 0x58 sets both, 0x10052fb0..0x10053010) from its value now to the float at +0x10 over `dur` (task vtable 0x1032bb80, tick 0x10060920). DATA: zone 104 `on01` lamps | 0x10059cd7 -> 0x100608c0 |
| 0x7A | the caster held at the target's attach point +0x20 plus the offset +0x10 turned by the target's rotation y, for `dur`: actor +0x7a4 point, +0x7a8 target, +0x7b4 offset; at the end +0x7a4 = -1 and the position back (0x10061e80). Points 0x30..0x3c go through 0x1003b950 (xi-client: the actor's position). DATA: 111 files (`jh02`, `mot1`) | 0x1005c6f9 -> 0x10061f00, 0x100c6e7a |
| 0x7B | EnvMgr_ZoneChange(rec+8): another area's environment set (as event ops 34 / 35). DATA: only the title demo files 23-26; xi-client has no title screen: NoVisual | 0x10057f0f -> 0x1018b2b0 |
| 0x7C | Env_ChangeWeather(the fourcc at +8, start rec+6, offset 0): that weather group now. DATA: 23 `ex3e` `fine`, 616 / 617 / 619 | 0x10057f4e |
| 0x7D | sets the Vana'diel clock 0x10629ffc's hour / minute / second from rec+8 (1/60 s of the day; 0x1018c120). In a zone the clock sync 0x1018c1c0 writes it again every frame; it sticks only in the title demo (files 23-26): NoVisual | 0x10057f35 |
| 0x7E | one Weather record (chunk type 0x2f named +8 in this file) applied as it is (Env_ApplyZoneDefaultRecord). DATA: 23 `mov1` `s101`, 30769 `sg01` `0000` | 0x10058399 -> 0x1018af20 |
| 0xAE | EnvMgr_SelectFourccTable(rec+8): the alternate table (`fin1` `sun1` .. `dar1`, 0x10377c08) for the weather shown now, changed with offset rec+0xC x 3600 ticks (Vana'diel minutes, 2.4 s each). DATA: zone 158 `ints` / `inte`, `evst` / `eved` (30) | 0x1005cf4f |
| 0x8A / 0x8B | a sound held in slot rec+0x20 (< 8): the Sep named +8 (type 0x3d) through 0x10036e30, handle in 0x1047bcc4[slot]; 0x8B stops that slot (0x10036ac0 -> 0x10245b60). xi-client: at the caster (INFERRED, as 0x60), tagged mixer voices | 0x1005b7d0 / 0x1005b8ae |
| 0x9B | the caster holds weapon rec+8 (0 main, 1 sub; > 1 = 0) in the grip for `dur`: vfunc 0x324(1, slot) -> actor +0x83e[slot], 0 at the end (0x10061ce0); read by the weapon attachment 0x1002a070 (grip points 0x7e / 0x7f). DATA: emote files 10065.. xi-client: the main weapon only | 0x1005c577 -> 0x10061d30 |
| 0xA7 / 0xA8 | the same on / off with no time. DATA: 2482 `ded0` / `app0` / `app1` | 0x1005c5c7 / 0x1005c600 |
| 0xBC | footsteps sound while standing for `dur`: actor +0x8ab counter, read by the step test 0x100d4d91 | 0x1005c531 -> 0x10061ba0 |
| 0x9D | a drawn CXiDollActor of the target (0x100abdd0(target, 1)) takes its place, as 0x23; 0x100ac1c0(rec+8) writes rec+8 into its 48 point slots and +0x408 (meaning open; DATA only 0) | 0x10058912 |
| 0xA1 | the target's colour as 0x2A (CMoActorColorDriveTask) with +0x86 = 1: a task it replaces or that ends sets its colour to the target (0x1005f8c0) | 0x1005b27c |
| 0xB2 | the target's colour back to 0x80808080 (vfunc 0x30) for entity kinds 2 / 3 (xi-client: any actor) | 0x1005bd26 -> 0x100617d0 |
| 0xA4 / 0xA5 | the caster's run / walk motion: rec+8 picks `run` `1un` `2un` `3un` (table 0x1032b824) / `wlk` `1lk` `2lk` `3lk` (0x1032b834) into actor +0x7d0 / +0x7cc and entity +0x1b4 +0x19c / +0x1b0 +0x198 (the family of 0x79 `btl` 0x1032b804, 0x8C `idl` 0x1032b814) | 0x1005c011 / 0x1005c068 |
| 0xAB | entity +0x130 bit 4 = rec+8 & 1: when the entity is placed again (0x1008fc52) its motion names are applied again. NoVisual | 0x1005c0bf |
| 0xAC | which scheduler the caster's death plays: entity +0x130 bits 5-6 = rec+8 & 3, read by 0x1008ca55 from table 0x1032d5f0 `dead` `1ead` `2ead` `3ead` | 0x1005c0fb |
| 0xA6 | a sound level (type 8, the music stream) to the float rec+8 over rec+0xC ticks (0x10035930; tick 0x10035870 -> 0x10036020, which stops the stream at 0). DATA: 30777 `s12k`, 30821 `se3e` | 0x1005b95b -> 0x10035d60 |
| 0xB0 | ends the CMoCameraTask running Camera chunk rec+8 (0x1003cae0) | 0x1005a05c |
| 0xB3 | a sound at the caster as 0x0A, only with config 153 bit 1 set (0x101937a0(1)). DATA: emote files 10069.. | 0x1005b531 |
| 0xB4 | actor +0x8c0 = the float rec+8 for `dur`: the height added to the name / arrow anchor points (vfunc 0x278). xi-client's anchors use the model height: NoVisual | 0x1005c74d -> 0x100620b0 |
| 0xB5 | actor +0x8aa counter for `dur`: no random idle choice while set (0x100cdef5); xi-client has no idle fidgets: NoVisual | 0x1005c4eb -> 0x10061a20 |
| 0xB6 | rec+8 -> actor +0x840 bit 22; with it the stance picker (0x100c86df) shows wep0 / wep1 (0x100d5e40) and hides wep2 (0x100d6010) | 0x1005be66 |
| 0xB8 | actor +0x88 bit 7 = rec+8 (no reader found; one file): NoVisual | 0x1005c3cb |
| 0xB9 | actor +0x8ac bit 1 = rec+8: the stance picker is skipped (0x100c7171) and 0x100ccd90 places the actor from points 0x17 / 0x18 (not done). xi-client: a motion hold | 0x1005c3fb |
| 0xBA | CXiSkeletonActor caster: +0x840 bit 23; the stance picker skips its motion change while set (0x100c8598). xi-client: a motion hold | 0x1005b0cc |
| 0xBD | actor +0x8a9 = rec+8, the bone the camera code reads (0x1003c2ba; follow cameras not done): NoVisual | 0x1005c6c9 |
| 0xAD | actor +0x8a8 = rec+8, the bone whose height 0x100d65a0 stores in 0x10456da8: NoVisual | 0x1005c699 |
| 0xC2 | entity +0x13c bit 20 = rec+8: a scheduler op then looks up its scheduler in the caster's model (0x1005d926); xi-client already falls back to the caster's files: NoVisual | 0x1005c48b |
| 0x62 | the caster turns toward the target at rec+8 degrees a tick (actor +0x870 angle left, +0x874 rate) | 0x1005ae5c |
| 0xBF | knock-back of the target (task 0x10060cd0): motion `bf0?` / `bb0?` (0x100ce0b0 picks; rec+0xA swaps). xi-client: the motion only (INFERRED side); the push is not done | 0x1005abd3 |
| 0x1D | a lift move: carried out by client::Doors::updateLifts (lift_re.md) | 0x1005860b |
| 0x27 | CMoPathDriveActorTask (ships): carried out by client::Ships | 0x1005c884 |
| 0xA9 | CMoActorRotationDriveTask: rotation to (rec+8, +0xC, +0x10) degrees, absolute; mode rec+0x14 = 0 at once and held, else eased over `dur`. DATA: ship 31020 `seq0` y -90 at t 1040. Carried out by client::Ships | 0x1005f950, tick 0x1005fa60 |
| 0x6E | (part 7E) the caster walks along its facing at rec+8 yalms a second for `dur` ticks: 0x100aac70(dur from rec+6 (0x1005e130), rec+8 / 60) sets entity +0x101 = 1, +0x118 = the ticks left, +0x11c the speed, +0x120 = (1, 0, 0, 1) through the actor's rotation (vfunc 0x1c0) x the speed; the entity's position update (0x100a59eb / 0x100a5aa3, behind the +0x102 / +0x103 / +0x104 movement modes) calls 0x100aad20, which adds +0x120 to the position once per whole tick until +0x118 runs out, then clears +0x101. DATA: 39275 `cdam` (`dam?` then 8 / s for 120 ticks while fading out), `coff` (`ch1?`, `run?`, 12 / s for 72 ticks): a monster leaving. xi-client: the knockback offset (op 0x5E's) with that velocity, held for `dur`, drawn until the next server position. Test: "effect op 0x6E ..." | 0x1005ae10 -> 0x100aac70, tick 0x100aad20 |
| 0x7F | (part 7E) NoVisual: actor +0x668 (setter vfunc 0x34 = 0x100a4790, getter 0x38 = 0x100a47a0, in the actor vtables 0x1032d710 .. 0x103313e8; constructor default 0x80 at 0x100c6046) to rec+8 (0 / 0x80) over `dur` (tick 0x10061380). The only call of the getter in the actor, scheduler and draw code (0x10029000-0x1002e000, 0x1005a000-0x10063000, 0x100a0000-0x100da000, 0x10180000-0x10195000) is the 0x7F task's own start (0x10061355); the other `+0x668` accesses (0x100ac96e, 0x100acb29, 0x100acee2) belong to a class (vtable 0x1032fd58) whose +0x668 is a pointer to 0x40-byte records. DATA: 5036, 5069-5071 `brak` | 0x1005b97b -> 0x10061300 |

xi-client: the screen colours are layers from EffectSystem::screenLayers; the environment ops are
EffectSystem::environment (group, alternate table or one record, cross-faded, held until the zone changes); sound slots
and the music level go through audio::Mixer tags and channel levels; actor states are EffectSystem::weaponHeld /
stepsForced / motionVariant / attachedPosition / yawOverride. Tests: tests/test_effects_7b.cpp.

### Open items (meshes / scheduler)

- 0x028 result struct offsets r+8..r+0x28 vs bit fields: parser not found; mapping INFERRED.
- `efhl` → `ef h` resolution: partly traced (2026-10-20). Op 0x57 (0x100595fa) looks the name up exactly (mask -1)
  three ways: the caster's vfunc +0x1ec (0x100ce8e0: each file of the actor's +0x674 list, then the file from vfunc
  +0x1e0), for a CXiSkeletonActor `0x100ce3b0(7, name, -1, 0, 0)` (its +0x7e4 file chain, a type-7 hit also checked
  against actor +0x7c4), then the global root. No step rewrites the name and the DLL holds no `efhl` / `ef h`
  constant, while file 0 asks for `efhl` / `sehl` (DATA 0.DAT +532384) and weapon files hold `ef h` / `se h`
  (e.g. 27921). So the hand letter is put into the weapon's names somewhere at load: not found. Live, the weapon's
  `ef h` and `se h` run on every hit (BotA, West Ronfaure, 2026-10-20).
- Meaning of the duration-bound actor tasks: settled (0x07 / 0x08, 0x1F / 0x20 / 0x56, 0x59 busy count; 0x2E / 0x2F
  motion hold; 0x48 / 0x49 refraction; 0x88 colour reset; see the Coverage tables); 0x89 not traced.
  Lead kept (CODE, 2026-10-17): the client keeps its own class records `{name, size, parent}` (e.g. `0x1032c960`
  CMoTask size 0x34; `0x1032b7e0` CMoActorColorDriveTask 0x88, parent CMoTask; `0x1032b7c8` CMoLockColorDriveTask;
  CMoLockLookAtDriveTask, CMoActorRotationDriveTask, CMoPathDriveActorTask, CMoCameraTask, CMoOtTask by name string).
  Vtable slot 0 returns CMoTask for every task; slot 11 of the vtables `0x1032bb0c`, `0x1032bfb8`, `0x1032c000` returns
  `0x1032aafc` CMoAttachments (size 0x40). No record names CMoAttachments as its parent, so the derived classes are
  still unnamed; MSVC RTTI is absent (the dword before each vtable is not a locator).
- D3a frame selection; D3b vertex layout.
- Afb bytes +0x02, +0x03, +0x11, +0x13; exact fade order.

---------------------------------------------------------------------------------------------

# Cast start and attach points (client RE, 2026-10-17)

Same binary and conventions as above. Evidence is tagged **CODE(address)**, **DATA** or **INFERRED**.
"PC base" means the race base file of dat-format §10.1 (Hume male 7072). Scheduler dumps were made with the
scratch decoder `fx3/dsched.py`, with post-delay timing (§S.1): `t` is the tick at which a record runs.

## C.1 How a 0x028 action reaches a scheduler

**Parser.** The parser is `0x1018c950` (CODE). It reads the bit stream with `0x1018c480(nbits)` into a raw
record:
- header: +4 actor id (32), +0x18 target count (6), +0x14 res_sum (4), +8 type (4), +0xC cmd_arg (32),
  +0x10 info (32);
- per target (stride 0x16C): actor id (32), result count (4);
- per result (stride 0x2C, offsets from the result base): +0 miss (3), +4 kind (2), +6 animation (12),
  +8 info (5), +0xA scale (5), +0xC param (17), +0x10 message (10), +0x14 modifier (31), then an optional
  added effect (+0x18 kind (6), +0x1A info (4), +0x1C param (17), +0x20 message (10)), then optional spikes
  (+0x22 kind (6), +0x24 info (4), +0x26 param (14), +0x28 message (10)).

These are exactly the field order and widths that §S.5 assumed for the expression-VM variables, so the r+N
mapping in §S.5 is confirmed for the parsed record. That §S.5 reads the same layout after the copy that
`0x1018d080`/`0x1018d0e0` make is INFERRED.

`0x1018d080` copies the header into the action object (CODE): +0xC type, +0x10 cmd_arg, +0x14 info,
+0x1E target count, +0x20 targets. The wrapper's scheduler name (+4) is initialised to `'main'`
(`0x1018c5a0`).

**Dispatch by type.** The 0x028 handler is `0x1009e330`. It switches on type − 1 (byte table `0x1009e614`, jump
table `0x1009e604`) and calls a virtual method of the **caster** actor (CODE):

| type | handler | call |
|---|---|---|
| 1, 2 | `0x1009e4b9` | vtbl+0x2B4(action) → `0x100cef60`: vtbl+0x298(**cmd_arg**, first target, action) |
| 3–6, 11, 13–15 | `0x1009e5ca` | vtbl+0x2C0(action) = `0x100a9160`: effect file base + animation, scheduler `main` (dat-format §10.8) |
| 7, 8, 9, 10 | `0x1009e4d8` | if `cmd_arg & 0xFFFF == 0x6163` (`"ca.."`), first `0x10085380(cmd_arg)` (below); then vtbl+0x298(**cmd_arg**, caster, 0) |
| 12 | `0x1009e4cc` | vtbl+0x298(**cmd_arg**, caster, 0) |

For the local player, types 8 and 10 with a `"ca"` cmd_arg also call `0x1022af30` / `0x10222960` with the
result param (the spell or ability id). INFERRED: this drives the cast-bar UI. The `"sp"` (interrupt) path calls
`0x1022af80` / `0x10222970` instead, and type 9 calls `0x101f3100`.

**vtbl+0x298, "run a named scheduler on this actor" (CODE `0x100ced70`, skeleton-actor vtable `0x1032d710`).**
1. Look the name up as a type-7 chunk in the actor's own loaded files (`0x100ce3b0(actor, 7, name, -1, 0, 0)`).
2. If that fails, look it up in the global resource list (`[0x1047d128]+0x40008` → `0x100723f0`), which includes
   file 0.
3. Start it with `0x10056ba0`, with caster = this actor, target = the second argument and action = the third.
4. If neither lookup finds it, nothing runs.

So for the **start** types (7, 8, 9, 10, 12) the 4CC in cmd_arg *is* the scheduler name. Caster and target are
both the acting actor, and no action results are attached.

**The cast-state slot (CODE `0x10085380`).** `actor+0xA0` holds the 4CC of the cast in progress. Storing a new
value first runs vtbl+0x298 on the **old** value with its first two bytes replaced by `"st"`
(`and 0xFFFF7473; or 0x7473`). For example, `cawh` → `stwh`. A new cast therefore stops the previous cast's glow.
`0x100a9160` stores 0 when it rejects an action (`0x100a917f`).

**LSB side (DATA, `~/git/server/src/map`).** cmd_arg comes from `enums/four_cc.h`:

| Source | cmd_arg |
|---|---|
| `magic_state.cpp:114` (type 8 start) | `CSpell::getFourCC()` by spell group: white `cawh`, black `cabk`, blue `cabl`, song `caso`, ninjutsu `canj`, summoning `casm`, geomancy `cage`, trust `cafa` |
| `interrupts.cpp` `MagicInterrupt` (type 8 interrupt) | `getFourCC(true)`: the same groups with `sp..`, e.g. `spwh` |
| type 7 (mob skill, weapon skill and pet skill readying) | `cate` |
| interrupt of type 7 | `spte` |
| type 9 item start / interrupt | `cait` / `spit` |
| type 12 ranged start / interrupt | `calg` / `splg` |
| type 2 ranged finish | `shlg` |
| type 1 melee | `atk0` |
| type 10 ability start (`ability_state.cpp:130`) | **0** |

In the type 8 start, the result's param is the spell id, its animation is 0, and the target is the spell target
(or the caster when `HIDE_READIES_TARGET`).

## C.2 Magic cast start (type 8) — what plays

The `ca..` schedulers live in **the caster's model files**:
- in every PC race base file (7072, 23176 and 26352 checked) inside block `hm_s` (DATA);
- in NPC/monster model files, e.g. 1300 and 1600 (DATA).

File 0 has **none** of them. File 0 holds the `sp..` (interrupt) and `st..` (stop) schedulers and the glow
schedulers `ner1`…`nefa` (DATA).

PC base (Hume male 7072, DATA). Op 3 starts a sub-scheduler (looked up in the own file, then globally), so `ner?`,
`hwmg` and `sei5` resolve to file 0:

| 4CC | motion (op 5, dur, +0x1E) | glow (file 0) | notes |
|---|---|---|---|
| `cawh` | `mw0?` 93, 0x17 | `ner2` (gens `gn2s` burst, `gn20`, `gn21` for 1800 ticks) | + `hwmg` |
| `cabk` | `mb0?` 33, 0x3F | `ner1` (`gn1s`, `gn10`–`gn13`) | + `hwmg` |
| `casm` | `ms0?` 150, 0x08 | `ner4` | + `hwmg` |
| `cafa` (trust) | `ms0?` 150, 0x08 | `nefa` | + `hwmg` |
| `canj` | `mn0?` 150, 0x10 | `sei5` | + `hwmg` |
| `cabl` | `ma0?` 80, 0x19 | `neao` (`am00`, `am10`, `se50`) | |
| `caso` | none directly: op 0x76 mode 1 → `lc00`/`lc01`/`lc02` | `ner5` | see C.4 |
| `cage` | none directly: op 0x76 mode 2 → `lc10`/`lc11` | (`negh`/`negc` from lc1x) | |
| `cait` | `mi0?` 28, 0x01 then `mi1?` 99, 0x11 | none | item use, op 0x2E for 30 ticks first |

Monster/NPC files (1600 Mandragora, 1300; DATA):
- `cawh` = op 3 `ner2` + op 3 `cast`;
- `cabk` = `ner1` + `cast`;
- `canj` = `sei5` + `cast`;
- `cait` = `cast`;
- `cate` = `nerm` + `cast`;
- `cast` = op 5 `ma0?` (dur 84, +0x1E 0x0A).

So monsters always use `ma0?` as the casting loop. The glow depends on the school.

**Looping.** The cast motion record's byte +0x1E (the `u8 & 0x7F` passed to `0x100cd850`) is INFERRED to be a
repeat count:
- duration × count ≈ the 1800-tick glow cap: 93×23 = 2139, 33×63 = 2079, 150×8 = 1200, 150×16 = 2400,
  80×25 = 2000;
- weapon-skill motions use 1.

So `x0` simply repeats until something replaces it.

**Glow attachment (DATA, file 0 origin words).** Every `gn??`/`ge5?`/`am??` glow generator is mode 1 (at the
caster, yaw = caster facing):
- point 0 (root, i.e. the feet) for `gn10`–`gn13`, `gn20`, `gn21`, `gn42`;
- point 1 (pelvis) for the others;
- sizeModeA 4: the size is uniform, from max(actor size).

## C.3 How the cast ends

**Finish (type 4).** This is the general path of dat-format §10.8: file **2800 + animation**, scheduler `main`.
`0x100a9160` also stores cmd_arg + 1 (spell id + 1) at task+0x26, or 0 when it is ≥ 0x400 (CODE `0x100a91d8`,
`0x100a925e`); its use was not traced.

The spell `main` schedulers start the release with **op 0x3C** `sh..`, which runs `shwh`/`shbk`/… from the
**caster's** files. DATA, the 300 `main` schedulers in 2800–3099: `shwh` 147, `shbk` 142, `shso` 7, `shsm` 1,
and 3 with no op 0x3C. Ninjutsu and the other schools sit in other spell ranges, which were not surveyed. The PC base versions (7072, DATA) are:
- `shwh` = op 3 `hwmg`, op 3 `stwh` (file 0), op 3 `eis4` (release burst `ge4s`, `ge40`–`ge43`), then at t1 op 3
  `sswh` and op 0x3B `wash`;
- `sswh` = op 5 `mw1?` (60), then `mw2?` (60). `ssbk` = `mb1?` (58), then `mb2?` (61).
- `shbk`/`shsm`/`shnj` follow the same pattern: `stbk`+`eis3`, `eis2`+`stsm`, `ner3`+`stnj`.
- `shso` = `stso`, `eis6`, op 0x77 mode 1 (`ls00..02`), op 0x3B `waso`.
- Monster `shwh` (1600) = `stwh`, `eis4`, `shot` (`ma1?` 28, then `ma2?` 48).

**Stop (`st..`, file 0, DATA).**
- `stwh` = op 0x5F `ner2`, op 0x1E `gn2s`, op 0x2D `gn21`, op 0x2D `gn20`.
- `stbk` = op 0x5F `ner1`, kill `gn1s`, stop `gn10`–`gn13`.

**Op 0x5F is "kill every running instance of the named scheduler"** (CODE: handler `0x10059ac5` resolves the
name in the own block and calls `0x10056de0`, which walks the task list `[0x1047d140]+0xD50` for tasks of type
`'Ms'` (0x4D73 in the high word) whose scheduler is that resource and deletes them with `0x100749e0`). This
**corrects §S.3**, which called 0x5F "start … as a replacement". Op 0x1E then kills the burst generator and op
0x2D lets the looping glow particles die out.

**Which instances the stop ops reach (#113).** Op 0x5F's `0x10056de0` walks the task list and compares only the
scheduler resource (`task+0x80` → resource == the named one), **not** the caster or target (CODE). The 0x1E / 0x2D /
0x3F handler (`0x10059b57` loop) likewise walks the instances of the named generator resource (`0x1005e420` lookup,
type 5) and checks only each instance's own state (`0x10053910`, `0x1005dfe0`) before `0x10053810` / `0x100537c0`; no
compare with the scheduler's caster or target was found (CODE, partial). The data needs at least that much: `ca..` starts
the glow with caster = target = the caster (C.1), but a spell's `main` runs `sh..` → `st..` with the **spell's target** as
target, and `st..` must still stop that glow.

xi-client (effects.cpp): 0x1E / 0x2D / 0x3F stop the named generator's instances **with the same caster**, whatever their
target; 0x5F kills the named scheduler's instances with the same caster. Keeping the caster filter is our choice
(INFERRED: retail would also stop another actor's glow of the same school). Before #113 the generator ops also required
the same target, so a finished spell left its caster's `gn1?` / `gn2?` glow running for its full 1800 ticks (30 s) on
monsters and players alike; an interrupt (`sp..`, run toward the caster itself) was unaffected.

**Paralysed / intimidated at the end of a cast (LSB `MagicParalyzed` / `MagicIntimidated`).** The server sends a type 4
with result animation 508 and then the `sp..` interrupt. File 2800 + 508 (3308) `main` only waits 30 ticks and runs
file 0 `mdam` on the target: no op 0x3C, so no `sh..` release motion (DATA). xi-client: such a type 4 ends the cast
without the release motion (`client::castChange`).

**Interrupt (type 8, cmd_arg `sp..`, result with no message).** The `sp..` scheduler runs from file 0
(block `neri` etc., DATA):
- `spwh` = op 0x5F `ner2`, op 0x28 (f32 20.0), op 0x1E `gn2s`, op 0x2D `gn21`/`gn20`;
- `spbk`/`spsm`/`spnj`/`spbl`/`spfa`/`spso`/`spge` do the same for their own glow; `spso` also plays the sounds
  `kf&h`/`ksng` from the caster at t30;
- `spte` = kill `nerm`, op 0x28 (0.0), stop `gnm0`/`gnm1`, kill `sgnm`;
- `splg` = op 0x75 ×3 and op 0x28 (16.0).

**Op 0x28 = "return the caster to its base motion"** (CODE handler `0x1005aff8`). It calls `0x100c8ae0`, which
picks the motion from the entity status (`entity+0x170`, `0x100842c0`; table `0x100c8da4`/`0x100c8d88`):
- default: idle `actor+0x7C8` (walk/run `+0x7CC`/`+0x7D0` when moving);
- status 1 (engaged): `actor+0x7D4`;
- status 2, 3 (dead): `'cor '`;
- statuses 7 and 33 (resting): `'rx1 '`;
- status 47: `'si1 '`;
- statuses 63–83: `'ci1 '` (chair sitting, INFERRED);
- statuses 6, 38–43, 50–53, 56–62: `'fh1 '` or `actor+0x7D8` (fishing family, INFERRED).

It then plays that motion via `0x100cd850` with blend = the record's f32 (+8) frames. So **an interrupt stops the
glow and blends back to idle (or stance) over 20 frames**. No `spit` scheduler exists in any DAT (DATA: a byte
search over ROM–ROM3 finds only `spop`), so an item interrupt has no visual of its own (INFERRED).

## C.4 Other start types

**Type 7 (`cate`, readying).**
- Monster/NPC files hold `cate` = `nerm` (file 0; → `nrm0`, expression VM) + `cast` (`ma0?` loop) (DATA 1300,
  1600).
- PC base files and file 0 have no `cate` (DATA). The PC lookup therefore normally finds nothing and plays no
  readying motion, as noted in dat-format §10.8.
- Caveat (INFERRED, open): the global fallback `0x100723f0` scans the global resource list linearly from its
  head. If a loaded monster model holding `cate` is reachable from there, a PC might pick it up. Not verified.
- The readying is ended by the type 11/3 finish (file 3900+anim or the WS files), or by `spte` (interrupt).

**Type 9 (`cait`, item start).**
- PC: op 0x2E (30 ticks), `hwmg`, `mi0?` (28), then `mi1?` (99 × 17) loop.
- The finish (type 5, file 4912 + animation) is expected to call `shit` via op 0x3C (INFERRED from the pattern).
  PC `shit` = op 0x5F `chit`, `hwmg`, `ssit`, op 0x3B `wash`.
- Monster: `cait` = `cast`.

**Type 10 (ability start).** LSB sends cmd_arg 0. No scheduler is named 0, so nothing plays (INFERRED from CODE
`0x100ced70`).

**Type 12 (`calg`, ranged start), PC base.** `calg` = op 0x76 mode 0, op 3 `hwso` (file 0).

**Op 0x76/0x77** (CODE `0x1005ace5`) builds the name `lc` (0x76) or `ls` (0x77) + two decimal digits of n, and
runs it from the caster's files with the same caster and target (`0x10056ae0`):
- n = `(actor vtbl+0x3D8)` → descriptor at `actor+0x878`, byte +0xE = `actor+0x886`. This is the **ranged-slot
  model's `info` chunk byte 14**, set by the info switch at `0x100d0c15` (CODE).
- mode 1 (songs): n > 2 → 0.
- mode 2 (geomancy): n ≠ 11 → 10.
- mode 0: n as is; a negative n aborts.

DATA, Hume male ranged models 9416–9671: byte 14 ∈ {0: 121 models, 1: 17, 2: 14, 3: 40, 4: 7, 5: 6, 6: 48, 8: 1,
11: 2}.

The `lc`/`ls` schedulers in the PC base (7072, DATA):

| NN | `lcNN` (aim / cast loop) | `lsNN` (shoot / release) | INFERRED class |
|---|---|---|---|
| 00 | `sk1?` 114 + sound `vsng` | `sk2?` 104, sound `ksng` | sing, no instrument |
| 01 | `sf0?` 60 → `sf1?` 80×10, gen `sefl` | `ls01` | string instrument |
| 02 | `sh0?` 60 → `sh1?` 80×10, gen `sehp` | `ls02` | wind instrument |
| 03 | `gu0?` 56 → `gu1?` 85×8, sound `lgin` | `gu2?` 84, `kalg`, op 3 `ldad` (damage) | gun |
| 04, 05 | `na0?` 56 → `na1?` 85×8 | `ls04`, `ls05` | throwing |
| 06 | `yu0?` 116 → `yu1?` 85×8 | `ls06` | bow |
| 10 | `stgh`, `negh`, `gh0?` 40 → `gh1?` 68×15 | `gh2?` 148, `eigh` | geomancy, bell class ≠ 11 |
| 11 | `stgc`, `negc`, `gc0?` 40 → `gc1?` 68×15 | `ls11` | geomancy, bell class 11 |

**Type 2 (`shlg`, ranged finish).** vtbl+0x2B4 → vtbl+0x298(`shlg`, **first target**, action) (CODE `0x100cef60`).
PC `shlg` = `hwso`, op 0x77 mode 0 (`lsNN`), op 0x3B `wash` (DATA). `lsNN` kills `lcNN` (op 0x5F) and plays the
shot. `ldad` (file 0) = `atpr`, `daml`, **op 0x2B** (damage number), `crtl`. So the ranged damage commit happens
inside `ls03` at t34. There is **no** effect file for ranged (`0x100a8c20` returns 0 for types 2 and 12).

**Op 0x75** (`hwmg`/`hwso`/`lcNN`) hides (+8 != 0, `0x100d6010`) or shows (+8 = 0, `0x100d5e40`) the caster's Os2
meshes named `wep0`+n, n = s16 +0xC (CODE 0x1005bd44; the per-mesh byte set through `0x1002c660`, see the scheduler op
table). On PCs: the main/sub/ranged weapon meshes while casting or shooting (INFERRED: which mesh each record names).

## C.5 Implementation table

| action type | effect file | scheduler | caster / target | ends with |
|---|---|---|---|---|
| 1 melee | none (base 0) | cmd_arg `atk0` from caster files (weapon class file) | attacker / first target | (§Melee) |
| 2 ranged finish | none | cmd_arg `shlg` (PC base) → `lsNN` | shooter / first target | `lsNN` end; `ldad` commits damage |
| 3, 14, 15 WS | PC: WS files (§10.8); NPC: 3400+anim | `main` | actor / first target | — |
| 4 magic finish | 2800 + animation | `main` (op 0x3C → caster `sh..` → `st..` + release motion) | caster / targets | — |
| 5 item finish | 4912 + anim (≥100: 56485 + anim) | `main` | | — |
| 6 JA finish | 4412 + anim | `main` | | — |
| 7 readying | none | cmd_arg `cate` (monster model; none for PCs) | actor / actor | type 11/3 finish, or `spte` |
| 8 cast start | none | cmd_arg `ca..` by spell group, from caster files | caster / caster | type 4 (via `st..`) or type 8 `sp..` (file 0) |
| 9 item start | none | `cait` (caster files) | user / user | type 5, or `spit` (absent) |
| 10 JA start | none | cmd_arg 0 → nothing | — | — |
| 11 mob TP finish | 3900+anim (≥0x200: 49647+, ≥0x600: 59193+, ≥0x800: 84743+) | `main` | | — |
| 12 ranged start | none | `calg` → `lcNN` (NN = ranged info byte 14) | shooter / shooter | type 2 (`lsNN` kills `lcNN`) or `splg` |
| 13 pet skill | 49391 + anim | `main` | | — |

Minimal client logic:
1. On a start action, look up cmd_arg as a scheduler in the actor's own model/motion files, then in file 0, and run
   it with caster = target = actor.
2. Remember cmd_arg per actor. When a new `ca..` arrives, first run `st` + cmd_arg[2..3] for the previous one.
3. On the finish, the effect file's `main` calls back into the caster's `sh..` through op 0x3C, so op 0x3C must
   search the caster's files.

## C.6 Actor attach points: the Sk2 point table

**Where the points come from (CODE).** For skeletal actors (vtable `0x1032d710`), vtbl+0x1C4(id, out) is
`0x100d4490`:
1. out = actor position (`actor+0x678`).
2. `0x1002c310(model=actor+0x674, …)` → `0x1002c330(0, id, out)`:
   - id ≠ 2 (requires model flag `+0x30 & 0x10`): first skeleton of the model, `0x1002a680(id, out)`.
   - **id == 2**: `0x1002a7e0(2)` takes the point's **raw offset**, combines it with the model position/scale
     (`model+0x24..0x2C`) and ignores bones. INFERRED: a fixed "above the head" marker.
3. When riding or in some statuses (`0x100842a0`, `0x10084280`, `0x10084260` on the status), out.y is raised by
   f × height (actor vtbl+0x110 or `+0x754`/`+0x75C`), with f ∈ {−0.5, −1.3} or a per-race table for statuses
   0x1D/0x25. Otherwise f = 0.

Non-skeletal actors (base vtable `0x1032db40`, `0x10082030`) return the actor position for **every** id (CODE).

**Point record lookup.** `0x10035120(id)` (CODE) works on the actor's **Sk2** resource:
```
table  = Sk2 payload + 4 + 30*N           (N = bone count, payload+2; CODE 0x10035140: res+0x34 + N*30)
u16    count  (=128 in every skeleton checked)      table+0
u16    0xFFFF                                       table+2
Point[count] at table+4, 26 bytes each:
  +0  s16 bone index
  +2  f32 rx, ry, rz      Euler angles (0x10027ab0/b00/b50 = rotation about X, Y, Z)
  +14 f32 ox, oy, oz      offset in the bone's frame
then (table + 4 + 26*count) ~1270 bytes not decoded (DATA: 1262..1274 in every skeleton checked)
```
This is the "trailing block" of dat-format §10.2: its `u16 0x0080, u16 0xFFFF` is count = 128, then 128 records.
The client does not bounds-check the id (CODE).

**Position (CODE `0x1002a6b0`):**
- `M = Rx(rx)·Ry(ry)·Rz(rz)`, translation row = (ox, oy, oz), then `M = M · Wbone[bone]`. The bone matrices are
  at skeleton+0x14, 64 bytes per bone, posed.
- `out = (0,0,0)·M`, i.e. **pos = offset transformed by the posed world matrix of `bone`**.
- vtbl+0x1D0 (generator modes 9/10) returns the whole matrix M.
- DATA: rx = ry = rz = 0 in all 29 skeletons checked (the 7 race skeletons and 22 NPC models from 1300–1447), so
  the matrix is just the bone matrix moved to the offset.

The bone matrices are taken as already world-space (the actor transform included), because no actor matrix is
applied after `0x1002a680` (INFERRED from CODE: `0x1002c310` drops its matrix arguments).

**Monsters and NPCs** use the same mechanism with the table in their own Sk2. For example, 1600 Mandragora
(33 bones) has its ring at y −0.30 and its chest point 0x15 at y −0.40. **Compute every point from the skeleton
data; nothing is per-race in the DLL.** The special ids 0x30–0x3C are resolved before the table (§G.4).

## C.7 Point ids: meanings, Hume male worked example, survey

Hume male (7072, 94 bones). Bind-pose positions are computed with the §10.2 rule `W = R(q)·T(t)·W[parent]` (Y down,
facing +x). Bone 0's frame maps offset −z to forward (+x) and offset x to world z. The meaning column is INFERRED
from the geometry.

| id | bone | offset | bind position (x, y, z) | meaning |
|---|---|---|---|---|
| 0x00 | 0 | 0 | (0, 0, 0) | root / feet |
| 0x01 | 25 | 0 | (0.05, −1.05, 0) | pelvis |
| 0x02 | 0 | (0, −2.0, 0) | (0, −2.0, 0) | above head, unposed (special path) |
| 0x03 | 51 | 0 | (0, −1.50, 0) | neck |
| 0x04 | 0 | (0, −1.5, −1.8) | (1.8, −1.5, 0) | 1.8 ahead at head height |
| 0x05 | 52 | (0.18, 0, 0) | (0.05, −1.81, 0) | top of head |
| 0x06 | 52 | (0.05, −0.12, 0) | (0.15, −1.66, 0) | face / mouth |
| 0x07 | 50 | (0.05, −0.13, 0) | (0.17, −1.32, 0) | front of chest |
| 0x08 / 0x09 | 31 / 37 | (0, −0.06, 0) | (0.05, 0.01, ∓0.16) | feet (−z side, +z side) |
| 0x0A / 0x0B | 66 / 80 | (0.08, 0, ∓0.04) | (0.04, −0.86, ∓0.31) | hands (−z side, +z side) |
| 0x0C | 0 | (0, −1.81, 0) | (0, −1.81, 0) | height marker (= model height; Galka −2.30, Taru −1.00) |
| 0x0D–0x14 | 0 | ring at y −1.10 | 0x0D (0.24, −1.1, 0) front; 0x11 (−0.17, −1.1, 0) back; 0x0F/0x13 sides (z ∓0.32) | the eight **hit locations** used by 0x31–0x33 |
| 0x15 | 0 | (0, −1.24, −0.04) | (0.04, −1.24, 0) | **body centre** (root-relative, follows only bone 0) |
| 0x16 | 52 | (0.06, 0, 0) | (0.03, −1.69, 0) | head |
| 0x17 | 0 | (0.08, −1.2, −1.2) | (1.2, −1.2, 0.08) | 1.2 ahead, chest height |
| 0x18–0x25, 0x28 | 68 | (0, 0, −0.37 … −1.2), 0x22 (0, 0.44, −0.8) | along the −z-side hand's axis | **points along the main weapon** (0x1C/0x1D 0.37, 0x28 0.6, 0x18 0.95, 0x21/0x23–0x25 1.0, 0x20 1.2, 0x1E/0x1F ±0.5) |
| 0x19 | 0 | (0, 0, −2.0) | (2.0, 0, 0) | 2.0 ahead on the ground |
| 0x1A / 0x1B | 59 / 58 | 0 | (0.09, −1.66, ∓0.02) | eyes |
| 0x26, 0x2D | 4 | (0.01, 0.03, −0.11) | | on attachment bone 4 (sub weapon?) |
| 0x27, 0x29 | 0 | (0, −1.0, 0) | (0, −1.0, 0) | 1.0 above the root |
| 0x2A | 0 | (0, −1.2, −0.5) | (0.5, −1.2, 0) | 0.5 ahead, chest height |
| 0x2B | 50 | 0 | (0.05, −1.25, 0) | upper spine / chest (follows the spine, unlike 0x15) |
| 0x2C | 0 | (0, 0, −0.4) | (0.4, 0, 0) | 0.4 ahead on the ground |
| 0x2E–0x5E | 0 | 0 | root | unused (0x30–0x3C are special ids anyway) |
| 0x5F–0x6A | 8, 19, 5, 22, 10, 24, 3, 17, 15, 13, 23, 11 | 0 | | attachment bones 3–24 (weapon / prop slots) |
| 0x6B | 25 | (0, 0.15, 0.02) | (0.03, −0.90, 0) | below the pelvis |
| 0x70–0x7D | 3, 4, 6, 7, 9, 11, 12, 14, 16, 18, 20, 21, 23, 24 | 0 | | attachment bones |
| 0x7E / 0x7F | 85 / 68 | 0 | (0.05, −0.84, ±0.31) | the two hand / grip bones (0x36/0x37 remap here, §G.4) |

The other races use the same ids with their own bones and offsets (DATA, 10248/13424/16600/19776/23176/26352):
- Galka ring radius 0.5 at y −1.20 and centre 0x15 at −1.50;
- Taru ring at −0.65 and centre at −0.65;
- Elvaan ring at −1.30 and centre at −1.43;
- Mithra centre at −1.14.

**Survey of origin-word point ids (DATA; counted only where the mode reads that actor's point, §G.4 table).**

| set | generators | pointA (caster) | pointB (target) |
|---|---|---|---|
| file 0 | 320 | 0x01: 54, 0x00: 43, 0x17: 24, 0x32: 20, 0x31: 9, 0x2B: 8, 0x0A: 5, 0x15: 4 | **0x31: 84**, 0x01: 33, 0x32: 12, 0x00: 7, 0x06: 5, 0x05: 3 |
| spells 2800–3099 | 5,053 | 0x2B: 111, 0x15: 100, 0x00: 47, 0x16: 16, 0x17: 5 | **0x15: 2,573**, 0x30: 838, 0x2B: 522, 0x00: 486, 0x01: 149, 0x06: 76, 0x0A: 67, 0x05: 38, 0x16: 37 |
| WS HumeM 33227–33482 | 4,881 | 0x00: 216, 0x15: 114, 0x28: 97, 0x0B: 86, 0x0A: 69, 0x1C: 67, 0x2B: 64, 0x1E: 51, 0x18: 36, 0x01: 30, 0x23: 21, 0x17: 20, … | **0x31: 3,334**, 0x15: 143, 0x01: 96, 0x00: 79, 0x32: 59, 0x2B: 42 |

Modes in those sets: spells are 94% mode 2 (at the target); WS files are 60% mode 2, 18% mode 1 and 17% mode 4,
plus mode 9 (107) and 0x12 (40).

The points a client must get right:
1. 0x15, the body centre;
2. 0x31, the nearest ring point;
3. 0x30, the ground;
4. 0x00 and 0x01, root and pelvis;
5. 0x2B, the chest;
6. the weapon points 0x18–0x28 on bone 68 for WS trails and sparks.

This replaces the approximation in the header note ("0x31–0x33 = 0.6 × height …").

## C.8 Damage flinch, guard and death motions

**Flinch stems (DATA, Mo2 names / element count E / frame count F as stored / speed):**

| actor | `dfi?` / `dbi?` (op 0x25/0x21, not engaged) | `dfm?` / `dbm?` (engaged, actor+0xF8) | guard `gdi?` / `gdm?` |
|---|---|---|---|
| Hume male | `dfi6`/`dbi6` E94 F2 sp1.0, in base+3/+4 (7075/7076) and in the class piece-2 files (9802, 9927) | `dfm1`/`dbm1` E68 F2, in 7072 and every class file (9674, 9799) | `gdi6` E94; `gdm1` E68 in 7073/7074 and the class files |
| Galka | `dfi6`/`dbi6` E107 F2 (26355/26356) | `dfm1`/`dbm1` E71 F2 (26352) | `gdi6`/`gdm1` |
| 1600 Mandragora, 1300 NPC | `dfi0`/`dbi0` E33 / E94 F2 | `dfm1`/`dbm1` F2 | — |

- PCs carry the not-engaged flinch only as a whole-body piece "6" (94/107 tracks). The stem `dfi?` plays every
  piece present, so piece 6 alone covers it.
- All flinch motions are **2-key poses** (F = 2): the flinch is a blend into the pose and back.
- The op 0x21/0x25 fields (+0xC speed 1.0, +0x10 count, +0x14 blend-in 2.0, +0x18 blend-out 10–32) give its
  shape (INFERRED).
- Knockback `bf0?`/`bb0?` (F2) and `bf1?`/`bb1?` (F10, sp 0.5) are in the PC base.

**Death (CODE + DATA).**
- When the entity's flag `+0x134 & 0x20000` is set and its timer `+0x202` is 0, the client calls
  vtbl+0x298(`'dead'`, actor, 0) and sets `+0x202` = 90 (CODE `0x100919cd`..`0x100919f8`). The timer then counts
  down. At ≤ 0 it calls `0x10092730` (INFERRED: the post-death step, e.g. fade or despawn). What sets 0x20000 was
  not traced (INFERRED: the 0x00D/0x00E status → dead).
- `dead` is in the actor's model file:
  - PC 7072: op 0x57 `vded` (voice), op 5 `ded?` dur 116, op 0x78 at t92, op 5 `cor?` dur 2.
  - 1600: `ded?` 112, op 3 `vded`, `cor0` at t112.
  - `corp` = op 5 `cor?` only. INFERRED: used when an actor is already dead on spawn.
- Op 5 special-cases `ded?`/`cor?`: when `0x10083150(actor)` is 0 it skips the effect-file lookup and always uses
  the actor's own motions (CODE `0x1005cab2`).
- Motions (DATA):
  - Hume `ded0`/`ded1`/`ded2` F30 sp0.5; Galka F31; Mandragora `ded0` F29; NPC 1300 `ded0` F26 sp0.45;
  - `cor0`/`cor1`/`cor2` F2 (held pose). The class files also hold `ded1`/`cor1` (upper body per weapon).
- Afterwards the base motion for status 2/3 is `'cor '` (C.3, op 0x28 / `0x100c8ae0`), so the corpse pose holds.

## C.9 Corrections to earlier notes

- §S.3 op 0x5F: **kill running instances** of the named scheduler (C.3), not "start as replacement".
- §S.3 op 0x28: "caster motion chosen by `0x100c8ae0`" = return to the status base motion with blend +8 (C.3).
- §S.5 / Open items: the 0x028 result layout is now CODE (`0x1018c950`, C.1).
- dat-format §10.6 casting stems:
  - `mi0..` is **item use** (`cait`);
  - ninjutsu is `mn`, summoning and trusts `ms`, blue magic on PCs `ma`;
  - songs and geomancy have no `ca` motion and go through `lc00..02` / `lc10..11` (`sk`, `sf`, `sh`, `gh`, `gc`).
- dat-format §10.7 resting: the resting loop is **`rx1`** (statuses 7 and 33, CODE `0x100c8c00`). `sh0?`/`sh1?`
  is the wind-instrument song motion (`lc02`), not resting.
- The "cast loop until the type 4 finish" in §10.6 holds. The release is `x1`+`x2`, played by `ss..` from the
  caster's `sh..`.

## C.10 Open items

- The exact effect of `0x10085380` on the `st..` name when the old 4CC is not a `ca..`.
- Whether the PC `cate` global fallback can hit another model's `cate` (C.4).
- `0x10092730` (after death) and the source of entity flag 0x20000.
- The 1262–1274 bytes after the Sk2 point table.
- The meaning of origin mode 0x12 (40 uses in WS files).
- Op 5 +0x1E as a repeat count is INFERRED from arithmetic only.

## Z. Zone ambient effects (the sea, smoke, lamps, windmills, clouds)

A zone's model DAT (zone + 100) is also an effect file. Its `effe`, `mode` and `weat` blocks hold generators that run
with no actors. The full RE notes are in the session report; this section summarises it.

### Z.1 What starts them (CODE)
- **At load, `Gen_Init` 0x10047c10** starts a generator when all of these hold:
  - its flags (chunk+0x78) have bit 0x1000;
  - no parent block is named `weat` (and none has header info bit 31);
  - its origin mode is 0, 0xe, 0xf or 0x14.
  It runs through `Gen_Start` 0x10053100. DATA: Selbina (348) has 89 such generators, Port Bastok (336) 191.
- **The weather object** (ctor 0x1018a6b0) starts the flagged generators and auto-start schedulers of
  `weat/<weather fourcc>` (falling back to `suny`). On a weather change it swaps them (0x1018a9d0 / 0x1018aa50).
- **Schedulers start themselves** at init (0x10056980) when they contain op 0x01 or op 0x52 and no op 0xa2
  (xi-client: op 0x52 ones; the generators they start are zone generators, placed in the world).
  DATA: the seagull schedulers `kamo` s001..s003 wait on op 0x52 (a clock window), then start generator `kamo`.

### Z.2 Placement and meshes
- **Mode 0** is an identity generator matrix, so the world position is the L1 header position (+0x14). Scale and
  rotation come from the L1 ops.
- **Mesh lookup:** when no D3m matches the particle's resource name, the client walks up the blocks for an **Mmb chunk
  of that 4-character name**. It draws the zone's built mesh for that model (0x100494a0 → 0x10183330). These Mmbs are
  not placed by the Mzb.
- **Selbina's sea** is three such generators (scale 10, life 0, so one particle that lives forever):
  - `sea1` draws Mmb `sea1` (texture sea02) at y 0.9, blend 0x44, colour 80 80 80 44, scrolling V by 0.0004 per tick;
  - `sea2` draws Mmb `sea0` (sea01) at y 0.8, blend 0x44, scrolling U 0.0002 and V 0.0005;
  - `scol` draws Mmb `sea_` (untextured) at y 1.0 with **blend 0x42 = reverse subtract** (dst − src·α; CODE
    0x10046480, BLENDOP 3), colour 44 44 30 5a, with the `ksea` / `kseb` curves.
- **Default particle blend** is 0x48 (header init, CODE 0x1005140c). Port Bastok's `1sea` has no 0x1e record, so its
  sea is additive.

### Z.3 Time of day (CODE)
- **Keyframe slots:** L1 0x62 / 0x63 (0x1005039a / 0x100503d4) set a two-word slot `{curve, mode}`. DATA: the lamps
  put one at slot 0 next to an alpha channel at slot 2, so the slot must not have a third word.
- **L2 0x3e** (0x1004d3f2) writes the blue colour byte as curve(dayFraction) × 255.
- **L2 0x3f** (0x1004d51f) multiplies a per-frame factor by curve(t), where t is the day fraction, or
  fmod(day × n, 1) when n = (mode >> 5) & 0x1ff is not 0. The factor is reset to 1 each frame (0x1004a8e3). At draw
  time alpha is multiplied by it when it is below 1, and the particle is skipped when it is 0.01 or less.
- **Day fraction:** a global (0x10629ff4) of seconds mod 86400 (INFERRED: the Vana'diel day).
- **DATA:** lamp curve `k000` is lit before 0.28 and after 0.755, and dark from 0.296 to 0.735.

### Z.4 The 0x4A chunk is a sound path, not water
Its magic is `RAB\0`, version 7. It holds a node list (x, y, z, radius, at a stride of 0x20) and an edge list of index
pairs. Only sound generators read it, through L1 op 0x6b (handler 0x100500f9), which puts the emitter at the point
nearest the camera. The chunk names are water sounds (`umi`, `kawa`, `taki` ...).

### Z.5 xi-client
- `EffectSystem::startZone` runs the Z.1 rule at zone load.
- `ZoneScene::effectMesh` keeps the unplaced Mmbs as effect meshes (`dat::meshFromModel`).
- Effect-file textures decode lazily.
- The renderer draws three passes: subtract, then alpha, then additive.
- `setWeather` starts `weat/<weather>` (live: clouds in Port Bastok); `setDayFraction` drives Z.3.
- **Verified live:**
  - Selbina's sea at noon (turquoise), dusk and night (dark blue);
  - lamps lit at night and dark at noon;
  - windmills and smoke start (89 generators).
- **Open:**
  - Sun and moon (done; CODE 0x1003c485.., position 0x10187630):
    - modes 0xe / 0xf are a translation only: the sun 1000 and the moon 900 from the camera, against their travel
      directions (a = 2π·t/day, moon travel (sin a, cos a, 0), sun the opposite);
    - particle flag bits 6-8 = 3 turn the mesh's +X toward the camera: d = cam − pos, pitch = atan2(d.y, hypot(d.x,
      d.z)), yaw = −atan2(d.z, d.x), local × RotZ(pitch) × RotY(yaw);
    - the sun is the additive Mmb glow bowl `sunsphere` at scale 50;
    - the weather block's mode-0 sky (the cloud dome, Mmb `suny` at scale 9, 7, 9; stars; rain) follows the camera;
    - verified live: the moon, halo, stars and night clouds at 21:00, and the day clouds;
    - the moon's frame: L2 0x45 sets it from 0x1018bcf0, which is (Vana'diel day / 7) % 12 (days of 5,184,000 ticks
      after a half-day offset), and rolls it π/2. xi-client aligns the day with LSB's moon ((day + 26) % 84; INFERRED).
      Its D3a has texel UVs (§M.3 uvMode 1), now divided by the texture size, which also applies to every D3a with
      uvMode 1. Live: a gibbous moon;
    - open: the moon colour tables
      (L2 0x4e / 0x4f), and the lens flares' glare tint (L1 op 0x4b, a full-screen additive colour);
    - lens flares (resource type 0x39, screen-space renderer 0x10054800; done): the chunk is a D3a whose groups are
      {u8, u8, nQuad, u8}, f32 t, 3 f32, then 6 * nQuad vertices (dat::FxFlare). Element centre = s * (1 - 2t) for the
      body's screen position s (t = 0.5: the screen centre); vertex x / y in 1/16 of the screen width; v counts from the
      texture's bottom (DATA: `lf01` element 0 maps v 0.5..1, the star glow in the image's top half). Origin mode 0xe
      is the sun's (`lf01`, 17-18 elements), 0xf the moon's (`molf`, 2). The game counts visible pixels around the
      body (0x1006c1b0); xi-client tests a ray against the collision mesh instead (INFERRED stand-in: trees without
      collision do not hide it), fading over 0.25 s, and fades by the body's height over the horizon (INFERRED). The frame clear is the Weather +0x4C horizon colour (CODE 0x100115f0), which our sky's stop 0 covers;
  - Port Bastok (fixed): its harbour is three layers, ordered by the particles' depth keys (L1 0x30) with the highest
    drawn first (INFERRED direction): `down` 5000 is a black base (`col1` mesh, 00 00 00 96); `col1` 4500 is the
    water colour, from time-of-day curves `rtms` / `gtms` / `btms` (L1 0x60-0x62 slots → L2 0x3c / 0x3d / 0x3e write
    R / G / B); `1sea` 4000 is the additive surface. Drawing in file order put the black base on top;
  - the seagulls (op 0x52 windows and the Vtx flight path) are not done. `mode/pl` are point lights rather than
    geometry: no resource name, colour 6c 3a 08, and L1 op 0x58 with what look like radii (6.0, 8.0). They need lights
    in the zone shader;
  - rain (done for weather generators): the falling rain is `~1ra`, a **particle cloud**. Generator flag bit 29 means
    one particle drawing (flags & 0x1ff) + 1 copies, not "no velocity"; L1 0x56 copies the velocity / spawn /
    acceleration ops into the cloud. It spawns 300 streaks every 30 ticks, living 60:
    - pflags bit 2 centres the batch 35 above the eye, and drops spawn in a ±20 cube around it;
    - each drop moves as p0 + v0·t + ½a·t² with v0 (0, 0.3, 0) and a (0, 0.005, 0);
    - it is drawn as D3a `rain` (8×4, rain_b01) scaled (0.3, 2, 1), additive, alpha 0x26;
    - L2 0x50 scales alpha by a weather value.
    `~1h*` are ground ripple rings; `~1r*` / `rai2` are mist.
    xi-client applies the cloud reading to weather generators only: (flags & 0x1ff) + 1 drops per emission, each in the
    ±20 cube, with the velocity ops kept. Other generators keep the older no-velocity reading. Live: La Theine with
    weather 6, about 6,000 particles at 1.5 ms a frame;
  - **L0 op 0x0a, camera-distance cull (done):** the L1 header position through the generator matrix, against the
    camera eye. Emission pauses when d > rec+4 (0 = global draw distance; xi-client uses 200, INFERRED) or
    d < rec+8; rec+0xc bit 0 soft-stops the generator. Live particles stay. Live: West Ronfaure went from about 700 to
    139 particles and from 0.9 to 0.65 ms a frame;
  - point lights (done: xi-client lights zone meshes per pixel; live in Selbina at 21:00, warm pools by the houses):
    - **What they are:** resource type 0x47 is a point light (ctor 0x10056220), not a clone; 256 D3DLIGHT8 lights are
      kept, keyed by generator name;
    - **Op 0x58:** range = rec+4 × (1 + rec+0xc); intensity = rec+8 × (1 + rec+0x10) × the particle's day factor;
    - **Time of day:** L2 0x49 sets the intensity from curve `ktpl`, which is 8 at night and 0 from 0.265 to 0.748;
    - **Distance fade:** L2 0x2e fades the light over 30..50 units from the camera;
    - **Colour:** R, G, B bytes / 128, and attenuation2 = 1 / intensity;
    - **Which meshes:** the Mzb header +0x18 lists light names (256 × 0x4c), and each placement's u32s at +0x54..+0x60
      are 1-based indices into it, so at most 4 lights per mesh;
    - **Shader** (vs.1.1 at 0x10377210): add += col·I·max(N·L, 0)/d² within range, then out = c7·vcol + add·vcol;
  - seagulls (done, live 2026-10-18: a gull crossing Selbina's sky at 08:00):
    - **Trigger:** scheduler op 0x52 checks a clock in 1/60 Vana'diel seconds (global 0x10629ff8) with
      {start, end, period, accept}. It fires in the 08:00-09:00, 14:00-15:01 and 16:30-17:30 windows, starting `kamo`
      for 9000 ticks;
    - **Path:** Vtx (0x3E) is `u32 n`, 12 bytes of padding, then n × (x, y, z, 1). Birds follow it (L1 0x54 {+4 keyframe,
      +0x14 Vtx name (DATA `line`)} + L2 0x34; L2 0x34 at CODE 0x1004b389 only adds path(u) - the last point to the
      position, it writes no rotation)
      on an open Catmull-Rom curve weighted by segment length, with the ends padded by reflection. Details are in the
      session report zonefx2_re.md.
    - **Heading:** none. A gull keeps its spawn rotation (L1 0x09: 0) whichever way the path
      runs (its legs head from -102° to +146°); only L2 0x64 rolls it about X. Checked 2026-10-19 by every write of the
      particle's rot.y (p+0xe4): L2 0x13 / 0x57 / 0x62 / 0x64 / 0x67 and L1 0x0a / 0x3b / 0x79, none in `kamo` except
      0x64 (factor 0 on Y); the local matrix (0x10044c70) is scale x Euler(rot) and the world build (0x10045c70) adds
      only the generator's rotation and the view-alignment matrix (flags bits 13 / 14 = 0: bfa8+0xb90). `kamo`'s flag
      bits 12, 16 and 30 (0x40011040) are not read by either build. xi-client turned gulls along the path until then
      (an INFERRED stand-in, removed).

## S. Ground shadows (CODE + DATA, 2026-10-18; RE notes shadow_re.md)

- **Texture:** file 7052 (ROM/27/81), chunk `kage` (type 0x20, DXT3 256x256): a soft white disc on black (253 at the
  centre, 0 at radius ~124 px). Only the colour is used.
- **Draw:** right after each actor's model (CODE 0x1002b610 -> 0x10033cd0), only while the model is visible. Blend
  ZERO / INVSRCCOLOR (`dst *= 1 - src`), depth test on, no depth write, no fog or lighting, a small bias toward the
  camera (the projection rebuilt with near / far + 3e-4). Vertex colour 0x60 (0.75 under MODULATE2X, INFERRED).
- **Camera distance >= 20:** a flat quad, half-width s, 0.001 above the actor's position (strip at 0x10377360).
- **Closer:** a decal over the zone mesh's up-facing triangles in the 3x3 grid cells (4 units) around the actor:
  planar UVs u = (x - px) / s + 0.5, v = (z - pz) / s + 0.5; per-vertex colour 0x60 - 48 |y - py| (gone 2 units above
  or below); up to 1,500 triangles.
- **Size:** the model's bounding box seen from the zone light, its larger side * 5/9, / 3 (minimum 0.01).
- **Skipped:** config 0x3A = 1 (off; 2 = silhouettes for all within 40), an actor more than 2 above the camera, some
  actor states. The local player gets a projected silhouette (mode 1) instead of the blob by default (not traced:
  where its texture is rendered).
- **ffxi-linux** (`app::GroundShadows`): the blob for everyone, the player included; the box is approximated by the
  bind-pose footprint or 0.7 x the height, and the size doubled by eye (the traced value drew a smudge between the
  feet); the near decal uses the same width as the quad. `XI_NO_SHADOWS=1` turns them off.
