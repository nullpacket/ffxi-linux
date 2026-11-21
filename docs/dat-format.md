# FFXI DAT asset format (client RE notes)

**STATUS: PARTIAL — interrupted by reboot (2026-10-16).** Questions 1, 2, 3 and 4 are mostly
answered. What is left is listed under "Next steps" at the end. Section 5 (zone geometry, Mmb/Mzb) was added in a second
session the same day and is complete enough to render a zone.

Target: an unpacked `FFXiMain.dll` (`~/re/ffxi-linux/bin/FFXiMain.unpacked.dll` here)
(sha256 `1bb2f62d9d2fe731768c55045c3f1a1651c3da975d53e94c7efc888b3b2f78b1`, image base 0x10000000),
analyzed with REA + Ghidra 12.1.2, plus an offline `objdump -d -M intel` listing.
Game data: lsb-dev install (read only).

Labels: **CODE** = read from decompile or disassembly (address and REA evidence ID given).
**DATA** = measured on the install. **INFERRED** = a guess.

No REA `set_address_name`/`set_comment` calls were made before the interruption, and the Ghidra
session will be lost. The names below are the ones proposed for these addresses. Apply them in a new session. (Session 2 update: all of these, plus the section 5 names, were applied with `set_addresses_names`; ev_617d11d2..., ev_2afe7279....)

| Address | Proposed name |
|---|---|
| 0x101c2fb0 | `DatTables_Load` (FTABLE/VTABLE + ROMn tables) |
| 0x101c3640 | `DatTables_FileIdToPath` |
| 0x101c46a0 | `Dat_GetFileSize(fileId)` (INFERRED from use) |
| 0x101c42f0 | `Dat_ReadFile(fileId, buf, size, 0)` (INFERRED from use) |
| 0x100730e0, 0x10072f00 | `ResMng_LoadDatFile(out, fileId)` (two near-identical variants) |
| 0x10073360 | `ResMng_WalkChunks` |
| 0x100711a0 | `ResMng_CreateChunkObject` (factory, switch on type) |
| 0x10071cb0 | `CMoResource_ctor` (base) |
| 0x10071d30 | `CMoResource_LoadRaw` (vtable slot 7 default: copy the chunk) |
| 0x10073990 | `ResMng_InitPhase(obj, phaseMask)` |
| 0x103542b8 | `g_ResTypeNames[95]` (char* table, read at 0x10071edf) |
| 0x10178ca0, 0x10178d00 | `Zone_LoadModelDat` / `Zone_LoadZoneDats` |
| 0x1001d730 | `CYyTex_InitFromChunk` (type 27/32 slot 8) |
| 0x1001d870 | `CYyTex_ParseHeader` |
| 0x1025b0c0 | `Dat_LangFileId(index)` (language-dependent file id table; not zone related) |

---

## 1. Open/read path and chunk dispatch

### File id → path (CODE, ev_e1892d1b7bd80a6fea8efb490d5285a1216f575f99c66a937db11568f290ddfd)
- `0x101c2fb0` loads `/FTABLE.DAT`, which holds one u16 per file id (count = size/2), and `/VTABLE.DAT`, which holds one u8 per id.
  It then loops over `/ROM%d/FTABLE%d.DAT` and `/ROM%d/VTABLE%d.DAT` for n = 2.. . If `VTABLEn[id] == n`, it sets
  `vtable[id] = n` and `ftable[id] = FTABLEn[id]`.
- `0x101c3640` maps an id to a path. Let v = vtable[id] and f = ftable[id]:
  - v = 0: the file does not exist.
  - v = 1: the path is `/ROM/%d/%d.DAT` with (f>>7, f&0x7f).
  - otherwise: the path is `/ROM%d/%d/%d.DAT` with (v, f>>7, f&0x7f).
  - A separate CD mode uses `\CDROM\%d\%d.DAT` with (f/30, f%30).

### Load a file and walk its chunks (CODE)
- `0x100730e0` / `0x10072f00` (ev_644d6c568f703dcac89a5c21ccf3117a5ee5762c082f170bb6a4dfc2b3df4d8d) do the following:
  1. Check a cache keyed by fileId at obj+0x34.
  2. Get the size with `0x101c46a0(id)`.
  3. Allocate a buffer with `operator new`.
  4. Read the file with `0x101c42f0(id, buf, size, 0)`.
  5. Call `ResMng_WalkChunks` (0x10073360) on the buffer.
- `0x10073360` (ev_6a6ee8602614282a5ebf58bd50e7c26fd433872d2a17ff4d5f447f85c39ddf77) is the walker:
  - The first chunk must be type 1. If it is not, the file is a single resource: one object is created and init runs with mask 7.
  - Step to the next chunk: `p += ((info >> 7) & 0x7FFFF) * 16`.
  - Each chunk goes through the factory `0x100711a0`.
  - **Type 1 pushes** a parent onto a depth stack, which is a local array of 256 entries.
  - **Type 0 pops** the stack. When the depth would drop below 0, the walk ends.
  - Each type 0x20 or 0x1B chunk gets an extra synthetic object of **type 10 ("TexInfo")**. That object takes its name from the texture header bytes `+0x31` (the texture name), and the walker links it next to the texture.
  - After the walk there are three init passes over the children: `0x10073990(obj, 1)`, then `(obj, 2)`, then `(obj, 4)`.
- `0x10073990` (disassembly) shows the pass order:
  - Pass 1 calls vtable+0x20 for types 0x58, 0x3D, 0x20, 0x5D, 0x1B and 0x18 (textures and sounds first).
  - Pass 2 covers types 0x0A, 0x2E, 0x36, 0x2A, 0x29, 0x2B, 0x1F, 0x21, 0x25, 0x39, 0x31, 0x30, 0x13, 0x49, 0x44, 0x3E, and more beyond what was read.
  - Pass 4 covers the rest.

### Factory `0x100711a0` (CODE, ev_07320b3f5cdff2ec4f9cf5793668a150b153de40e53bcb1ecd2f6feefcf466f8)
- `switch(info & 0x7F)`, case range 0..0x5F.
- Allocation size is `chunkSize + classExtra`.
- The base ctor `0x10071cb0` runs and the class vtable is set.
- It then calls `vtable[7]` (offset +0x1C), which loads the object from the chunk. The default, `0x10071d30`, copies the whole chunk, header included, to obj+0x20. So **obj+0x20 is the chunk header and obj+0x30 is the payload**.

Vtable layout (CODE, read from .rdata):

| Slot | Offset | Role |
|---|---|---|
| 6 | +0x18 | scalar deleting dtor |
| 7 | +0x1C | load from chunk |
| 8 | +0x20 | init / create device resources (called by the init passes) |
| 9 | +0x24 | type specific |

The classes have no RTTI.

### Type table
The client has its own name table at `0x103542b8`, indexed by type and holding 95 entries (0..0x5E)
(**CODE**, referenced at 0x10071edf). "Seen" means the type occurs in 83,001 chunk-format files that walked cleanly (**DATA**).

| Type | Client name | Factory / vtable | Role | Basis |
|---|---|---|---|---|
| 0x00 | Terminate | default | end of block (pops the walker's stack) | CODE |
| 0x01 | Rmp | vt 0x1032c588 | block begin / directory (pushes the walker's stack); the block's name is the chunk name | CODE |
| 0x05 | Generater | ctor 0x10047eb0, vt 0x1032b114 | particle/effect generator | name; INFERRED |
| 0x06 | Camera | vt 0x1032c5d0 | effect camera | name |
| 0x07 | Scheduler | vt 0x1032c644 | effect timeline/scheduler | name |
| 0x0A | TexInfo | synthesized by the walker | texture registration record (never in files) | CODE |
| 0x19 (25) | KeyFrame | vt 0x1032c5fc | keyframe curves | name |
| 0x1B (27) | Bmp2 | vt 0x1032c8ac (texture class) | texture (older flag form, 0x01) | CODE + DATA |
| **0x1C (28)** | **Mzb** | ctor 0x10176e70, vt 0x103366c4, init 0x10176ec0 | **zone map: object placement (and collision, INFERRED)**; init skips name 'fune' (ship) | CODE (name), INFERRED (contents) |
| 0x1F (31) | D3m | vt 0x1032c74c | effect mesh (`CMoD3mSpecularElem`) | INFERRED |
| **0x20 (32)** | "D3s" in the table, but the factory builds the **Bmp2 texture class** (vt 0x1032c8ac) | init 0x1001d730 | **texture (IMG)**; the walker adds TexInfo for it | CODE + DATA |
| 0x21 (33) | D3a | vt 0x1032c720 | effect anim/lightmap (`CMoD3aLightmapElem`) | INFERRED |
| 0x25 (37) | D3b | vt 0x1032c6f4 | effect billboard? | INFERRED |
| 0x29 (41) | Sk2 | vt 0x1032c828, init 0x10034070 ("Sk2 Version Error") | skeleton/bones | CODE |
| 0x2A (42) | Os2 | vt 0x1032c854, init 0x1002d720 ("Os2 Version Error", `CYyOs2VtxBuffer`) | character/object mesh (vertex buffers) | CODE |
| 0x2B (43) | Mo2 | vt 0x1032c7fc, init 0x10019660 ("Mo2 Version Error") | motion/animation | CODE |
| **0x2E (46)** | **Mmb** | ctor 0x10176d90, vt 0x10336698, init 0x10176de0 → 0x10179200 | **zone model mesh block** | CODE (name), DATA (261 of them in the zone file) |
| 0x2F (47) | Weather | ctor 0x101892c0 | zone weather/lighting/fog | name + "Weather file" string |
| 0x30 (48) | Meb | vt 0x1032c69c | ? | |
| 0x31 (49) | Msb | vt 0x1032c6c8 | ? | |
| 0x36 (54) | Rid | ctor 0x10176ef0 | zone region/ID data (first chunk of the zone DAT) | INFERRED |
| 0x3D (61) | Sep | vt 0x1032c530 (shared with 0x58) | sound effect (`CYySepRes`) | name |
| 0x3E (62) | Vtx | vt 0x1032c55c | ? | |
| 0x45 (69) | Cib | vt 0x1032c7d0 | ? | |
| 0x4A (74) | Sph | vt 0x1032c504 | sphere resource (`CMoSphRes`)? | INFERRED |
| 0x54 (84) | Afb | vt 0x1032c778, ctor calls 0x1001ceb0 ("Afb Version Error") | afterimage? | INFERRED |
| 0x58 (88) | Spw | vt 0x1032c530 | sound related (same class as Sep) | INFERRED |
| **0x5D (93)** | Bp | vt 0x1032c880, init 0x1001de90 | **bump/height map** | DATA (see §4) |
| 0x5E (94) | Max | vt 0x1032c7a4 | ? | |

## 2. Chunk header semantics (corrected)

```
struct ChunkHeader {      // 16 bytes
  char     name[4];       // not NUL-terminated; the block name for type 1
  uint32_t info;          // bits 0..6 type, bits 7..25 size/16, bits 26..31 flags
  uint8_t  pad[8];        // zero in every clean chunk file
};
```
- **The size must be masked**: `size = ((info >> 7) & 0x7FFFF) * 16`. The size includes the header. CODE: 0x100711a0, 0x10071d30, 0x10073360.
- **Type** is `info & 0x7F`. CODE: factory switch, walker.
- **Bits 26..31 are flags.** The client masks them out of the size.
  - DATA: every type-47 Weather chunk has `info>>26 == 4`. Other values seen (24, 12, 44, 32...) are on types 5, 6, 7 and 1.
  - CODE: the walker keeps `info & 0xFC00027F` when it builds a TexInfo, and 0x100fb2f8 reads `obj+0x24 >> 26` (bit 0 → a render flag).
  - Meaning: INFERRED / open.
- **Pad**: DATA shows all zero in 83,001 clean files. The only exceptions are 3 chunks in file 5212. The client copies the pad but nothing was found that reads it.
- **Nesting** (CODE + DATA): type 1 opens a block and type 0 closes it. Blocks nest (a zone file has `t_sa{ Rid, mode{ Mzb, textures..., Mmb... } ... }`). Only 2 of 83,001 files are unbalanced. The walker stops when the outermost block closes.
- File coverage (DATA): 83,001 of 86,335 existing files are clean chunk files (they walk exactly to EOF, start with type 1, and use types ≤ 0x5F). The rest are other formats (tables, sound, etc.).

## 3. Zone id → DAT file id

CODE (disassembly, `Zone_LoadZoneDats` 0x10178d00 and its helper 0x10178ca0; same arithmetic at 0x10178748):
```
model_fid(zone) = (600 <= zone && zone < 700) ? zone + 83191 /*0x144F7*/ : zone + 100 /*0x64*/
bump_fid(zone)  = (600 <= zone && zone < 700) ? zone + 83991 /*0x14817*/ : zone + 39831 /*0x9B97*/
```
- `bump_fid` is loaded only when `[mgr+0x39498] != 0`, which looks like a graphics option.
- File `0x1B8C` (7052) is also always loaded. It holds a single texture named "kage" (shadow).
- The id goes through identity hooks 0x10178ce0 / 0x10178cf0, then `ResMng_LoadDatFile` 0x10072f00.

DATA verification:
- Zone 230 → 330 = ROM/1/31.DAT, 9,511,280 bytes. It walks cleanly and contains:
  - Rid (54) ×1
  - **Mzb (28) ×1**
  - textures (32) ×68
  - **Mmb (46) ×261**
  - Generater/Scheduler/KeyFrame/Sep/Weather (47) ×42
- Zone 230 → bump file 40061 = ROM/92/77.DAT holds 44 Bp (93) chunks. Their names match zone 230's textures (arch, rerf, door).
- Zone 600 → 83791 = ROM/309/10.DAT contains Mzb and Mmb, which confirms the 600–699 branch.
- Every file id from 100 to 536 contains an Mzb chunk, which matches zone + 100 for zones 0..436.

So geometry is in the Mmb (46) chunks and the map/placement data is in Mzb (28), both in `model_fid`. That collision lives inside Mzb is INFERRED and not yet confirmed from code.

## 4. Texture chunk (types 0x20 and 0x1B)

CODE: 0x1001d730 → 0x1001d810 → `0x1001d870` (disassembly), plus the "Bm2 Version Error" string at 0x10351204.
DATA: checked on 8,986 texture chunks from 6,000 random files. Every one matches the computed size, with less than 16 bytes of padding.

Offsets are relative to the payload, which is the chunk start + 16:
```
+0x00 u8   flags: kind = (flags>>4)&7; bit7 = "has DXT / alt data"; low nibble = 1 (unused?)
+0x01 char name[16]   e.g. "model   arch    " (8-char category + 8-char name); "cubemap " sets the cubemap flag
+0x11 BITMAPINFOHEADER (40 bytes): biWidth@+4 -> u16 width, biHeight@+8 -> u16 height,
      biBitCount@+0xE, biClrImportant@+0x24 = palette entry bits (16/24/32)
+0x39 body, chosen by kind:
  kind 0: logs "Bm2 Version Error" and continues as kind 1      (type 27, flags 0x01)
  kind 1: [palette][pixels]                                     (flags 0x91)
  kind 3: u32 extra (low nibble -> tex flags), then same as kind 1   (flags 0xB1)
  kind 2: DXT (flags 0xA1): u32 fourcc stored byte-reversed in the file ("1TXD"/"3TXD"... = DXT1..DXT5),
          u32 dataSize, u32 blockRowPitch (DXT1: w*2, DXT3: w*4), then dataSize bytes of DXT blocks
```
- Palette size:
  - 4 bpp: 16 entries × (biClrImportant/8) bytes.
  - 8 bpp: 256 entries × (biClrImportant/8) bytes.
  - Entries are 4 bytes when biClrImportant is 32. The colour order is BGRA (INFERRED, D3D convention).
- Pixel bytes: 4 bpp = (w·h+1)/2, 8 bpp = w·h, 16 bpp = 2·w·h, 24 bpp = 3·w·h, 32 bpp = 4·w·h.
- For kind 2 the client checks the fourcc against device caps (`[0x1045666c]+0x910/0x911` for DXT1/DXT3). It stores the data pointer as fourcc+4. That the u32 after the fourcc is dataSize and the next is the pitch is DATA.
- In kinds 1 and 3 with bit 7 set, the client also looks after the pixels for a DXT fourcc (CODE, 0x1001da64). Not seen in the data so far.
- biBitCount is **not** reliable for DXT. Values of 4, 8, 16 and 32 were seen across DXT1 and DXT3.
- Observed mix (DATA):

  | flags | format | count |
  |---|---|---|
  | 0xA1 | DXT3 | 8,361 |
  | 0xB1 | 8 bpp paletted | 567 |
  | 0xA1 | DXT1 | 18 |
  | 0x01 | type 27, 8 bpp | 2 |

- Worked example: file 330, chunk "arch". 128×128 DXT3: payload 16,464 = 57 + 12 + 16,384 + 11 bytes of padding.

**Alpha scale (DATA, 2026-10-16).** Texture alpha follows the PS2 convention, where 0x80 means fully opaque. Across every DXT3 texel in zone file 330:
- no value is above 0x88;
- 84% of texels are 0x77–0x88;
- 11% are 0.

Renderers must use `min(a * 2, 1)`. Not yet confirmed in the client's D3D texture-stage / alpha-test setup.

**Orientation (CODE, #79).** DXT blocks are stored top-down: the San d'Oria banner crest decodes upright with no flip.
Every other format (8 / 4 bpp paletted, 32 bpp: flags 0xB1, 0x91 item and status icons, type 0x1B) is stored
bottom-up, as in a BMP: the client's fill (CODE 0x10039d80) writes destination row y from source row height − 1 − y in
each of its non-DXT branches, while the DXT path copies the blocks as they are. `parseTexture` turns those rows
top-down, so `Texture::data` is always top-down. DATA: 209 of the 240 map textures are 0xB1 (Tahrongi Canyon, file
5330, has its title scroll at the top and compass at the bottom once flipped, like the DXT maps); the Potion and
Bronze Cap icons stand upright only after the flip. Texture data hashes (the HD texture cache keys) of non-DXT
textures changed with this.

Bp (type 93) bump map, DATA (file 40061):
```
+0  "bp\0\0"
+4  u16 width
+6  u16 height
+8  zero[8]
+16 name[16]          e.g. "bump    arch    "
+32 w*h bytes         8-bit height values
```

## 5. Zone geometry: Mmb / Mzb

Session 2 (2026-10-16), same binary. REA/Ghidra names applied this session (ev_617d11d2..., ev_2afe7279...):
`CMoMmb_ctor` 0x10176d90, `CMoMmb_Init` 0x10176de0, `CMoMmb_Register` 0x10179200, `Mmb_DecodeAndBuild` 0x1016fbe0,
`Mmb_DecodeBlockSwap` 0x10170d90, `CMoMzb_ctor` 0x10176e70, `CMoMzb_Init` 0x10176ec0, `Mzb_DecodeAndBuild` 0x10179340,
`Mzb_FindMmbByName` 0x1017ae90 (→ 0x1017aef0), `Mzb_FixupQuadtree` 0x1017c730, `Mzb_FixupCollision` 0x10167e60,
`g_MmbXorKeyTable` 0x10376ac0, `g_MmbSwapKeyTable` 0x10376de8, `g_MzbKeyTable` 0x10376ef0 (byte-identical copy of 0x10376ac0).
(The Ghidra provider does not support `set_comment`; evidence IDs are recorded here only.)

Reference implementation: `tools/zone_to_obj.py` (the two 256-byte key tables are embedded as hex).
All offsets below are relative to the chunk **payload** (chunk start + 16).

### 5.1 Call path (CODE)
- `CMoMmb_Init` (vtable 0x10336698 slot 8; disassembly 0x10176de0) calls `CMoMmb_Register(payload, chunkName4, size&0x7ffff0, ...)`
  which calls `Mmb_DecodeAndBuild` 0x1016fbe0 (ev_50ff9aec... for 0x10179200, ev_25f9d9cb... for 0x1016fbe0). The return value
  (a mesh object) **overwrites the first dword of the payload** (`mov [edi],eax` at 0x10176e62), so the client decodes in place
  and only once.
- `CMoMzb_Init` 0x10176ec0 skips a chunk whose 4-char name is `fune` (0x656e7566), else calls `Mzb_DecodeAndBuild(payload, ...)`
  0x10179340 (ev_cfb9f134..., ev_50ff9aec...).
- Both decode **in place**; the decode is a pure function of the payload bytes plus the static tables, so it can run offline.

### 5.2 Mmb (type 46) decode — CODE (0x1016fbe0 disassembly + ev_25f9d9cb...; 0x10170d90 ev_e09a2e74...)
```
len = u32(p+0) & 0xFFFFFF          // bytes covered, counted from payload start
// stage 1: XOR keystream, only if version byte p[3] >= 5
if p[3] >= 5:
    key = T1[p[5] ^ 0xF0]          // T1 = 0x10376ac0, int accumulator
    c = 0
    for i in 8 .. len-1:
        x = key & 0xFF
        w = (x << 8) | x           // 16-bit
        c += 1; key += c
        p[i] ^= (w >> (key & 7)) & 0xFF
        c += 1; key += c
// stage 2: 8-byte block swap, only if p[6] == 0xFF and p[7] == 0xFF (u32(p+4) & 0xFFFF0000 == 0xFFFF0000)
if p[6] == 0xFF and p[7] == 0xFF:
    k3 = p[5] ^ 0xF0               // byte
    k2 = T2[k3]                    // T2 = 0x10376de8, byte
    half = ((len - 8) & ~0xF) / 2
    for pos in 0, 8, 16, ... < half:
        k3 = (k3 + 9) & 0xFF
        bit = k2 & 1
        k2 = (k2 + k3) & 0xFF
        if bit: swap 8 bytes at p+8+pos with 8 bytes at p+8+pos+half
```
Bytes 0..7 are never transformed. DATA: version 5 meshes (97%) are XOR-encoded; versions 4, 3 and 0 are stored plain.
Stage 2 never fired in the zones checked (p[6..7] != FFFF). Stage order (XOR then swap) is from the code; untested on a
swapped file.

### 5.3 Mmb layout after decode
Header (CODE: field reads in 0x1016fbe0; DATA: values from file 330):

| Off | Type | Meaning | Basis |
|---|---|---|---|
| 0x00 | u24 | length used by the decoder | CODE |
| 0x03 | u8 | version (5, 4, 3 seen; 0 once). `version <= 1` ⇒ sub-mesh count is fixed at 9 | CODE + DATA |
| 0x04 | u8 | **vertex/primitive type** (see below); the full u32 at +4 is also the swap marker | CODE |
| 0x05 | u8 | key byte (`^0xF0`) | CODE |
| 0x06 | u16 | 0xFFFF ⇒ block-swap stage applies | CODE |
| 0x08 | char[8] | group/category, e.g. `tshimono`, `tchiba\0\0` (not read by the build code seen) | DATA |
| 0x10 | char[16] | **model name**, space padded, e.g. `pa_passage02    `; Mzb placements reference this | CODE (copied to obj+8..0x14) + DATA |
| 0x20 | u32 | sub-mesh count N | CODE |
| 0x24 | f32[6] | bbox: minX, maxX, minY, maxY, minZ, maxZ (corner order from 0x1016fbe0 expansion into 8 corners) | CODE |
| 0x3C | u32[N] | offsets (from payload start) of each sub-mesh; 0 = absent | CODE |

Sub-mesh at `off` (CODE):

| Off | Type | Meaning |
|---|---|---|
| +0x00 | u32 | piece count M |
| +0x04 | f32[6] | sub-mesh bbox (same order) |
| +0x1C | u32 | unknown (copied, e.g. 0x14, 0x74) |
| +0x20 | Piece[M] | contiguous |

Piece (CODE: 0x1016fbe0 loop; DATA validates for all pieces in 400 zones, see 5.6):
```
+0x00  char[16] texture name, e.g. "model   kabe    " — exactly the 16-byte name at texture payload +0x01
       (sec. 4). All-spaces = untextured. Lookup: 0x10039740 builds a key from the name, 0x1003ac70/0x1003ac00 resolve it.
+0x10  u32  nv | flags<<28   nv = w & 0x0FFFFFFF; bits 31,30,29,28 stored as four separate booleans (meaning unknown;
                             DATA zone 230: bit31 on 105 pieces, bit29 on 14, bit28 on 7)
+0x14  Vertex[nv]            stride by type (below)
+..    u32 ni
+..    u16 index[ni], then pad to an even count (advance = ((ni+1)&~1)*2)
```
Vertex/primitive type (byte +4) — CODE (fields set at obj+0x1c..0x3c in 0x1016fbe0):

| type | file stride | layout | D3D primitive | count |
|---|---|---|---|---|
| 1 (DATA: ~97%) | 0x24 | pos f32×3, normal f32×3, colour u32 (D3DCOLOR, BGRA), uv f32×2 | TRIANGLESTRIP (5) | ni−2 |
| 2 (~2%) | 0x30 | pos f32×3, **pos delta** f32×3, normal f32×3, colour u32, uv f32×2 | TRIANGLELIST (4) | ni/3 |
| 3 (set at runtime from 1 when bump maps load) | 0x24 | as 1 | strip, de-indexed | |
| other (0 seen 3×) | 0x24 | as 1 | TRIANGLELIST (4) | ni/3 |

- FVF used for the GPU copy is 0x152 = XYZ | NORMAL | DIFFUSE | TEX1 (CODE, `obj+0x20 = 0x152`), which matches the 36-byte layout.
- Type 2 is animated: the client builds `N` position streams (N = 1, 16 or 32 depending on a graphics setting `DAT_10456ac0`)
  each `pos + (k/(N-1)) * delta`, and one 24-byte stream of normal/colour/uv from +0x18 (CODE). For a static render use `pos`.
- Strips: degenerate triangles (repeated index) are skipped (CODE: the type-3 expansion loop counts only triangles with
  three distinct indices); odd strip positions use order (a, c, b) (CODE: the same loop alternates with
  `local_38 = 1 - local_38`; DATA: this gives 99.9% normal-consistent winding, 5.5).
- Vertex positions are **model-local** (bbox of `pa_passage02` is ~±3 units); world placement comes only from Mzb (5.4).

### 5.4 Mzb (type 28) decode and placement table
Decode — CODE (0x10179340 disassembly 0x10179340..0x101793f3):
```
len = (u24(p+0)) - 8
if p[3] >= 0x1B:
    key = T1[p[7] ^ 0xFF]          // table read as *(0x10376fef - p[7]); 0x10376ef0 == T1
    c = 0; pos = 0
    while pos < len:
        k = key & 0xFF
        c += 1; key += c
        n = ((k >> 4) & 7) + 16    // 16..23 bytes
        if (k & 1) and pos + n < len:
            p[8+pos .. 8+pos+n-1] = ~p[...]    // bitwise NOT
        pos += n
```
Header (CODE: 0x10179340; DATA: file 330 values):

| Off | Type | Meaning | 330 |
|---|---|---|---|
| 0x00 | u24 | length | 0x2c9ce4 |
| 0x03 | u8 | version (DATA: 0x1B in all but one zone, which has 0x17 and is not encoded) | 0x1B |
| 0x04 | u24 | **placement record count** (`u32 & 0xFFFFFF`) | 1675 |
| 0x07 | u8 | key byte | 0x91 |
| 0x08 | u32 | offset of the **collision** block (0 = none) | 0x55fb8 |
| 0x0C | u8 | copied as `version`; 0x0D..0x0F: three u8 → collision grid dimensions (passed to 0x10167e60 as `param_1[5..7]`) | 28 28 28 28 |
| 0x10 | u32 | offset of the visibility quadtree (version ≥ 0x15) / a count (older) | 0x30a8c |
| 0x14 | u32 | offset of a `u32 count, u32 entries[]` list (version ≥ 0x15; follows the records) | 0x28e6c |
| 0x18 | u32 | offset of an extra block, version > 0x11 only (0x1017a5e0) | 0x513b8 |
| 0x1C | u8@0x1D | flags passed to the collision builder (bits 0,2,3,4) | |
| 0x20 | Record[count] | placement records, 100 (0x64) bytes each | |

Placement record, 100 bytes (CODE: loop in 0x10179340, `local_534` = record+0x10, stride 0x19 dwords; DATA: 330):

| Off | Type | Meaning |
|---|---|---|
| 0x00 | char[16] | **Mmb model name**; if version > 0x1A every byte is XORed with **0x55** (CODE). Matches Mmb +0x10 exactly (DATA: 1675/1675 resolve in zone 230) |
| 0x10 | f32×3 | translation x, y, z |
| 0x1C | f32×3 | rotation x, y, z in **radians** (fed straight to fsin/fcos) |
| 0x28 | f32×3 | scale x, y, z |
| 0x34 | u32 | **door id** (fourcc, e.g. `_6w0`; 0 otherwise), copied to instance+0xC0 (CODE 0x1017a03f); see §5.8 |
| 0x38, 0x3C, 0x40 | f32 | three distances, stored squared (LOD/draw distances, INFERRED; 10/100/1000 in samples). The second one stored is `max(d1, d2)²` |
| 0x44 | u8 ×4 | flags: +0x44, +0x45 copied, +0x46 bit0 and bit2 extracted, +0x47 copied |
| 0x48 | u32 | offset (from payload) of per-instance data, 0 = none |
| 0x4C | f32/u32 | copied (unknown) |
| 0x50 | u32 | copied (unknown) |
| 0x54..0x63 | u32×4 | only for version ≥ 0x12; small integers (0,1,2,3,4...), INFERRED link/light indices |

Instance lookup: `Mzb_FindMmbByName` 0x1017ae90 searches loaded Mmbs by the 16-byte name (CODE). DATA: in zone 230, 238 of
256 unique Mmb names are placed; the other 18 (fountains `01hunsui`, lights `ltcube`/`ltlanp`, windows...) are not
in the Mzb, so other systems (effects/events) place them. Duplicate Mmb names occur (261 chunks, 256 names); the first wins in
the tool (INFERRED: they are identical copies).

Other Mzb blocks (CODE, not fully documented):
- Collision (`Mzb_FixupCollision` 0x10167e60, called from 0x10179340 with header+0x08): a header of
  `{meshCount, meshOff, n2, off2, gridOff, off5, count6}`; meshes hold float3 vertex and float3 normal arrays and 8-byte
  triangles `{u16 v0 & 0x7FFF, u16 v1 & 0x3FFF (+flag 0x4000 = degenerate), u16 v2 & 0x3FFF, u16 normal & 0x7FFF}`;
  a grid of `(dimA*dimB/4)×(dimC*dimD/4)` cell pointers. So **collision lives in the Mzb** (CODE), separate from the render
  meshes.
- Visibility quadtree (`Mzb_FixupQuadtree` 0x1017c730, header+0x10): node +0x60 = leaf list offset, +0x64 = count of
  placement indices; else +0x68..+0x74 = four child offsets.
- Special case (CODE): zone 0xD5 (213) placement `es_anmo` at (-57.9466, -15.7264, -269.387) gets a hide flag.

### 5.5 Coordinate conventions
- World transform per placement (CODE, 0x10179340; matrices built with D3DX row-vector layout):
  `W = S(scale) · Rx(rot.x) · Ry(rot.y) · Rz(rot.z) · T(pos)`, vertex `v' = v · W` (row vector).
  Rx = [[1,0,0],[0,c,s],[0,-s,c]], Ry = [[c,0,-s],[0,1,0],[s,0,c]], Rz = [[c,s,0],[-s,c,0],[0,0,1]] (standard
  D3DXMatrixRotationX/Y/Z). Equivalently, in column-vector form: `v' = T·Rz·Ry·Rx·S·v`; X rotation is applied first.
- The client flags a mirrored instance when `scale.x * scale.z < 0` (CODE, `instance+0x98 = 1`); DATA: scale.y is never
  negative in 28 zones, while 3,476 placements have negative sx·sz, so flip winding for those.
- **Axes (DATA):** world coordinates are exactly server (LSB) coordinates. In zone 230 the floor directly under the
  spawn point (-0.63, 54.37) is at **y = -2.00**, matching the spawn y. **Y points down**: roofs above the spawn are at
  y = -15.4, the 1st–99th percentile of y is -28.0..3.6. Handedness is D3D left-handed.
- **Winding (DATA):** for 99.9% of triangles (67,691 of 67,782 in zone 230's unique meshes), `(B-A)×(C-A)` is opposite to the stored vertex normal. So front faces are clockwise in the stored space, the D3D default.
- **Compass (DATA, from LSB zone lines):** game +x is **east** (the East Ronfaure gate is at x = +113) and +z is **north** (the Northern San d'Oria gate is at z = +58).
- **Right-handed Y-up rendering:** use a 180° rotation about X, `(x, y, z) → (x, −y, −z)`.
  - This keeps clockwise front faces, so the pipeline's front face is set to clockwise.
  - **Do not** negate only y. That is a reflection, which mirrors the world east–west. xi-client shipped that bug on 2026-10-16 and it was caught by the user.
  - Verified: an aerial view looking north shows `z6e0` (East Ronfaure) on the right and `z6e2` (West Ronfaure) on the left.
- Positions need only the placement transform; there is no extra per-zone offset (DATA, spawn check).

### 5.6 Validation (DATA)
- Zone 230 (file 330): 1 Mzb (1675 placements), 261 Mmb (256 unique names), every placement resolves.
  OBJ: **278,490 vertices, 204,889 triangles**. World bbox (game coords): x -306.1..206.2, y -69.3..10.0,
  z -119.8..346.8 (1st–99th percentile x -285.6..182.1, z -98.2..275.7: the bbox includes the out-of-bounds backdrop
  scenery, not just the walkable ±150 area).
- Texture names: every non-blank piece texture name in zones 230/104/245 exists as a texture chunk name in the same file.
- 28-zone sample (0,1,2,3,4,11,24,26,50,87,100,104,118,126,140,145,160,175,191,230,231,232,236,243,245,250,290,291):
  **28/28 parse cleanly** (every index < vertex count, every placement name resolves). Mmb versions 5:5777, 4:91, 3:82, 0:1;
  vertex types 1:5827, 2:121, 0:3; Mzb versions 0x1B:29, 0x17:1.
- Full sweep, zones 0..299 and 600..699 (400 ids): 321 have a model DAT with an Mzb (79 have no file or no Mzb).
  **0 decode/parse failures**; every index is in range. 320/321 resolve every placement; zone 220 (ship to Selbina) has a
  second Mzb chunk named `ship` whose 12 of 1873 placements name meshes not in that file (INFERRED: the ship model lives
  in another DAT). Totals: Mmb versions 5:49,220, 4:400, 3:82, 0:1; vertex types 1:48,805, 2:894, 0:4; Mzb versions
  0x1B:335, 0x17:1; one placement with negative scale.y, 61,768 with sx·sz < 0.
- The tool merges every Mzb chunk except one named `fune` (as `CMoMzb_Init` does) and skips unresolved names.

### 5.7 Open (geometry)
- ~~Piece flag bits 28..31~~ — identified (CODE, sec. 6): `Mmb_DecodeAndBuild` stores them as 16-bit booleans in the
  0xC0-byte built piece record: **bit31 → piece+0x20 = alpha-blended** (blend on, Z-write off, Z-bias),
  **bit30 → +0x22 = no reader found**, **bit29 → +0x24 = double-sided** (cull none),
  **bit28 → +0x26 = alternate light/fog set**, read only by the attached-object pass `0x10183cb0`.
- Mmb +0x08 group name and sub-mesh +0x1C are not read by the build path seen.
- Mzb record fields 0x4C, 0x50, 0x54..0x63 and the 0x18 block (0x34 is the door id, §5.8; 0x54..0x60 the point lights).
- ~~Who uses Mmbs not referenced by the Mzb~~ — answered 2026-10-20 (`xi-dat unplaced ZONE...`, all 298 zones: 10,951
  unplaced Mmbs). **4,217 are LOD variants** of placed meshes (§6.3: a placement naming `..m` also draws `..h` near and
  `..l` far), **5,234 are drawn by the zone file's own generators** (no D3m of that name, so the particle takes the
  Mmb: the sea, windmills, lamp glows, fountains; effect-format §Z.2). The other ~1,500 have no reference anywhere in
  their zone file (Selbina's nets `_ami`, plates `sara`, knives `houtyou`: only their chunk header names them; Port
  Bastok's tents and crates): INFERRED unused or placed by cutscene data not traced. The game's only by-name lookups
  are `Mzb_FindMmbByName` (placements) and 0x10183330 (generator particles; both through 0x10170e40).
- Stage 2 (block swap) has not been seen in the data; test it if a file with p[6..7]=FFFF turns up.

### 5.8 Doors (CODE + DATA, live 2026-10-18; RE notes in the session report doors_re.md)

- **Parts:** placements whose record +0x34 id starts with `_` or `@` belong to door (or lift) `id`; up to 4 per id,
  in record order, are its parts (CODE 0x1017a860). Selbina `_6w0`..`_6w3`: `doorl` (part 0) + `doorr` (part 1).
  Zone 230: `_6e0`..`_6ez`, 2 leaves each (one mirrored, scale z −1).
- **Entity:** S2C 0x00E look kind 2 (door; 3 lift, 4 ship) carries the id at +0x34 (LSB yaml `door_id`, e.g.
  811939423 = `_6e0`). The entity draws nothing.
- **Effect block:** the zone file's block named by the id holds schedulers `open`, `clos` (and optional `into` /
  `intc` at spawn; `evst` / `eved` for model-less ones such as Jeuno's street lamps). Animation byte (0x00E +0x1F)
  8 / 26 / 29 / 45 runs `open`, 9 / 27 / 30 / 46 runs `clos` (CODE 0x1008c7c6).
- **Motion:** scheduler ops 0x0D / 0x0C rotate / offset part i over a duration (effect-format §S.3). The part's
  matrix is `v · R(rot) · T(off) · W0` in row vectors (CODE 0x100ace20), W0 the placement's: it turns about the mesh
  origin, which is the hinge. Zone 230 `_6eq`: both parts to (0, 1.3962, 0) over 70 ticks, sound `9021`; `clos` back
  to 0 with `9022`. Live: talking to Door:Rosel's Armour opens it (the server sends 8, then 9 about 7 s later).
- **Collision:** the door meshes do not collide; a thin Rid box with the same id blocks the doorway, disabled from the
  start of `open` (anim 45 opens but keeps it) and re-enabled at `clos` (CODE 0x1017ab50). xi-client pushes the character out of solid boxes in
  `ZoneScene::move` (the box y is its centre; live: walking into the closed `_6eq` stops at the door plane). Boxes
  with no parts (Selbina `_6ww` dock gate) only toggle collision.
- Model-less door entities (no parts; Lower Jeuno's street lamps) run `ints` / `inte` at spawn and `evst` / `eved`
  on open / close instead (CODE 0x100acad0). Live 2026-10-20 (BotD in Lower Jeuno, `!animatenpc 17780881..3 8` /
  `9`): `evst` runs, starts the lamp's `lton`, which starts its own `RRnn` (Mmb `rnp2`) and `swnn` (light `ligh`) at the
  lamp's position (`_l00` at (-118.1, -1.2, -171.1), `_l02` at (-102.1, -1.2, -143.5)); `eved` on close. Each lamp block
  is `_lNN { ltXX { swXX, RRXX, lton, textures, rnp0 / rnp2 }, ligh { ints, inte, evst, eved } }`. The glow was not
  judged on screen (daytime test; the lamps' light curves dim it by day).
- **Lifts** (CODE + DATA, live 2026-10-18; RE notes lift_re.md): look kind 3, id `@...`; 0x00E +0x38 Time (earth
  seconds since the Vana'diel epoch) and +0x3C travel seconds. State = animation - 10 (10..17). The Rid `@` record
  gives each state's platform height: centre y + s16 / 256 at code-record +0x34, +0x36 (this doc's table: record +0x44,
  +0x46; zone 237 `@6l0` / `@6l1`: 1.962 bottom, -9.983 top). On a change the client runs `mvXY` (motor sound, op 0x1D)
  and the platform moves linearly from Time to Time + travel (task 0x10060020; xi-client moves it directly); at spawn
  it stands at its floor. Every part's world y is the height (CODE 0x100c3850); anyone inside the `@` box has its y
  set to it (CODE 0x10176530): riding is a height override, not collision. Live: Metalworks lifts cycle every ~18 s
  (about 7 s of travel) and carry the player.
- **Ships** (CODE + DATA, live 2026-10-18; RE notes ship_re.md): look kind 4 with a small id (LSB's cutscene-only props
  use animation 55). DAT 31004 + id (0 Bastok, 1 Windurst, 2 San d'Oria airships, 3 Mhaura and 4 Selbina ferries,
  5 Kazham, 6 Jeuno) holds the hull Mmb `fune`, schedulers `seq0`..`seq7` and Vtx paths. Animation 18..25 runs
  `seq(anim - 18)` from Time (0x00E +0x38): its op 0x26 / 0x27 legs {+6 duration, +8 Vtx, +0x10 flags (bit 0 absolute,
  bits 1-2 orientation, bit 3 forward: clear = end to start), +0x14 ease (0 linear, 1 sin, 2 1-cos, 4 0.5-0.5cos), +0x18
  yaw offset} move the hull along the path (yaw = offset - atan2(dz, dx)); a ship met mid-leg is placed at the right
  point. No collision: boarding is the door and LSB's region. xi-client draws the hull in its own `gfx::PropLayer` (one small renderer per ship file with a few
  instance slots; a ship arriving or leaving takes or frees a slot instead of rebuilding the zone, 2026-10-20), posed every
  frame (client::Ships); out of a leg it holds the last leg's point, or the server position. Live: the Selbina ferry
  arriving.
- Not done: the ships' wake, propellers and sounds; the lifts' motor sound.

## 6. Zone mesh render states

Session 3 (2026-10-16), same binary and REA/Ghidra session. Names applied with `set_addresses_names`
(ev_c98238fbc001a34890f11e8879e916b8323768c233f1679e4ef732631fbc5b2a): `ZoneMesh_RenderAll` 0x1017b420,
`ZoneMesh_DrawInstanceHW` 0x1017c8d0, `ZoneMesh_DrawInstanceSW` 0x1017dd30, `ZoneMesh_DrawAttachedList` 0x1017acf0,
`ZoneMesh_DrawAttachedInstance` 0x10183cb0, `D3DW_DrawIndexedPrimitive` 0x1000cc00, `D3DW_DrawPrimitive` 0x1000cb20,
`D3DW_SetTransform` 0x1000bd50, `D3DW_SetLight` 0x1000bf30, `D3DW_ApplyLightingPreset` 0x10009920,
`g_ZBiasOpaque` 0x105ff518, `g_ZBiasBlend` 0x105ff3f8.

Evidence:
- ev_6e18071369bcea32d1bf4140a11fa2b972828321144692f3dfff8cd8774fd08d: decompiles of 0x1017b420, 0x10009920,
  0x1017aef0 and 0x101780e0.
- ev_e2d623604749c6e0bc4177d225bb8b8dac70ff456232d062576a68527ff6eab8: decompiles of 0x1017dd30 and 0x1017c8d0.
- ev_fc2cd76cc85cbffd3dec1ce24218eef81004d3f1166e5a98fa3cfaaee8c01d29: 0x10183cb0.
- ev_0c04884ea72d4d834c41ef5781c86ca091053da8ba34a07f9dd56690a65eebd2: 0x1017acf0.
- ev_9348c09e80fe9e4553945ffa77eb27ae245e82d1b28a13f71a28e291cfb4823f: `Mzb_DecodeAndBuild`, for the instance fields.
- ev_b0b88e2c82c529fa0cb5103242f16997d7078df01ec55f80fd4c1fc54446e49d: jump table 0x1017dd1c.

Ghidra drops the pushed arguments of the per-piece code in 0x1017c8d0, so that code was read from an
`objdump -d -M intel` listing (0x1017d871..0x1017dd19). Every D3D call goes straight through the device vtable
(`[[0x1045666c]+0xc]`). Offsets used:
- SetRenderState 0xC8, SetTextureStageState 0xFC, SetTexture 0xF4.
- SetMaterial 0xA8 (not 0xC4), SetLight via the wrapper 0x1000bf30, LightEnable 0xB8.
- Begin/End/Apply/CreateStateBlock 0xD0/0xD4/0xD8/0xE4.
- SetVertexShader 0x130, SetVertexShaderConstant 0x13C, SetStreamSource 0x14C, SetIndices 0x154, SetPixelShader 0x160.

### 6.1 Built piece record (0xC0 bytes, from `Mmb_DecodeAndBuild` 0x1016fbe0, disassembly 0x101700e4..0x101701ac) — CODE
The decoder writes these fields for each piece. The sub-mesh array uses 0x24-byte entries `{count, pieces*, bbox...}`
at mesh+0x60, with the count of sub-meshes at mesh+0x44.

| Off | Meaning |
|---|---|
| +0x04 | texture object* (null = untextured); the draw binds `tex+0x40`, or `tex+0x44` if that is null |
| +0x08 | nv; +0x0C vertex ptr; +0x10 ni; +0x14 index ptr; +0x18 primitive count |
| **+0x20** | i16 = file flag **bit31** |
| **+0x22** | i16 = **bit30** |
| **+0x24** | i16 = **bit29** |
| **+0x26** | i16 = **bit28** |
| +0x28 | VB (de-indexed, FVF 0x152); +0xB0/+0xB4/+0xB8 type-2 VB/IB; +0xBC bump texture (vertex type 3) |

### 6.2 Frame setup recorded once per zone pass (`ZoneMesh_RenderAll` 0x1017b420, state block at mgr+0x38d94) — CODE
| State | Value | Note |
|---|---|---|
| ALPHABLENDENABLE | 0 | per piece below |
| SRCBLEND / DESTBLEND | **SRCALPHA (5) / INVSRCALPHA (6)** | fixed. BLENDOP is never set (D3D8 default ADD) |
| ALPHATESTENABLE | 0 | per instance below |
| ALPHAFUNC / ALPHAREF | **GREATER (5) / 0x60** | |
| ZBIAS (47) | `g_ZBiasOpaque` = 0 | |
| LIGHTING | 1 | |
| SHADEMODE | GOURAUD | |
| FOGENABLE | 1 | |
| NORMALIZENORMALS | 0 | |
| TSS 0..2 ADDRESSU/V | WRAP | |
| TSS 0..7 TEXCOORDINDEX | = stage | |
| TSS 0..2 MAGFILTER / MINFILTER | LINEAR (2) / POINT (1) | as pushed; looks odd, but that is what the code does |
| MIPFILTER | LINEAR if `[0x10456a98]` else NONE | |
| **Stage 0 COLOROP** | **MODULATE2X** (5): ARG1 = TEXTURE, ARG2 = CURRENT (= lit diffuse) | |
| **Stage 0 ALPHAOP** | **MODULATE4X** (6): ARG1 = TEXTURE, ARG2 = CURRENT | |
| Stage 1 and 2 COLOROP / ALPHAOP | DISABLE | |
| Fog mode | if `mgr+0x3949c` == 1 (vertex-shader path): FOGVERTEXMODE = LINEAR, FOGTABLEMODE = NONE. Else if caps byte `+0x92d`: TABLEMODE = LINEAR, VERTEXMODE = NONE. Else if `+0x92e`: vertex LINEAR. RANGEFOGENABLE = caps `+0x92f` | |
| Fog values | FOGCOLOR / FOGSTART / FOGEND = `mgr+0x39254 / +0x3925c / +0x39258`, filled by the environment object `[0x10629fe4]` (0x1018b930) | |
| AMBIENT | `mgr+0x39250` (0x1018b8f0) | |
| Lights | 1–2 directional `D3DLIGHT8` (mgr+0x39180, +0x391e8) from the environment; light indices 5..7 are disabled | |
| Material | diffuse (1, 1, 1, 0), ambient (0.7, 0.7, 0.7, 0), specular/emissive 0, power 50 (init at 0x10179041) | |

Material colour sources are not set in the zone pass. The device preset `D3DW_ApplyLightingPreset` 0x10009920 sets
COLORVERTEX = 1, DIFFUSEMATERIALSOURCE = COLOR1, AMBIENTMATERIALSOURCE = COLOR1, SPECULARMATERIALSOURCE = COLOR2,
EMISSIVEMATERIALSOURCE = MATERIAL and SPECULARENABLE = 0 (CODE). That these are still in force during the zone pass is
INFERRED: nothing in 0x1017b420 / 0x1017c8d0 overrides them. The same function sets ZENABLE = 1 and ZFUNC = LESSEQUAL.

**What the colour pipeline computes (CODE + INFERRED from the D3D8 fixed-function definition):**
```
lit.rgb = clamp( vtx.rgb * (AMBIENT + sum_lights(light.diffuse * max(N·L, 0))) )   // material emissive = 0
lit.a   = vtx.a
out.rgb = clamp( 2 * tex.rgb * lit.rgb )        // MODULATE2X: vertex/lit colour 0x80 = 1.0
out.a   = clamp( 4 * tex.a   * vtx.a )          // MODULATE4X: tex 0x80 * vtx 0x80 * 4 = 1.0
```
DATA (zone 230, unique meshes): 81,448 of 95,822 vertices (85%) have alpha 0x80, and the RGB maximum is typically ≤ 0x80. Together
with texture alpha ≤ 0x88 (sec. 4), this matches the PS2-style "0x80 = 1.0" scaling on both inputs. So `min(a*2, 1)` in
sec. 4 is only right when vertex alpha is 0x80. A renderer should use `clamp(4 * texA * vtxA)`.

### 6.3 Per instance (0x1017c8d0, disassembly 0x1017d871..0x1017d984) — CODE
- `instance+0xd6` (set in `Mzb_FindMmbByNameInSet` 0x1017aef0 from the **first character of the placement/model name**):

  | Name starts with | `+0xd6` | ALPHATESTENABLE | TSS stage 0 ADDRESSU/V |
  |---|---|---|---|
  | `_` | 1 | 1 | WRAP |
  | `#` | 2 | 0 | **CLAMP** |
  | anything else | 0 | 0 | WRAP |

  So **alpha test is off for ordinary zone meshes**. DATA, zone 230: only `_plant_01..03` and `_flag` start with `_`.
  Zone 104 has 65 such names.
- The same function picks LOD variants: if the last non-space character of the name is `h`, `m` or `l`, it stores the
  h, m and l meshes in instance+0x18/0x14/0x10. All three start as the named mesh; `<stem>m` replaces all three,
  `<stem>h` the near one, `<stem>l` the far one. The record's +0x38 / +0x3C / +0x40 become instance +0xC8 = d1²,
  +0xCC = max(d1, d2)², +0xD0 = d3² (CODE 0x10179340). At draw time (0x1017c8d0) the squared distance from the camera to
  the placement's translation picks **h when ≤ d1², m when ≤ max(d1, d2)², else l** (a null mesh draws nothing); the
  same distance culls the placement beyond the global draw distance (mgr+0x39424), or beyond mgr+0x39428 × d3² when
  record +0x46 bit 0 is set. DATA: 524,767 of 1,049,859 placements (298 zones) name an h / m / l mesh, and 251,046
  name `..m` while a `..h` exists (West Ronfaure's trees `_ron_w10_m`: d 30 / 50 / 160). xi-client (2026-10-20):
  `ZoneScene` adds one instance per distinct mesh of such a placement in a `MeshRenderer` LOD group, and
  `updateLod(eye)` each frame sets the draws' instance counts for the band (`XI_NO_LOD=1` draws the named mesh,
  `XI_LOD_TRACE=1` logs the bands). The draw-distance cull is not done (fog hides the far geometry).
- `instance+0xd4` (mirror, `sx*sz < 0`): CULLMODE = CW (2) instead of CCW (3).
- `instance+0x0e` → NORMALIZENORMALS. `instance+0xe0` ≠ 0 → a second light and fog set (0x10189030, 0x10188bb0).
- World matrix: `D3DW_SetTransform(D3DTS_WORLD=0x100, instance+0x40)`.

### 6.4 Per piece (0x1017d9e1..0x1017dcdb) — CODE
States are cached in mgr+0x39114 (blend) and mgr+0x39120 (cull), so a call happens only when the value changes.

| bit31 (+0x20) | bit29 (+0x24) | ALPHABLENDENABLE | ZWRITEENABLE | ZBIAS | CULLMODE |
|---|---|---|---|---|---|
| 0 | 0 | 0 | 1 | `g_ZBiasOpaque` = 0 | CCW (3), or CW if mirrored |
| 0 | 1 | 0 | 1 | 0 | **NONE** |
| 1 | 0 | **1** (SRCALPHA, INVSRCALPHA) | **0** | `g_ZBiasBlend` = **8** (1 if caps byte `[0x1045666c]+0x920`) | CCW / CW |
| 1 | 1 | 1 | 0 | 8 | NONE |

- bit30 (+0x22): no reader found in the zone draw paths or anywhere else in 0x1016xxxx..0x1018xxxx.
- bit28 (+0x26): not read by the main zone pass. It is read only by `ZoneMesh_DrawAttachedInstance` 0x10183cb0, which
  draws instance lists hanging off an Mzb (0x1ac-byte entries) after the main pass. There, if bit28 is set and
  `mgr+0x50` is non-null, the piece is lit with a different light, ambient and fog set (mgr+0x39340/0x39410,
  0x10188d10). INFERRED: this is an "interior/special lighting" marker. In that pass ALPHABLENDENABLE is set straight
  from +0x20, without the Z-write change.
- Texture: `SetTexture(0, tex+0x40 ?: tex+0x44)`. No texture flag, alpha kind or format is read, so **render state
  does not depend on the texture**. Vertex type 3 (bump) also binds `piece+0xBC` to stage 1.
- Vertex type selects the shader (jump table 0x1017dd1c):
  - types 0 and 1: FVF 0x152 with PS = 0 (fixed function);
  - type 2: vertex shader `mgr+0x39454`, a morph between the position streams;
  - type 3: vertex shader `mgr+0x39480[i]` plus pixel shader `mgr+0x39494` (bump).
- Draw: types 0, 1 and 3 use `D3DW_DrawPrimitive(primType = mesh+0x2c, 0, piece+0x18)` on the de-indexed VB. Type 2
  uses `D3DW_DrawIndexedPrimitive`.
- `mgr+0x3949c == 2` selects the software path `ZoneMesh_DrawInstanceSW` 0x1017dd30, which transforms on the CPU but
  makes the same per-piece blend, Z-write and cull decisions.

### 6.5 Draw order — CODE
- Inside an instance, pieces are drawn in file order. **Blended pieces are not deferred or sorted.** They draw in
  place, with Z-write off and Z-bias +8.
- Instances: the Mzb (visibility list or full list) is walked in order. An instance whose `instance+0x0f` is set
  (= Mzb record byte **+0x46 bit 2**; ev_9348c0...) is appended to a deferred list (max 512). After the other
  instances of that Mzb, the deferred list is drawn in append order, **unsorted**. DATA: no placement in zones
  230/104/245 has that bit, so in practice zone transparency is order-dependent and relies on the Z-bias.
- Instances with `instance+0x08` ≤ 1 are skipped. `Mzb_DecodeAndBuild` initialises it to 2.
- After each Mzb: blend is reset to off, Z-write to on and ZBIAS to opaque, then `ZoneMesh_DrawAttachedList` runs.

### 6.6 Worked cases, zone 230 (file 330) — CODE + DATA
- **`model   siba0` (lawn) without bit31** (17 pieces, every vertex alpha = 0x80): blend off, and alpha test off because
  names like `lef_04` / `mid_02` do not start with `_`. **Texture alpha is ignored**, so the lawn renders as fully
  opaque green, `2 * tex.rgb * lit`. The 92% zero-alpha texels have no effect.
- **`model   siba0` with bit31** (14 pieces in the same models; vertex alpha 0x7E on 1034 vertices, 0x3F on 94, 0x80 on
  91 and 0 on 12): drawn blended with Z-write off and Z-bias 8, using `alpha = clamp(4 * tex.a * vtx.a)`. The 92%
  zero-alpha texels are fully transparent. The rest (tex.a ≈ 0x80) give about 0.98, or about 0.5 where vertex alpha
  is 0x3F. Result: only the non-zero-alpha texels (grass tufts or edges) show over what is already drawn. INFERRED:
  these are fringe/overlay strips that blend the lawn edge into neighbouring surfaces.
- **`funsui  umi02` (water, always bit31)**: 2 pieces. Vertex alpha varies (0 on 57 of 84 vertices, then values from
  10 to 255). Drawn blended with Z-write off and Z-bias 8. Final alpha = `clamp(4 * tex.a * vtx.a)`, so the water
  fades to invisible toward the alpha-0 vertices and saturates where `vtx.a * tex.a` ≥ 1/4. It still writes no depth.
- Other alpha-tested content in this zone is limited to the `_plant_*` / `_flag` models: GREATER 0x60 on the
  *combined* alpha. With vertex alpha 0x80 this means texel alpha > about 0x30.

### 6.7 Confidence and open items
- High (CODE): the per-piece blend, Z-write, Z-bias and cull table; the stage 0 MODULATE2X / MODULATE4X setup; the
  SRCALPHA/INVSRCALPHA blend; ALPHAFUNC GREATER with ALPHAREF 0x60; alpha test only for `_` names; no sorting.
- Medium (INFERRED): the vertex colour as ambient and diffuse material source (set by the device preset, not
  re-checked at the time the zone is drawn); MIN = POINT filtering (verify on a live device if visuals differ).
- Open: the meaning of bit30 (no reader found); the exact environment light, fog and ambient values
  (0x1018b8b0 / 0x1018b8f0 / 0x1018b930); what 0x1017a300 does for meshes with mesh+0x68 set (not D3D state; maybe
  colour animation); and how the vertex shaders for types 2 and 3 light the mesh.

## 7. Collision mesh (Mzb collision block)

Session 3 (2026-10-16), same binary and Ghidra session. This supersedes the collision note in 5.4. Names applied this
session (ev_4c7ea8cc...): `Mzb_MergeCollisionSecondary` 0x10167af0, `Coll_UnlinkSecondaryLists` 0x101678b0,
`Coll_LinkDynamicLists` 0x101679e0, `Coll_SegmentQuery_Nearest` 0x101692c0, `Coll_SegmentQuery_SkipBarriers` 0x1016b0e0,
`Coll_PointInTriangle` 0x1016c2a0, `Coll_GetLastHitInfo` 0x10168e50. Three more segment queries have the same shape and
are not named yet: 0x10169a50, 0x1016a1e0 (2 arguments), 0x1016a8a0.

Evidence: `Mzb_FixupCollision` pseudocode ev_bad7ab81...; `Mzb_DecodeAndBuild` pseudocode ev_9348c09e... and disassembly
ev_d4629ded...; batch decompile of 0x10167430..0x1016bd00 ev_c14029cd...; `Coll_SegmentQuery_Nearest` disassembly
ev_fdaa3782...; constants 0x10329ce4 = 0.25f (ev_4c0c429a...) and 0x1032a3f0 = -1.0f (ev_775874688...);
`Coll_PointInTriangle` ev_81751128....

Reference implementation: `tools/zone_collision.py` (OBJ export, ground-height query, stats and self-checks).
All offsets are relative to the de-obfuscated Mzb payload (5.4). The file stores offsets; the client turns them into
pointers in place by adding the payload base (`Mzb_FixupCollision`).

### 7.1 Entry and Mzb header fields used (CODE: 0x10179340 disassembly 0x101793f3..; DATA)
- `Mzb_DecodeAndBuild` builds a zone record `R` (0x78 bytes at mgr+0x60): `R[0] = p[0x1C] + 1` (disassembly
  `mov al,[edi+0x1c]; inc eax; mov [esi],eax`), `R[2] = payload`, `R[4] = p[0x0C]`, `R[5..7] = p[0x0D..0x0F]`.
- If `u32 p[0x08] != 0`, `R[3] = payload + p[0x08]` and, for the **first** Mzb with collision,
  `Mzb_FixupCollision(R, remapTable 0x106299b8, version=p[3], flags=p[0x1D])`. A later Mzb with collision (secondary,
  for example a moving object) goes to `Mzb_MergeCollisionSecondary` 0x10167af0 instead, which links its lists into the
  existing grid (DATA: no zone in 0..299 / 600..699 has a second Mzb with collision).
- `p[0x1D]` bits (stored as booleans at this+0x34138..0x3413b): bit0, bit2, **bit3 = search radius 2 cells instead of
  1** (read by 0x101692c0/0x1016a8a0), bit4. DATA: 0 in zone 230.
- Grid present only when `R[0] == 1`, that is `p[0x1C] == 0` (CODE). DATA: true for every zone checked.

Grid geometry (CODE, `Mzb_FixupCollision`):
```
A = p[0x0C], B = p[0x0D], C = p[0x0E], D = p[0x0F]
gridW  = A*C / 4            (this+0x48, columns along x)
gridH  = B*D / 4            (this+0x4C, rows along z)
halfX  = A*C * 0.5          (this+0x40, float)
halfZ  = B*D * 0.5          (this+0x44, float)
```
DATA: C = D = 40 in 316 of 320 zones (20 in two, 80 in two), so the grid usually spans A*40 by B*40 world units centred on the origin.
Zone 230: 40 40 40 40 → 400 × 400 cells, x and z in [-800, 800).

### 7.2 Collision block header at `coll = p[0x08]` — 7 × u32 (CODE: `puVar16[0..6]`; DATA)

| Off | Field | Zone 230 |
|---|---|---|
| +0x00 | mesh count | 4278 |
| +0x04 | offset of first mesh | 0x55fd8 (= coll + 0x20) |
| +0x08 | cell-list count | 3313 |
| +0x0C | offset of first cell list | 0x218e24 |
| +0x10 | offset of the grid (only used if `p[0x1C] == 0`) | 0x22d8dc |
| +0x14 | offset of first placement record | 0x1ca5e4 |
| +0x18 | placement count | 1675 |
| +0x1C | (pad; zero) | 0 |

DATA: the regions are contiguous in this order: header, meshes, placements, cell lists, grid; the grid ends 0x14 bytes
before the payload end (zone 230). Checked in 320 zones (7.8).

### 7.3 Mesh (CODE: fixup loop in 0x10167e60; DATA)
```
+0x00 u32 vertOff   → float3[nv]   local-space positions
+0x04 u32 normOff   → float3[nn]   local-space unit normals (one per triangle, shared)
+0x08 u32 triOff    → Tri[nTri]
+0x0C i16 nTri
+0x0E u16 barrierFlag   nonzero ⇒ some triangles may carry the barrier bit (CODE: 0x1016b0e0 tests it; see 7.5)
```
`nv = (normOff - vertOff) / 12`, `nn = (triOff - normOff) / 12` (DATA: always exact; header is always followed
directly by the vertex array, `vertOff = mesh + 0x10`). The next mesh starts at `triOff + nTri*8` (CODE: `piVar30 =
piVar30[2] + (short)piVar30[3]*8`). DATA zone 230: barrierFlag is 1 in 809 meshes; all 337 meshes that contain
barrier triangles have it set (472 flagged meshes have none).

Triangle, 4 × u16 (CODE: masks in 0x10167e60 and every query; DATA):

| Word | Bits 0..13 | Bit 14 | Bit 15 |
|---|---|---|---|
| w0 | vertex a (with bit 14: `& 0x7FFF`) | part of index | material bit 0 |
| w1 | vertex b (`& 0x3FFF`) | **runtime only**: set by the fixup when the triangle has a zero-length edge (never set in files, DATA) | material bit 1 |
| w2 | vertex c (`& 0x3FFF`) | **barrier** (transparent wall) | material bit 2 |
| w3 | normal index (with bit 14: `& 0x7FFF`) | part of index | material bit 3 |

- `material = w0>>15 | (w1>>15)<<1 | (w2>>15)<<2 | (w3>>15)<<3` — CODE: `Coll_SegmentQuery_Nearest` 0x101698e4..0x1016990e
  stores exactly this at this+0x34148, and `Coll_GetLastHitInfo` returns it as the hit's first field. INFERRED from LSB's
  extracted format (`TriangleMeta.material`, `TerrainType`): 0 object, 1 path, 2 grass, 3 sand, 4 snow, 5 stone, 6 metal,
  7 wood, 8 shallow water, 9 deep water. DATA zone 230: {0: 10370, 2: 1316, 5: 62851, 6: 1066, 7: 30827, 8: 16, 9: 16}
  world triangles (San d'Oria: stone and wood).
- Barrier bit: CODE `Coll_SegmentQuery_SkipBarriers` 0x1016b0e0 — when the mesh's barrierFlag is nonzero it skips
  triangles with `w2 & 0x4000` (`test byte [tri+5], 0x40`); `Coll_SegmentQuery_Nearest` does not. Meaning INFERRED from
  LSB (`barrier`, `IgnoreTransparentBarriers`). DATA zone 230: 948 world triangles, 924 of them walls, all material 0.
- The fixup also clamps bad normals: a component equal to 0xFFC00000 (NaN) becomes 0, and an all-zero normal becomes
  (0, 1e-6, 0) (CODE).
- Winding (DATA, zone 230, 106,462 triangles): in local space `(b-a)×(c-a)` is **opposite** to the stored normal for
  100% of triangles, the same clockwise-front convention as the render meshes (5.5). In world space this holds except
  for mirrored placements (determinant < 0: 9,247 triangles), where it flips.
- Normal direction: stored normals point out of the solid (floors have (0, -1, 0), i.e. up in Y-down space) — DATA.
  The queries negate the normal for the plane test (`fmul -1.0` at 0x10169619) and `Coll_GetLastHitInfo` negates it
  back, so the reported hit normal is the stored normal in world space (CODE).

### 7.4 Placement record, 0xC0 bytes (0x80 if version < 0x1A) (CODE: 0x10167e60 stride `this+0x70`, query field reads)

| Off | Type | Meaning | Basis |
|---|---|---|---|
| 0x00 | f32[16] | **object→world** 4×4, D3D row-vector: `world = [x y z 1] · M`, translation in 0x30..0x38 | CODE (hit point transformed with +0x00/+0x10/+0x20/+0x30 at 0x1016978b) + DATA |
| 0x40 | f32[16] | **world→object** 4×4 (inverse of 0x00) | CODE (segment endpoints transformed with +0x40..+0x78 at 0x10169503) + DATA (max error 5e-5) |
| 0x80 | f32[9] | normal matrix, row-vector 3×3: `n_world = n · N` | CODE (0x10169856..0x101698de) |
| 0xA4 | u32 | flags (below) | CODE |
| 0xA8 | u32 | offset of per-instance data (0 = none), relocated by the fixup; returned with the hit at this+0x1807c. For secondary Mzbs overwritten with an object handle | CODE; target not decoded |
| 0xAC | u8[4] | four 1-based indices remapped through the table at 0x106299b8 by the fixup (sets flag bits 28..31 once done) and returned with the hit | CODE; meaning unknown |
| 0xB0 | u32 | returned with the hit (this+0x34158) | CODE; DATA mostly 0 |
| 0xB4 | f32 | world **y min** of the instance (conservative) | CODE (segment rejected if its y range misses [0xB4, 0xB8]) + DATA |
| 0xB8 | f32 | world **y max** | CODE + DATA (zones 230/104/245: [0xB4, 0xB8] always contains every vertex of the instance's meshes; the max is exact for ~98%, the bounds are sometimes padded by several units) |
| 0xBC | u32 | owner id; a query skips the placement when it equals this+0x04 (ignore self) | CODE; DATA 0 except a few |

- DATA: 0x80 is the row-normalised rotation part of 0x00 (`M` rows / |row|, equal within 1e-3) for every placement that
  a cell references; placements no cell references (e.g. 150 of 1675 in zone 230, 1464 of 4695 in zone 104) may have an
  all-zero 0x80 block. The matrices are **final world transforms**; no other per-zone offset applies.
- A placement can own several meshes (DATA: 7,270 unique (placement, mesh) pairs, 1,390 placements, 4,141 meshes used in
  zone 230). Collision placements are a separate table from the 100-byte render placements of 5.4, even though the
  count (1675) happens to match in zone 230.
- Flags at 0xA4 (CODE: 0x10169914..0x10169951 → `Coll_GetLastHitInfo`):
  - bit 2 → hit field +0x18 (INFERRED from LSB: `roofed`). DATA zone 230: set on 232 placements.
  - bits 3..5 (low) and 26..27 (high) → 5-bit **map/floor id** `(f>>26&3)<<3 | (f>>3&7)` (LSB: `mapId`).
  - bits 6..25 → signed 20-bit value `(int)(f<<6)>>12`, returned multiplied by 1/256 (0.00390625) (CODE; meaning unknown).
  - bits 0, 1: not read by the queries (DATA: bit1 set on 1,640/1,675). Bits 28..31: runtime "already remapped" marks for
    0xAC..0xAF.

### 7.5 Cell lists and grid (CODE: 0x10167e60, 0x101678b0, 0x101679e0, 0x101692c0; DATA)
Cell list:
```
u32 hdr                    bits 0..10 = n entries; bit 11 = dynamic list (runtime); bit 12 = secondary-Mzb list (runtime);
                           bit 13 = list disabled, skipped by every query; bits 14..22 = cell column, bits 23..31 = cell row
{ u32 placementOff, u32 meshOff } [n]    both relocated; 0 is reported as an error
u32 next                   0 in files; the client chains extra lists (dynamic objects, secondary Mzbs) through it
```
DATA: the packed row/column equal the cell's position for every cell with column < 512; above that the 9-bit column
wraps and half the time carries into the row field, so readers should rely on the grid, not these bits. Every list is
referenced by exactly one cell; `next` is always 0.

Grid: `gridH` rows × `gridW` u32 offsets, row-major, 0 = empty cell. World to cell (CODE: 0x10169317..0x1016935a):
```
col = ftol((x + halfX) * 0.25)      reject unless 0 <= col < gridW
row = ftol((z + halfZ) * 0.25)      reject unless 0 <= row < gridH
cell = grid[row * gridW + col]
```
Cells are 4 × 4 world units. Queries visit the 3×3 block around the start cell (5×5 if `p[0x1D]` bit 3), clamped to the
grid.

### 7.6 How the client queries it (CODE: `Coll_SegmentQuery_Nearest` 0x101692c0)
```
query(P0, P1):                      // world-space segment; returns nearest hit to P0
  for each cell in the 3x3 (or 5x5) block around cell(P0):
    for list in chain(cell):
      if list.hdr & 0x2000: continue
      for (pl, mesh) in list:
        if not (pl.ymin <= max(P0.y,P1.y) and min(P0.y,P1.y) <= pl.ymax): continue
        if pl.owner == self.id: continue
        a = P0 · pl.inv ; d = P1 · pl.inv - a                  // object space
        for tri in mesh:
          n = -normal[tri.w3 & 0x7FFF]
          den = n·d ; if den == 0: continue
          t = -(n·a - n·v[tri.a]) / den ; if not (0 <= t < 1): continue
          h = a + t*d ; if not Coll_PointInTriangle(h, tri): continue   // edge tests with 1e-4 tolerance
          w = h · pl.m ; keep if |P0 - w|^2 is the smallest so far
  hit info: point w, normal = stored normal · pl.nrm, material (7.3), flags fields (7.4)
```
Ground height = the same query with a vertical segment. The client clips the segment endpoints to cells only by P0;
the 3×3 neighbourhood covers short segments.

### 7.7 Coordinate conventions
Same as 5.5: game space is **Y-down** (DATA: the floor under the spawn has normal (0, -1, 0) at y = -2.0 and the roof
above it is at y = -15.4 in the render mesh), left-handed, and collision world coordinates are server (LSB) coordinates.
`tools/zone_collision.py --obj` writes (x, -y, -z), a rotation, so it is not mirrored (see §5.5).

### 7.8 Validation (DATA)
Zone 230 (file 330), `zone_collision.py 230 --height ...`:
- (-0.63, 54.37): one surface, **y = -2.000**, material 5, normal (0, -1, 0) — matches the LSB spawn y.
- (113.458, -57.35): floor **y = 0.000** (gatehouse). (0.013, 57.965): floor **y = -2.000** (north passage). These are
  the LSB zone-line `from` points, whose y values (-4.079, -8.469) are trigger-box centres, not floor heights: the render
  mesh (tools/zone_to_obj.py) has floor 0.0 / ceiling -8.0 at the first point and floor -2.0 / ceiling -15.4 at the
  second, so both y values sit between floor and ceiling. The collision mesh and the render mesh agree on the floors.
- 106,462 world triangles (102,354 vertices): **63,414 walls** (|n.y| < 0.2), 24,138 floors (n.y ≤ -0.2), 18,910
  downward-facing, 948 barrier triangles, 0 degenerate.
- Sweep of zones 0..299 and 600..699 (400 ids): 77 have no model DAT/Mzb, 2 have no collision block (601, 602),
  **320 parse cleanly** (every vertex/normal index in range, regions contiguous as in 7.2, every list entry resolves to a
  mesh and a placement, every grid offset is a list). The remaining one, zone 0, uses Mzb version 0x17 whose placement
  records are 0x70 bytes apart in the data (client stride for < 0x1A is 0x80); not decoded. The requested set
  100-105 and 230-241: **18/18** clean. Grid dims seen: 40·40·40·40 (70 zones), 20·20·40·40 (27), 2·2 (18), 50·50 (18), ...

### 7.8b The player's movement (CODE 0x100a4e00 -> 0x10181920, 2026-10-18; RE notes collision_re.md)

- **Shape:** a sphere of radius 0.5 centred 0.5 above the feet, swept along the move (sub-steps of at most 4, advanced
  0.15 at a time, CODE 0x1016e6f0 / 0x1016e840 / 0x1016eff0); on contact it is pushed out along the contact (a slide),
  up to 3 times, else the move is refused. Degenerate triangles are skipped; **barrier triangles block the player**
  (only the camera's ray skips them, CODE 0x1016b0e0). Material plays no part in blocking.
- **Steps and slopes:** no explicit step height or slope limit; steps are climbed by the sphere riding over the edge
  (about its radius, 0.5), then the ground probe snaps the feet.
- **Ground and falling:** the probe looks from 0.5 above the feet to 40 below (CODE 0x10169a50); no ground -> stay at
  the old position. Walking off a ledge falls: the fall speed grows 0.0408 per 1/60 s tick, at most 1 per tick.
- **Camera:** an eye point that trails a look-at point (below, "Camera follow"); its rays skip barriers. On /lockon a
  ray from 1 above the feet to the eye puts it 0.2 in front of a hit; otherwise the view line from the look-at puts
  it 0.5 in front, and a last look from the look-at 0.2.
- **What blocks (CODE 0x1016e840):** only the collision grid (the resolver's cell lists at `this+0x60`, built from the
  Mzb collision placements; placements are culled by their y range +0xB4 / +0xB8 against the move's +-0.5, the actor's
  own placement +0xBC skipped, two passes by placement flag 0xA4 bit 1, triangles with mesh-entry byte 3 bit 6 skipped).
  Render-model props (MMB placements without collision triangles) are never visited, and neither are NPCs or other
  actors; the only other solids are, **in the Mog House only** (DAT_106306b4, and FUN_101d5b90 false, INFERRED: not
  while arranging furniture), `XiModelActor` furniture whose position is within 4 of the player and not below it by more
  than 0.1, tested as boxes of the actor's size (+0x764 / +0x768 / +0x76c, centred, CODE 0x1016d490). So a prop with no
  collision triangles (Desuetia's rock) is walked through in the game too.
- ffxi-linux (`gfx::ZoneCollision::sweepSphere`, CPU-only): the game's swept sphere with the step-up kept at 0.75
  (Davoi / Bastok Mines risers). The sphere (r 0.5) is centred max(0.5, step-up) above the feet and meets wall triangles
  clipped to the band from the step-up to 1.0 over the feet (lower parts are steps the floor snap climbs; lintels and
  arches above 1.0 do not block, barriers do). Sub-steps of at most 4, marched 0.3 r = 0.15 to the first contact, then
  pushed out sideways along the contacts (the closest point of each clipped triangle) in up to 8 passes (the game's 3
  settle too slowly with a sideways-only push: zone 234 walkcheck 98,937 steps moved with 3, 107,888 with 8, the slices
  107,540), else the last free position. A wall the move starts inside (it entered the band as the feet went down a
  step or along a slanted cave wall) asks only the start's distance + 0.05, so the character eases out instead of
  being thrown through what is behind; a final swept test refuses a path that crosses a wall above the step-up. Floors
  at most 0.75 up; falls with the game's gravity. `XI_MOVE_OLD=1` gives the earlier wall slices (0.8, 0.85, 1.0, circle
  pushed out). Checked with `xi-dat reach ZONE 0.75 o0.75` (o = slices): reachable ground at 0.75 is within 0.15% of
  the slices (234: 29,114 vs 29,098; 100: 496,756 vs 497,349; 102: 490,258 vs 490,983; 104: 473,698 vs 473,675; 230:
  38,277 vs 38,336; 149: 107,368 vs 106,328; 196: 80,663 vs 80,582). The cells only the slices reach are wall-hugging
  slivers and drop-downs reached through single grid points: the flood fill is path dependent (slices 0.751 vs 0.75
  already differ by 1,872 cells in zone 102, 1,006 in 100), and the paths into the regions checked (zone 102 game (60,
  260), 100 (-190, 400)) replay move for move with the sphere (`XI_REACH_LIST=1`, `XI_REACH_TRACE=x,z`; `xi-dat
  movetest ZONE X Z TOX TOZ [MAXY]` walks both resolvers). walkcheck pass-throughs over 234, 100, 102, 104, 230, 149,
  196: 4 (slices 3). **Seams** (found live, 2026-10-19): the floor triangles leave slivers (~0.05 wide beside
  Gusgen's rail sleepers) that only steep faces cover; a short step landing in one found no floor and was refused, so
  at 90 fps and up (steps under ~0.055) the character stopped dead where 60 fps steps jumped the gap (the tunnel at game
  (20.6, -235.3) and others; the slices too). The game's probe is a vertical segment against every triangle, so it
  never sees them. move() now stands on an upward-facing, non-barrier steep face within [feet - 0.5, feet + step-up]
  where no floor triangle covers the spot, when a floor lies 0.3 further along the move (`seamHeight` /
  `floorAhead`). Accepting any steep face (no look-ahead), or every descending one, let characters perch on hillsides
  and cost ~4% of reach outdoors (zone 100: 476,033); with the look-ahead reach is unchanged (100: 496,436, 102:
  490,213, 104: 473,713, 234: 29,254, 230: 38,277, 149: 106,270, 196: 80,864) and walkcheck counts no falls
  (seam-aware). Its pass-throughs rise to 11 (234: 1, 102: 2, 149: 2, 196: 6): five are steps off a cliff lip
  reached on the face (drops of 2.4-4.7; the game has no drop limit), one a 0.5 drop under an overhang. Test "ground
  probe: a seam covered only by a steep face is not a hole"; `xi-dat movetest` takes `XI_MOVETEST_FPS=N` (and
  `XI_MOVETEST_STEPS=1`). **Drawn position vs physics position** (2026-10-20): the floor snap still climbs a riser in
  one move, but the game draws actors at a second position, actor +0x5fc (vfunc +0x1bc, 0x100a4670), which the actor
  update (0x100c6300) moves toward the physics position +0x5c4 (gravity 0x100a4e00 and the movement write
  it) by 1/8 of the gap per 1/60 s tick, all three axes (0x100c6506..0x100c6586: the loop over 0x10014cb0's ticks,
  constant 0.125 at 0x1032a3bc); the last tick's step is the motion vector +0x6a8 (cloth inertia). Under the actor
  flags +0x101..+0x104 or vfunc +0x19c it takes the whole gap at once (0x100c659c), and with a stationary status and
  +0x768 == 0 it does not move (step = 0x1035d720, zero). ffxi-linux draws our own model, its shadow, rod, label and
  the game camera's look-at at `drawFeet`, eased the same way; a jump over 3 (warp, zone-in) and a lift ride draw at
  the feet (INFERRED: which of the flags the game sets then is untraced); `XI_EASE_OLD=1` draws at the feet,
  `XI_STAIRS_TRACE=1` logs feet and drawn y per frame. Southern San d'Oria's plaza steps (game (17.5, -10.5) to
  (17.5, 28), 7 risers of 0.33-0.43 up and back): the largest one-frame height change went from 0.433 to 0.060 and
  frames moving over 0.05 from 14 to 3. Other actors walk between server positions (MoveTrack, linear in 3D) and were
  already smooth: BotC climbing the same steps as seen by TestA, server y jumps up to 1.0 between updates, the
  drawn height at most 0.017 a frame. The camera ray skips barriers. Camera (CODE 0x1001ed90): the ray runs from 1 above the feet to the eye, which
  ends 0.2 short of a hit; then the eye keeps 0.17 above the ground below it (probe 10 down, 0x10181120), 0.17 below a
  ceiling (probe 10 up, SkipBarriers, 0x10181280) and 0.17 from walls to its left and right (`ZoneCollision::
  cameraDistance` / `cameraClearance`; `xi-dat camcheck ZONE [N]` measures close-up views). In the earlier orbit camera (`XI_CAM_ORBIT=1`, the 1.5 target, 8 back) the eye stays on the
  view line from the 1.5 target, not on the ray, so after the ray it is pulled in (to 0.1 if need be) until the target
  sees it: camcheck's "cut off" count (an eye behind a surface seen from both the target and 1 above the feet; eyes in
  open air under bridges and in caves no longer count) fell from 14 / 6 / 9 / 88 / 71 / 251 / 274 to 1 / 0 / 1 / 0 / 2
  / 0 / 0 per 24,000 views in zones 100 / 102 / 104 / 106 / 116 / 149 / 196. That pull-in is ours (the game has
  none), and next to a pillar it can leave the eye ~0.3 from the target, inside our own model: xi-client fades our
  model out between 1.6 and 0.6 from the target (INFERRED: no such fade found in the game; seen live, West Ronfaure
  game (-275.8, 96.2), `xi-dat camat ZONE X Z` lists the camera distance by heading).
- **Camera follow** (2026-10-20, read again from the unpacked DLL; the earlier reading of
  this loop had the 6 rule backwards). The camera object (0x10015210) holds an eye (+0x44) and a look-at (+0x50); the
  view is LookAtRH from the eye to the look-at (0x10021b40). It is not an orbit at a fixed distance. Each frame
  0x100a5d40 runs the third-person tick 0x1001ed90, or with first person (DAT_10487f80) 0x10021040. ffxi-linux:
  `client::GameCamera` (`XI_CAM_ORBIT=1` gives the earlier orbit).
  - **Placed** (0x100215b0, at zone-in and warps): the eye at (-4, -2, 0) in the actor's frame (4 behind, 2 up), the
    look-at 0.5 above the feet.
  - **Look-at** (0x1001f236..0x1001f320): 0.6 x the model's height above the feet (the skeleton bounds, vfunc 0x3cc /
    0x3c8; mounted the bounds are capped at 2.5 or the mount's height added). It closes a quarter of the gap a tick, so
    the view trails the character's moves a little.
  - **Camera keys** (axes 6 / 7, inversion cfg 0x8b / 0x8c): the eye turns about the look-at 0.027924 rad a tick
    (0x1001eae0, level distance kept), and rises or sinks 0.10667 a tick (it moves the eye's height, not a pitch angle).
    Any input holds the follow rules' "left alone" state for 10 ticks (DAT_10456d74).
  - **Follow, per tick** with d the eye to look-at distance (0x1001fc2f..0x1001fda3): d > 100 pulls the eye in by
    (d - 100) x 0.5; 6 <= d <= 100 by (d - 6) x 0.012 of the eye-to-look-at vector; under 6 nothing. So the eye keeps
    its place while the character comes toward it, trails at about 6.9 while it runs away at 5 a second, and settles
    at 6. Under 3 (and when the level distance is under 3) it is pushed back out by (3 - d) x 0.125, the second push
    level; with "locked behind" on (cfg 0x91) only when a line 3.1 out from the look-at is clear (0x1001f8a6).
  - **Height kept** (0x1001fdc0): left alone, an eye above the look-at is put back to the height over it that it had
    at the end of the last frame (DAT_10456d78), so the pull does not lower it; walking downhill the view steepens, and
    the level push keeps the eye at least 3 out, about 60 degrees down at 6.
  - **Locked behind** (cfg 0x91 = Config 145, 0x1001f9d0..0x1001fc2a): the eye swings 2.5 % a tick (int 50 at
    0x1035122c x 0.01 x 0.05) toward the point its distance behind the character, unless the view already runs along
    the facing (dot >= 0.99) or the eye is at the front (<= -0.5). After camera input the swing waits until the
    character is 0.5 from where it was, and 4 ticks more (DAT_10456d8c / d90).
  - **View line** (0x1001fe0a..): a ray from the look-at to the eye (0x101814c0 -> 0x1016b0e0, two-sided, barriers
    skipped, Mog House furniture boxes) puts the eye 0.5 in front of a hit. Left alone (no input for 10 ticks) with
    four or more look-at positions recorded, it instead walks the eye 5 % a tick back to where the look-at was four
    records ago (records 0.1 apart while the line is clear, 0x1001e250 / 0x1001e2e0; DAT_10456d70), until within
    0.05, or the trail is over 10 from the look-at, or camera input.
  - **After** (0x10020760..0x10020a59): when the eye moved this frame it keeps 0.17 off the ground, a ceiling and
    side walls (`ZoneCollision::cameraClearance`), and once settled a last ray from the look-at puts it 0.2 in front
    of a hit.
  - **Lifts** (actor +0x5e2, set by the lift check 0x10181470 in 0x100a4e00): the eye rises and sinks with the feet.
  - **Home** (action 0x51, Home key, 0x1001ff2a..): for 8 frames the eye closes 1/8 a tick on the point 3 behind the
    character at its point 2 height, while the view is 0.05 rad or more off the facing (here 16 ticks).
  - **/lockon** (actor flag +0xf9 cleared by 0x100c5370, set again by 0x100c5340 or when the target goes): the look-at
    is half way between our point 13 and the target's aim point (its point 7 height, DAT_10456da8, 0x100d65a0); input
    turns the eye scaled by 6 / d; the eye is kept within atan(4 / (near + sep)) of the line from the target through
    us (x3 engaged) and near + sep / 2 .. far + sep / 2 from the look-at, near / far = (7.2 / 8.6 + sep / 8 + size) x
    zoom / 1000 (size: the target's bounds x 1.5 when engaged); a ray from 1 above our feet keeps it 0.2 in front of a
    hit. Leaving the lock, the follow rules wait 20 ticks (DAT_10456d7c).
  - **First person** (Change View, action 0x10, toggles DAT_10487f80; 0x100a5ec0): the zoom is 280 (third person
    restores config 0x49). 0x10020ad0: the eye 0.5 behind the feet at 0.91 of the head point's (12) height, looking 5
    ahead and 0.25 down along the facing; the camera keys only look aside, toward ±1.885 rad and ±1.414 rad, a quarter
    of the gap a frame, and the view comes back when let go (cfg 0x8d / 0x8e invert). 0x10021040 eases the camera
    there 1/8 a tick and holds the follow rules 8 ticks. Movement (0x100a6170): left / right turn the character 2
    degrees a tick at full stick, forward / back walk along the facing.
  - **Zoom** (0x1001f742..0x1001f898): Zoom In / Out 6 a tick between 242 and 900; both held ease it back to 350 a
    quarter of the gap a frame (DAT_10456d84); a wheel notch (WHEEL_DELTA 120, 0x10001dd8) adds 2 to a count spent one
    a frame at 6 a tick (0x1025dd70 / 0x1025dda0), dropped at a limit; saved as config 0x49. Not in first person.
  - **Ours, not the game's** (each marked in `game_camera.cpp`): the push out toward 3 is kept only when the view line
    to where it put the eye is clear; straight over the look-at the level push goes behind the character; and an eye
    whose distance turns back and forth three times in half a second while nothing moves is held. The game lets the
    view line take a pushed eye back in, which against slopes and pillars bounced every frame or two: `xi-dat camcheck
    ZONE N` runs the camera N / 4 times (placed behind, the character runs 4 and stands 3 s); bouncing runs per 500
    before / after: zone 100 11 / 8 (318 / 35 frames), 102 6 / 3 (62 / 26), 149 3 / 2 (135 / 14), 230 0 / 0, 104 7
    (48 frames); eyes nearer than 1 at the end 27 -> 2 (100), 29 -> 3 (102). `xi-dat camsim ZONE GX GZ [TOX TOZ
    [SECONDS]]` prints one run frame by frame (`XI_CAMSIM_ALL=1`). The game's mouse camera is the window-edge push only
    (0x10124b40..0x10124d4c: ±1 on the keys' axes 6 / 7); the right-button drag (0.004 rad a pixel) is ours (goal 28).
    The look-at height (0x1001f2c2..0x1001f36f): feet - 0.6 h, h = the Sk2 bounds set 0's height (riding, state 5 /
    0x55: set 1, plus the mount's `info` byte 14 / 100, 0x100222e0); an effect's op 0xBD point (+0x8a9) replaces it;
    sitting has no case of its own (goal 28).
- **Projection** (CODE 0x10015450, defaults 0x10015390): D3DXMatrixPerspectiveFovRH (0x102d7f26) with fovY = 2 ·
  atan2(192, zoom), zoom 350 normally (57.5 degrees; an event shot's zoom replaces it, CODE 0x10015270, and the
  Zoom In / Out move it 6 a tick while held, between 242 and 900, 0x1001f742..0x1001f875; the mouse wheel moves it too
  (0x1025dd60); it is saved as config 0x49 by 0x10193740; the view distance does not change), **near 0.1**, far 65535. Aspect = 1 /
  (k · 0.791) where k = y / (0.75 · x) for the Config aspect setting x : y (DATA 0x1034f4a0: 4:3, 15:9, 15:10, 16:9,
  16:10, 5:4; CODE 0x10002e80 -> 0x10015360, k from +0x2f0 = y / (x x 0.25 x 3)) and 0.791 is the camera object's
  +0x2e8 (ctor 0x1001086b; its only other writer, 0x100153f0, is never called or referenced): a 4:3 setting gives
  1.264, so the picture is drawn about 5.5% wider than true. ffxi-linux uses the game's fovY (the zoom, `cfg_73`), near
  plane and aspect (`gfx::gameAspect`: 0.948 of the window's; `XI_ASPECT_WINDOW=1` for the window's own). Not compared
  against the Windows client.
- **Furniture** (Mog House; CODE 0x1016ee32 -> 0x1016d490): every furniture actor within 4 whose origin is at y >=
  -0.1 (floor level 0 or 1: not wall items or ones on a table) is a box of 12 triangles turned with the piece. Its size
  comes from the furn row, not the model (CODE 0x101cd2a4: actor +0x764 / +0x768 / +0x76c = w · 0.6, height · 0.01,
  d · 0.6); x and z span ± half of that, but y runs from a constant 1.0 above the origin (the box's own top is never
  read) to height / 2 below, so every floor piece blocks the sphere whatever its height (`client::mog::furnitureBox`).
  No per-item flag is read (only the actor type 0x10330684, y >= -0.1, distance^2 < 16 and a matrix). The furniture
  table has no rugs (`xi-dat furn`: no Rug / Carpet / Mat names); its flattest floor pieces are mannequins (height 0.01)
  and the campfire / bonfire (0.03), solid objects, which the 1.0 box suits. `xi-dat walkcheck ZONE` walks 200 characters 600 steps each and reports pass-throughs
  (a wall crossed at 0.75 / 1.0 above the feet; a step off a ledge is tested level at the old height, as it moves across
  and then falls: the earlier "pass-throughs off cliff lips" were the line from the lip to the lower floor dipping
  through the cliff face), falls (no floor under a step) and jitter steps (a jump over 0.02 that lands back within 0.02
  of a spot 2 or 3 steps before). Jitter: pressing into a slanted rock face, the 0.75 step-up lifts the sphere into it and
  the ease-out pushes it back down, 2- or 3-spot cycles bobbing 0.08-0.2 every frame (Gusgen game (6.5, -181.7) and
  (23.1, -245.9)); `move()` now tries a step that goes against the input and changes height up to three more times and
  stays put if that comes back (the game refuses moves it cannot resolve). Jitter steps per 200 x 600 walk, before / after:
  234 867 / 81, 196 926 / 76, 149 963 / 76, 102 343 / 36, 100 247 / 21, 104 68 / 23, 116 611 / 28; reach unchanged
  (within 0.05%); pass-throughs 0 in 234 / 149 / 102 / 100 / 104, 1 in 196 (a steep 4-yalm drop), 3 in 116 (knee-high
  rocks the 0.75 step-up gets into and the game's 0.5 does not: the hybrid's known cost). The Gusgen stops themselves
  are the game's too: the pure sphere (`XI_MOVETEST_STEPUP=0.5 xi-dat movetest`) blocks at the same places.
  `XI_MOVE_NOCYCLE=1` turns the cycle test off; `xi-dat colnear ZONE X Z [R]` lists collision triangles near a point.

- **Water:** the sea and rivers have collision floors like any other ground, material 8 (shallow) / 9 (deep water)
  (Port Bastok: 6,144 deep-water floor triangles at game y 9.13; Selbina / Mhaura: at the surface). The ground probe
  (CODE 0x101813e0 -> 0x101691a0 -> 0x10169a50 / 0x1016a8a0) and the move resolver take the highest hit with no
  material test; the material only feeds footsteps and the `i900` wake. So a character on the water stands on it (wading),
  and only geometry keeps people out of the sea. ffxi-linux: a sliver between floor triangles over a lower floor (Port
  Bastok's harbour stairs leave 0.02 at each riser over the sea floor 2.2 below) is stood on as the game's probe meets
  the riser's face (`ZoneCollision::sliverHeight`: a steep face with floor 0.3 to both sides along x, z or a diagonal),
  in the move, the gravity and the arrival snap; `xi-dat waterreach ZONE [STEP...]` lists where a walk from the zone
  lines first gets from dry ground onto deep water (Port Bastok 3 -> 0 with this; Selbina 61, Mhaura 3, Norg 15, Port
  Windurst 215, Bibiki Bay 81 places, down embankments and cliffs; Port San d'Oria and Kazham 0).

### 7.8c Other actors on the floor (CODE 0x100a4e00 -> 0x101813e0 -> 0x101691a0 / 0x1016a8a0, 2026-10-24; #93)

- **The probe:** the actor update stands NPCs, monsters and other players on the collision floor, not at the server's
  height. With probe flag 1 (`0x101813e0(pos, out, 1)`, CODE 0x100a4ff4) the segment runs from the collision's step
  height above the server position (collision +0x16c, 2.0; 1.5 in zone 149, CODE 0x10178d00) to 40 below it
  (0x10377320, set per zone by 0x1018b2b0, default 40), and 0x1016a8a0 keeps the hit nearest the segment's start (the
  smallest distance from it, 0x1016ae83), so the **highest floor from 2 above to 40 below**. On a hit the actor's drawn
  position (+0x5c4) takes the hit height (0x100a5442); on a miss the server height stays. The player's own probe
  (flag 0, 0x10169a50) starts 0.5 above the feet instead (§7.8b).
- **Who keeps the server height:** an actor whose entity has +0x120 bit 20, +0x120 bit 21 or +0x128 bit 14 set
  (0x100a4e79). Bit 20 is the 0x00D / 0x00E **GroundFlag**, +0x18 bit 15 (CODE 0x1009bb2d; LSB writes its `moving`
  value there, and 3,649 of the 18,714 NPCs in LSB's `npcs.yaml` set the bit: moogles, "???" markers, wells, auction
  counters, the hovering ones). Bit 21 is an event's op 33 (event-format.md, the op table). +0x128 bit 14 is a PC's HackMove, 0x00D
  +0x20 bit 27 (CODE 0x1009a456).
- **INFERRED / not traced:** the actor update skips the ground step while any of the actor's vfuncs +0x34c / +0x358 /
  +0x360 / +0x36c / +0x378 returns non-zero, and skips the probe (marking the actor grounded) when +0x5e1 is clear,
  float +0xbc is above 10, vfunc +0x3f0 is false and the owner flag (entity +0x120 bits 7 / 8) is clear; what those
  are was not traced, so ffxi-linux probes every actor. A hit on a moving platform (0x10181470) takes the platform's
  height; not done.
- ffxi-linux: `client::actorGround` (src/client/actor_ground.h) on `ZoneScene::groundHeight`, from
  `ClientApp::standOnGround` each frame (cached per server position) for every entity but GroundFlag / HackMove
  ones, doors, lifts and ships (look kinds 2..4) and the Mog House's own actors; a cutscene's actors where the event
  moves them are placed by the event (Cutscene::place). `xi-dat npcground ZONE` runs the probe over lines of
  "ID X Y Z KEEP NAME" and `tools/npc_ground_check.py` feeds it LSB's `data/zones/*/npcs.yaml` (read-only).
- Survey (2026-10-24, every zone's shown NPCs with a model, doors / lifts / ships left out): 11,443 actors, 2,328 keep
  their height (GroundFlag), 8,959 stood on a floor, 156 with no floor within 40 below. Of those on a floor, 1,061 moved
  more than 0.3: 631 were exactly 0.5 above it (LSB's hand-placed heights, as the Synergy furnaces and engineers at y
  -4.5 over Port Windurst's -4.0 ground, and San d'Oria's Voidwatch Officer and Atmacite Refiner at 1.5 over 2.0), 134
  more than 1 above (17 more than 5: Sauromugue Champaign [S] 23, Northern San d'Oria's Jeanparmand 8, Port Bastok's
  Synergy Engineer 6.8, standing where the collision has no floor at their height), and 15 were raised more than 1
  (sunk into the floor).

### 7.9 Open (collision)
- Meaning of placement flag bits 0, 1, the signed 20-bit field, 0xA8 data, 0xAC..0xAF indices (remap table 0x106299b8)
  and 0xB0; the `p[0x1D]` bits 0, 2, 4; which game systems call each of the five segment queries.
- Zone 0 (Mzb version 0x17) placement layout.
- Material names are from LSB, not from the client (the client only passes the 4-bit value on).

## 8. Zone lines (Rid chunk, type 54)

DATA (2026-10-16): checked against LSB's `data/zones/*/zone.yaml` zone lines in zones 230, 231, 240, 100 and 104.
CODE (2026-10-18): the trigger test is traced (below).

The zone model DAT has one `Rid` chunk (type 54). Payload layout:
- Header: `"RID\0"`, then a u32 (6 in zone 230), …, and a u32 at +0x10 = 0x30 (the record offset).
- Records: 64 bytes each. CODE (0x10173b20): the count is at +0x30 and the records start at +0x40, so the table
  below reads each record 0x10 bytes early (its +0x00..0x0F are the previous record's last bytes, or the header);
  its field values are right. Record +0x24 (table 0x34) is the id; ids starting with `_` are door collision boxes
  (§5.8).

| Off | Type | Meaning |
|---|---|---|
| 0x00 | 16 bytes | zero in the samples |
| 0x10 | f32 ×3 | centre x, y, z (game coordinates; equals LSB's `from`) |
| 0x1C | f32 | 0 |
| 0x20 | f32 | yaw, radians (multiples of π/4 seen) |
| 0x24 | f32 | 0 |
| 0x28 | f32 ×3 | size x, y, z |
| 0x34 | char[4] | region id (fourcc); for zone lines this is the `RectID` the client sends in C2S 0x05E |
| 0x38 | char[4] | arrival region id in the destination zone (e.g. `z6e4` → `z6f1`); zero for arrival points |
| 0x3C | u32 | a zone or region number (230 for San d'Oria lines; 253, 338, 342 elsewhere); unknown |

Record kinds:
- **Zone lines:** id starts with `z` and the arrival id is non-zero.
- **Arrival points:** id starts with `z`, arrival id zero (e.g. `z6e5`, where BotD first spawned).
- **Other regions:** ids starting with `m` (INFERRED: map or event regions).

Trigger test (CODE 0x10175820, called with the previous and the current position; `dat::ZoneLine::contains` /
`crossedBy`):
- Only ids starting `z` / `Z` whose record is enabled.
- Into the box's frame: `T(-centre) · RotY(-yaw) · S(1 / size)`; **inside = -0.5 <= l < 0.5 on all three axes**:
  centred, half the stored size, no padding (the stored y is the centre: zone 230 `z6e4`, centre -8.47, size 15, spans
  the passage floor at -2.0).
- Either end inside, or the step between them crossing the box (the segment against the unit cube's faces, CODE
  0x101767b0): a fast step through a thin box still counts.
- C2S 0x05E (+0x04 = the id) is sent **once**: a latch blocks re-sending until a server packet clears it (CODE 0x1009f070,
  INFERRED the refusal) or the zone changes. xi-client clears it on the zone change, or after 6 s without one.
- Verified live (2026-10-18): BotC walking north through `z6e4` changed to Northern San d'Oria.

The server checks only that its last known position for the character is within 40 yalms of the line (`0x05e_maprect.cpp`).

Verified live: walking into `z6e4` in Southern San d'Oria sent 0x05E. The server answered with 0x00B, and the client re-zoned into Northern San d'Oria (zone 231) within about 2 s.

Zone-in events:
- After the zone change, the server can start an event inside S2C 0x00A: `EventNum` at +0x62, `EventPara` at +0x64.
- Until the client answers with C2S 0x05B EVENTEND, the character stays "InEvent" and logout is refused.
- xi-client currently finishes every event at once with option 0.

## 9. Zone environment (Weather chunks): lighting, fog, sky

Session 4 (2026-10-16), same binary and REA/Ghidra session. Reference parser: `tools/zone_weather.py`
(`zone_weather.py ZONE WEATHER_ID HH:MM`, `--list`, `--from ID --blend T`, `--raw`).
Names applied with `set_addresses_names` (ev_527fdc7db2a2b3d036e614659d93f554798c73be62caf886b2c98388084dee4b):
`Weather_ctor` 0x101892c0, `WeatherRec_InitDefaults` 0x10189310, `WeatherRec_Lerp` 0x101893a0,
`WeatherRec_LerpLightSet` 0x101897f0, `WeatherRec_LerpSky` 0x10189890, `LerpF32` 0x10189720,
`Env_ApplyRecord` 0x101880a0, `Env_SetRecord` 0x10187fd0, `Env_UpdateSunMoon` 0x10187630, `Env_UpdateLightDirs` 0x10187510,
`Env_BuildLight` 0x10188bb0, `Env_GetAmbient` 0x10188d10, `Env_GetFog` 0x10189030, `Env_FogDistScale` 0x101891c0,
`EnvMgr_GetLight/GetAmbient/GetFog` 0x1018b8b0/0x1018b8f0/0x1018b930, `Env_ChangeWeather` 0x10187400,
`WeatherChannel_ctor` 0x1018a6b0, `WeatherChannel_UpdateBlend` 0x1018acf0, `WeatherChannel_UpdateKeys` 0x1018a250,
`WeatherChannel_FindPrevKey/FindNextKey` 0x10189d40/0x1018a000, `Weather_KeyTimeFromName` 0x100720b0,
`EnvMgr_WeatherIdToFourcc` 0x1018ba20, `EnvMgr_FourccToWeatherId` 0x1018ba30, `EnvMgr_SelectFourccTable` 0x1018b9f0,
`EnvMgr_SetZoneResource` 0x1018b5b0, `Env_ApplyZoneDefaultRecord` 0x1018af20, `EnvMgr_ZoneChange` 0x1018b2b0,
`VanaClock_Update` 0x1018c1c0, `Env_GetTimeOfDay` 0x10189b70, `Pkt057_Weather` 0x1009f2f0,
`Weather_ApplyPendingPacket` 0x1009f220, `Event_SetWeather` 0x10093f70; data `g_EnvMgr` 0x10629fe4,
`g_VanaTimeOfDayTicks` 0x10629ff8, `g_DefaultWeatherRec` 0x10629f38, `g_WeatherFourcc` 0x10377bb8,
`g_WeatherFourccAlt` 0x10377c08, `g_WeatherNames` 0x10377b18.

Evidence (decompiles): ev_8f76b9b92494e894bd938172d4ba96b02dacba26fbf7af52ceca7b9e94c60292 (ctor, EnvMgr getters);
ev_f4e0996279bb124051a8f7959fdb103a00f89594164976d8b8957c5e00752778 (InitDefaults, BuildLight, GetAmbient, GetFog);
ev_e029cecf432078e9751c54cc0be7788d06eab9af2607ceb0406138552ab1396a (env field getters, FogDistScale);
ev_83d3347a93258f01493424f2e5af75dcab02528fb079369dcbdf70bdbd3e9fe0 (FindPrevKey/NextKey, SetZoneResource);
ev_24642c69c7b3b09d32c8860a2426928839f4264e84b33eb650f8c5b1b93af12f (GetTimeOfDay, KeyTimeFromName, ApplyZoneDefaultRecord);
ev_ee32d4e32f38cfad4fadee7b1d4743bb1c06801afaa8f9091d78429320706e13 (zone load 0x1018b410, VanaClock_Update);
ev_44e6d968e11df6e573f42cad34bcf1bf304e600906413fefa44a172a68e46831 (UpdateKeys, channel ctor, Lerp, ApplyRecord,
UpdateSunMoon, SetRecord, EnvMgr ctor 0x1018b080 defaults);
ev_2b8691883d25a78533f9e72509b6378080d9ba3541d125dcbd2c9caabce5144f (UpdateBlend thunk, 0x1018baf0 = ×25, LerpF32);
ev_dc57b82dd07bdc2c67e9cddc62f632ba1866d5ac50212da38263c7d87101e91b (UpdateBlend, blend countdown);
ev_4fdc783689ae1cddf11c3263f654785d55b4049a6d92c2158be1b8f814d446cf (clear/fog colour setters);
vtable bytes 0x103367e8: ev_4630712f8d7d695962822f9e1703ae719934345981608bbebad066db1af24405.
Everything Ghidra could not show was read from the objdump listing (packet handler, `Env_ChangeWeather`, the fourcc
tables and the float constants 25.0 / −25.0 / 3600.0 / 255.0).

### 9.1 Chunk object (CODE)
- Factory type 47 → `Weather_ctor` 0x101892c0: base ctor, `WeatherRec_InitDefaults(obj+0x30)`, vtable 0x103367e8.
  Slot 7 is the default `CMoResource_LoadRaw` (copies the chunk over obj+0x20, so the defaults are overwritten), slot 8
  (init) is an empty `ret` at 0x10189300. **The object is pure data**: obj+0x30 = the 0xB0-byte payload, read in place.
- The record struct is 0xAC bytes (`rep movs` of 0x2B dwords in 0x1018a670 and `InitDefaults`); payload bytes
  0xAC..0xAF are chunk padding (always 0, DATA).
- The chunk **name is the keyframe time** `HHMM` in ASCII: `Weather_KeyTimeFromName` 0x100720b0 computes
  `hh = (c0−'0')*10 + (c1−'0')`, `mm = (c2−'0')*10 + (c3−'0')` (`−0x210` = `−'0'·11`).

### 9.2 Record layout (payload offsets; CODE unless marked)
Colours are **R, G, B, A bytes in that order** (A = 0x80 everywhere). The code reads `rec[o+0]` as red
(`Env_ApplyRecord` stores `rec[o]/255` into the x/r float), and DATA agrees: noon sky stop `67 95 da` is blue,
18:00 `c0 76 56` orange. Two identical 0x20-byte "light sets" exist: **set Z at +0x2C (zone meshes, light index 0)**
and **set A at +0x0C (actors/characters, light index 1)** — index = `which` argument of `EnvMgr_GetLight(light*, which, key)`;
the zone pass (0x1017bbf4) passes 0, the character draw at 0x100a5c4c passes 1 when a model's vfunc+0x230 returns 0.

| Off | Type | Meaning | Consumer / basis |
|---|---|---|---|
| +0x00 | u32 | flags. **bit0 = explicit light directions** (else sun/moon from the clock). bit2 = record is a heap temp (in-memory only, set by 0x1018adc0, freed by 0x1018acb0). DATA: bit0 set in all records of 140 zones (dungeons/indoor), clear in others | `Env_ApplyRecord`, `Env_UpdateSunMoon`, `WeatherRec_Lerp` (bit0 OR-ed) |
| +0x04, +0x08 | — | not read or interpolated (0 in DATA) | |
| **+0x0C** | rgba | set A **sun light colour** (day light; the only light colour if bit0) | env+0x64 → light 1 diffuse |
| **+0x10** | rgba / int8×3 | set A **moon light colour** (night); if bit0: **light direction** int8 x,y,z (normalised) | env+0x74 / env+0x24 |
| **+0x14** | rgba | set A **ambient** (D3DRS_AMBIENT for actors) | env+0x84, `Env_GetAmbient(…,1)` |
| +0x18 | rgba | set A fog colour copy (equals +0x38 in most DATA); stored to env+0x94, no reader found | INFERRED |
| **+0x1C** | f32 | set A **fog end** | `Env_GetFog(…,1)` |
| **+0x20** | f32 | set A **fog start** | `Env_GetFog(…,1)` |
| **+0x24** | f32 | set A **light diffuse multiplier** (1.36–1.7 in DATA) | `Env_BuildLight(…,1)` |
| +0x28 | — | not read or interpolated | |
| **+0x2C** | rgba | set Z **sun light colour** | env+0xe4 → light 0 diffuse |
| **+0x30** | rgba / int8×3 | set Z **moon light colour**; if bit0: **direction** | env+0xf4 / env+0xa4 |
| **+0x34** | rgba | set Z **ambient** → zone-pass D3DRS_AMBIENT (mgr+0x39250) | env+0x104, `Env_GetAmbient(…,0)` |
| **+0x38** | rgba | **fog colour** → D3DRS_FOGCOLOR. Used for **both** sets (`Env_GetFog` calls env+0x114 in both branches, disassembly 0x10189099 / 0x10189111) | env+0x114 |
| **+0x3C** | f32 | set Z **fog end** → FOGEND (mgr+0x39258) | `Env_GetFog(…,0)`, also device+0x300 |
| **+0x40** | f32 | set Z **fog start** → FOGSTART (mgr+0x3925c) | also device+0x2fc |
| **+0x44** | f32 | set Z **light diffuse multiplier** | `Env_BuildLight(…,0)` |
| +0x48 | — | not read or interpolated | |
| **+0x4C** | rgba | **clear colour** (`Clear(TARGET\|ZBUFFER, 0x80RRGGBB, 1.0)` via device+0x308, 0x10014d70; Clear at 0x100115f0). Set only at zone load from the zone's first Weather chunk (`Env_ApplyZoneDefaultRecord`). Equals sky stop 0 in DATA | CODE |
| +0x50 | f32 | → device+0x318; used negated as a depth/plane value by a full-screen pass that runs only when +0x5C ≠ 0 (0x100121ec..0x10012420) | INFERRED: screen effect depth |
| +0x54 | f32 | → device+0x314 (paired with +0x5D) | INFERRED |
| **+0x58** | f32 | **actor draw distance** (`0x1018b800`: `scale × rec+0x58`, default 100; also >500 halves the fog scale under a config switch in `Env_FogDistScale`) | CODE |
| +0x5C, +0x5D, +0x5E | u8 | effect selectors paired with +0x50/+0x54 (event scripts can override them, 0x10059165..); +0x5E unknown | INFERRED |
| +0x5F, +0x60, +0x64 | — | not interpolated (0 in DATA; default record has +0x60 = 0x80808080) | |
| +0x68 | f32 | interpolated; ≈2030 in DATA; consumer not found | INFERRED: far distance |
| **+0x6C..+0x88** | rgba×8 | **sky gradient colours** (stop 0 = horizon, = +0x4C; stop 7 = zenith) | interpolated by `WeatherRec_LerpSky`; consumer not located (INFERRED sky dome) |
| **+0x8C..+0xA8** | f32×8 | **sky gradient positions**, ascending 0.0 … 1.0 | same |
| +0xAC..+0xAF | — | padding (not in the record) | DATA |

`Env_BuildLight` 0x10188bb0 output (`D3DLIGHT8`, CODE): Type = DIRECTIONAL (3); Diffuse = `rgb/255 × mult`, a = 1.0
(may exceed 1); Specular = 0; Ambient = the global float4 at 0x10629f14; Direction = env dir (below); position/range 0.
If the env object is not "active" (`[this−0x1c] != 1`) it returns Diffuse = Ambient = 255.0 and Direction (0, 1, 0) instead.
`Env_GetAmbient` converts the float rgb back to a D3DCOLOR (0x10188d10; optional brightening via 0x10017030 when the
config flag 0x10456a7c is set and all channels < 0.8).
`Env_GetFog`: `end = rec[+0x3C or +0x1C] × s`, `start = rec[+0x40 or +0x20] × s`, `if end <= start: end = start + 1`.
`s = Env_FogDistScale(which)` = 1.0 normally; user draw-distance settings multiply it (0x10456ab8 for set Z,
0x10456aa8 for set A), and env+0x1d4 clamps it (set to 0.7 by `EnvMgr_ZoneChange` for zone 50 only).
An unrelated oddity: `Env_SetRecord` also pushes `rec[+0x38] × 255.0` (bytes, not /255) into device+0x304; the per-frame
FOGCOLOR comes from `Env_GetFog`, so ignore that value.

### 9.3 Validation (DATA, zone 230 = file 330, `weat/*`)
`zone_weather.py 230 <id> <time>` and a field dump of all 26 `weat` records:
- 12:00 suny: sun colour (207,207,199) ×1.5, ambient (189,190,193), fog (88,97,124) start 44.5 end 396, sky stops
  (154,185,218) → (45,49,125). 00:00 suny: sun colour 0, moon colour (92,96,120), ambient (92,96,120), fog (21,23,32).
- Day records have +0x30 (moon) = 0; night records have +0x2C (sun) = 0 — consistent with the sun/moon selection below.
- Fog end by weather: fine 450, suny 396, clod 302, **mist 129–203**; mist fog colour at noon (157,159,162) grey.
- 18:00 sky stop 0 (230,176,111) orange, zenith (27,24,61); 06:00 (233,227,156).
- bit0 zones (e.g. zone 9 `bliz` 0000): +0x10 = (12,50,21), +0x30 = (0,46,0) as int8 → mostly +y = **down** in
  game space (Y-down), i.e. light from above.

### 9.4 Group selection from the server weather id (CODE)
Zone file layout (DATA, 307 of 323 zone files have Weather chunks):
```
<root> { ... weat { suny { Generater/KeyFrame/textures/Mmb (sky, sun, clouds), Weather "1800","2100",...,
                           moon{...}, indo{...}, lf01{...} }  clod{...}  fine{...}  mist{...} ... }
         ev01 { suny{1200,0000} mist{...} ... }  ev02 {...}  ev10 {...} }     // event / cutscene sets
```
- Weather id → fourcc: `EnvMgr_WeatherIdToFourcc(i) = table[i]`, table = env+0x1d8 =
  `g_WeatherFourcc` 0x10377bb8 or `g_WeatherFourccAlt` 0x10377c08 (`EnvMgr_SelectFourccTable(0|1)`; zone change
  selects 0, event opcode at 0x1005cf78 can select 1). Display names at 0x10377b18 (8-byte stride).

  | id | fourcc (alt) | LSB weather | id | fourcc (alt) | LSB weather |
  |---|---|---|---|---|---|
  | 0 | fine (fin1) | None | 10 | wind (win1) | Wind |
  | 1 | suny (sun1) | Sunshine | 11 | stom (sto1) | Gales |
  | 2 | clod (clo1) | Clouds | 12 | snow (sno1) | Snow |
  | 3 | mist (mis1) | Fog | 13 | bliz (bli1) | Blizzards |
  | 4 | dryw (dry1) | Hot Spell | 14 | thdr (thd1) | Thunder |
  | 5 | heat (hea1) | Heat Wave | 15 | bolt (bol1) | Thunderstorms |
  | 6 | rain (rai1) | Rain | 16 | aura (aur1) | Auroras |
  | 7 | squl (squ1) | Squall | 17 | ligt (lig1) | Stellar Glare |
  | 8 | dust (dus1) | Dust Storm | 18 | fogd (fog1) | Gloom |
  | 9 | sand (san1) | Sand Storm | 19 | dark (dar1) | Darkness |

  The LSB names are the standard FFXI weather enum order (INFERRED pairing by index; the fourccs are CODE/DATA).
- `WeatherChannel_ctor` 0x1018a6b0: find the type-1 block `weat` (0x74616577) in the zone resource, then the child
  block named by the fourcc. **If it is missing, fall back to `suny`** (0x796e7573) and log an error
  (format strings at 0x10377ab0 / 0x10377a58). DATA: zone 230 has only suny/clod/fine/mist, so rain → suny.
- Keyframes = the type-47 chunks inside that group block (iteration `0x100741e0(block, 0x2f, 0, −1)`; −1 presumably
  "any depth", INFERRED). The channel also starts the block's type-7 Schedulers (sky/sun/cloud effects).
- S2C **0x057** (`Pkt057_Weather` 0x1009f2f0): `StartTime u32 @+4`, `WeatherNumber u16 @+8`,
  `WeatherOffsetTime u16 @+0xA` are latched with a pending flag; `Weather_ApplyPendingPacket` 0x1009f220 applies them
  when no event blocks it: `Env_ChangeWeather(fourcc[WeatherNumber], StartTime·60·25 ticks, OffsetTime·3600 ticks)`.
- S2C **0x00A** (zone-in, 0x100940c0 at 0x1009505f..0x10095165): first
  `Env_ChangeWeather(fourcc[WeatherNumber2], WeatherTime2·1500, offset2·3600)`, then
  `Env_ChangeWeather(fourcc[WeatherNumber], 0, 0)`. Because of the rule below, the zone comes up in WeatherNumber
  immediately (INFERRED effective behaviour; the first call only matters when both ids are equal).
- `Env_ChangeWeather(fourcc, start, duration)` 0x10187400 (disassembly): if `duration == 0` and fourcc ≠ current →
  delete every channel; then if fourcc ≠ current → push a new `WeatherChannel` (0x46 bytes) whose `+0x14 remaining =
  +0x18 duration = duration`, keeping the old channel at `+0x08` for the cross-fade. `start` is not used here.

### 9.5 Time and interpolation (CODE)
- **Units**: 1 tick = 1/60 Vana'diel second. Minute 0xE10 = 3600, hour 0x34BC0, day 0x4F1A00 = 5,184,000,
  wrap 0x6F3C9000 = 360 days (`0x1018bb50`). `0x1018baf0(x)` = 25·x, so earth seconds → ticks = ·60·25 = ·1500.
  Per frame the clock advances `frameDelta(1/60 s, ≥1) × 25` ticks (0x1018c180, const 25.0 at 0x1032cf58).
- **Clock**: `g_VanaTimeOfDayTicks` 0x10629ff8 = time of day; `VanaClock_Update` 0x1018c1c0 copies h/m/s from the
  full clock 0x10629ffc and sets day fraction 0x10629ff4 = ticks / 5,184,000. `Env_GetTimeOfDay` 0x10189b70 returns it,
  or env+0x1c8 when an event override is active. With S2C 0x057's earth-second StartTime and the LSB epoch
  (1009810800 = 2002-01-01 00:00 JST), the time of day is `((now_earth − 1009810800) × 25) mod 86400` Vana seconds
  (INFERRED from the ×1500 conversion; matches the standard Vana'diel clock).
- **Keyframes** (`FindPrevKey` / `FindNextKey`): `prev` = key with the smallest `now − key ≥ 0` (first in file order
  on ties; if none, the last key iterated); `next` = key with the smallest `key − now > 0`, else the earliest key
  (wraps past midnight). File order is unsorted (DATA: 1800, 2100, 0500, ...).
- **Interpolation** (`WeatherChannel_UpdateKeys` 0x1018a250 → `WeatherRec_Lerp(next, prev, t)` 0x101893a0):
  ```
  span = (key(next) - key(prev)) mod day;  el = (now - key(prev)) mod day;  t = el / span   (prev==next → copy)
  colour byte: out = prev + trunc((next - prev) * t)      // unsigned bytes, __ftol truncation (0x10189660)
  float:       out = (next - prev) * t + prev               // 0x10189720
  bit0 dirs:   rotate prev→next by t (0x10033290), renormalise, store as int8   // +0x10, +0x30
  lerped: +0x0C..+0x24, +0x2C..+0x44, +0x4C..+0x58, bytes +0x5C..+0x5E, +0x68, +0x6C..+0xA8
  ```
  Day/night keys are linear in between (e.g. zone 230 suny: 12:00 → 17:00 over 5 h).
- **Weather cross-fade** (`WeatherChannel_UpdateBlend` 0x1018acf0, per frame): `remaining += frameDelta × −25`
  (const −25.0 at 0x1033685c); `w = (duration − remaining) / duration`;
  `rec = WeatherRec_Lerp(newChannelRec, oldChannelRec, w)`; when `remaining` reaches 0 the old channel is deleted.
  So a 0x057 change fades over `OffsetTime` Vana minutes (LSB sends 4..28 → 9.6..67 earth s). Both channels keep
  following the clock while fading.
- **Sun / moon direction** (`Env_UpdateSunMoon` 0x10187630, only when bit0 is clear):
  ```
  a    = ticks / 5184000 * 6.283
  moon = (sin a, cos a, 0)            sun = (sin(a+3.1415), cos(a+3.1415), 0)     // game space, Y down
  optional rotation by env+0x1cc degrees (event opcode 0x10057fcb)
  day  = sun.y >= 0   (06:00..18:00)
  day:   light0 = (sun,  rec+0x2C × rec+0x44)   light1 = (sun,  rec+0x0C × rec+0x24)
  night: light0 = (moon, rec+0x30 × rec+0x44)   light1 = (moon, rec+0x10 × rec+0x24)
  ```
  At 12:00 a = π → sun = (0, 1, 0) = straight down; at 06:00 sun = (−1, 0, 0) (light travels west, sun in the east;
  +x = east, §5.5). When the global 0x10456a28 == 0x60 (a config mode) the switch is smoothed: direction and colour
  ease 10% per frame towards the target (factor 0.1), and the colour is handled with a 0.7 factor depending on
  `|dot(old, new)| ≥ 0.5` (not fully decoded). The default path switches instantly at 06:00 / 18:00.
  With bit0 set: light0 = (+0x30 dir, +0x2C colour), light1 = (+0x10 dir, +0x0C colour), no clock dependence.
- The resulting record pointer is env+0x18 (`Env_SetRecord`), and env+0x1c = record+0x4C (sky sub-struct).

### 9.6 Zones without Weather (CODE + DATA)
- DATA: zones 625–640 have no Weather chunks. Zones 259–299 and zone 0 have Weather only in event blocks
  (`ev10/…`, `selp`), not in `weat`.
- `EnvMgr_SetZoneResource` 0x1018b5b0 takes the zone's **first** type-47 chunk (file order) and calls
  `Env_ApplyZoneDefaultRecord`: with a chunk, it is copied into `g_DefaultWeatherRec` 0x10629f38 and the clear colour,
  fog colour and fog start/end are set from it; **without one**: clear colour (0x80,0x80,0x80), fog colour
  (0x80,0x80,0x80), fog end 128.0 and `g_DefaultWeatherRec` is left as it was (INFERRED: stale from the previous zone;
  the EnvMgr ctor 0x1018b080 initialises it to colours 0x80, fog end 100 / start 80, multiplier 1.0, +0x4C black,
  +0x50 80, +0x54 80, +0x58 100). With no channel, `Env_SetRecord(NULL)` uses that record.
- A renderer can treat "no `weat` block" as: a fixed record (fog 0x80 grey, start 80, end 100, ambient/sun 0x80,
  multiplier 1.0, direction straight down) — INFERRED defaults, CODE values.

### 9.7 Confidence and open items
- High (CODE + DATA): record layout of the light sets, ambient, fog colour/start/end, multiplier, flag bit0; keyframe
  naming and selection; linear interpolation with day wrap and the byte truncation; fourcc table and `suny` fallback;
  0x057 field use and the cross-fade over OffsetTime Vana minutes; sun/moon formula.
- Medium: which draws use set A vs set Z beyond the zone pass (index 1 confirmed for one character path); 0x00A
  zone-in effectively immediate; the time-of-day origin (standard Vana'diel clock, via ×1500).
- Open: the consumer of the sky gradient (+0x6C/+0x8C) and of +0x68; exact meaning of +0x50..+0x5E (screen-effect
  pass); +0x18 reader; how the `suny` block's Mmb sky dome / Generaters (`sun0`, `cld1`, `moon`) are drawn.

## 10. Character models (Sk2 / Os2 / Mo2) and appearance → file mapping

Session 3 (2026-10-16), same binary and Ghidra session. Names applied with `set_addresses_names`
(ev_83ac636e53737b8f42cd0c77e850c179f83eb5040a300a1c93017dfb297addef):
`Actor_LoadModelFiles` 0x100d2e90, `Model_NpcIdToFileId` 0x100c50f0, `g_RaceBaseFileId` 0x1035f420,
`g_RaceSlotRangeTables` 0x1035f388, `g_Os2MirrorSign` 0x103515f8, `CMoOs2_Init` 0x1002d720,
`CMoOs2_FixupSections` 0x1002d820, `Os2_CountPolyVerts` 0x1002cab0, `Os2Vb_BuildMirrorCopy` 0x1002d270,
`CMoSk2_Init` 0x10034070, `Sk2_BuildBoneWorld` 0x10034ab0, `Sk2_CopyBindPose` 0x10034690, `CMoMo2_Init` 0x10019660,
`Model_SetSk2` 0x10029d80, `Model_AddOs2` 0x10029ea0.
Reference implementation: `tools/char_to_obj.py` (tables copied from the DLL). All offsets are relative to the chunk
**payload** (chunk start + 16; the resource object keeps the payload at obj+0x30).

**None of Sk2/Os2/Mo2 is obfuscated** (CODE): all three use the default chunk copy for vtable slot 7 (0x10071d30,
vtables 0x1032c828 / 0x1032c854 / 0x1032c7fc) and their slot-8 init functions only check the version byte and turn
word offsets into pointers. No XOR / swap stage like Mmb.

### 10.1 Which files make up a character — CODE (`Actor_LoadModelFiles` 0x100d2e90, ev_b61d64d6a371be22b97768c2da47c9d878127a8d4fdac39591d55405e19b3178)

Look data per actor: race byte at entity+0xEF (`0x10084350`), 9 × u16 look words at entity+0xFC (`0x100843b0(slot)`).
The S2C 0x00D/0x00E handler at 0x1009b098 takes the race from the **high byte of GrapIDTbl[0]** (CODE); LSB packs
`GrapIDTbl[0] = race<<8 | face` and `GrapIDTbl[s] = model + (s<<12)` for s = 1..8 (char_update.cpp), so look[0]'s
index is the face.

```
race r: 1 HumeM, 2 HumeF, 3 ElvM, 4 ElvF, 5 TaruM, 6 TaruF, 7 Mithra, 8 Galka (0 = HumeM; 9..28 rejected -> 0x20/1);
        29..31 and 32..36 are extra skeleton families (chocobo/other mounts, INFERRED) with slots 0..5 only
base   = g_RaceBaseFileId[r]                // u16 table 0x1035f420; holds the Sk2 + the race's Mo2 set
load(base)
for slot s in 0..8:                         // 0 face, 1 head, 2 body, 3 hands, 4 legs, 5 feet, 6 main, 7 sub, 8 ranged
    look = look[s]; if (look >> 12) != s: look = (look & 0xFFF) | s<<12
    R = g_RaceSlotRangeTables[r] + s*0x30   // 6 x {i32 startFid, i32 count}
    idx = look & 0xFFF
    if idx >= sum(count): log, idx = 0
    walk ranges k = 0..5: if idx < count_0+..+count_k: fid = start_k + idx - (count_0+..+count_{k-1})
    load(fid)                               // every Os2 in the file is added to the model (Model_AddOs2)
```
Base file ids (DATA = CODE table): HumeM 7072, HumeF 10248, ElvM 13424, ElvF 16600, Taru M/F 19776 (one shared
skeleton; TaruF only differs in face range 22952 and a few head/hands ranges), Mithra 23176, Galka 26352.
Per-race slot ranges (startFid, count) — Hume male shown; other races follow the same pattern, full tables in the tool:

| slot | ranges (startFid, count) |
|---|---|
| 0 face | (7080, 32) |
| 1 head | (7112,256) (63323,48) (63371,16) (71247,256) (98787,32) (102961,64) |
| 2 body | (7368,256) (63387,48) (63435,16) (71503,256) (98819,32) (103025,64) |
| 3 hands | (7624,256) (63451,48) (63499,16) (71759,256) (98851,32) (103089,64) |
| 4 legs | (7880,256) (63515,48) (63563,16) (72015,256) (98883,32) (103153,64) |
| 5 feet | (8136,256) (63579,48) (63627,16) (72271,256) (98915,32) (103217,64) |
| 6 main | (8392,512) (63643,128) (72527,256) (107301,300) (0,64) |
| 7 sub | (41199,512) (66459,128) (81999,256) (105201,300) (0,64) |
| 8 ranged | (9416,256) |

A range with start 0 (the last main/sub range) yields no usable file. The base file also contains `Generater`/
`Scheduler` (effects) and 130 Mo2 chunks for Hume male (DATA). File names in the Os2 chunk names: `hh_h` face/hair,
`hh_m` head gear, `hf_d` "no head gear" stub, `hh_b` body, `hh_g` hands, `hh_l` legs, `hh_f` feet, `wep0/1/2`
main/sub/ranged (DATA).

**NPCs / monsters (CODE, `Model_NpcIdToFileId(kind, id)` 0x100c50f0, disassembly 0x100c5139):**
```
kind 0 (model id):  id <  1500 -> 1300  + id
                    id <  3000 -> 51795 + (id - 1500)
                    id <  3500 -> 99907 + (id - 3000)
                    else       -> 101739 + (id - 3500)
kind 1 -> id + 39275; kind 3 -> id + 100507; kind 4 -> id + 102705; kind 2 -> 0x101cc9b0(id)
```
In `Actor_LoadModelFiles`, actor model type 2 (and 7 with look[0] != 0) calls kind 0 with look[0]; types 0/1/8 use
the race/slot path above; type 6/7 use look[9] | 0x9000 (automaton/chocobo-like, INFERRED). LSB's 0x00E sends a
`model_id` for MODEL_STANDARD/UNK_5/AUTOMATON and a full GrapIDTbl for MODEL_EQUIPPED/CHOCOBO (entity_update.cpp
`SubKind`); which SubKind maps to which client actor type was not traced (INFERRED: model_id → kind 0, GrapIDTbl →
race path). An NPC model file is self-contained: its own Sk2, Os2 set and Mo2 (DATA: 1301 = Galka-armour NPC, 1400 =
Hume NPC, 1600 = Mandragora, all render correctly with the tool's `npc` mode).

### 10.2 Sk2 (type 41) skeleton — CODE (`CMoSk2_Init` 0x10034070 ev_74df41bd..., 0x100341d0 ev_c9481673..., `Sk2_BuildBoneWorld` 0x10034ab0 and `Sk2_CopyBindPose` 0x10034690 disassembly) + DATA

```
+0x00 u8   version (must be 0, else "Sk2 Version Error")
+0x01 u8   0 (unused / runtime)
+0x02 u16  bone count N                         (obj+0x32; 0x100341d0 allocates (N+1)*64 bytes of matrices)
+0x04 Bone[N], 30 bytes each:
      +0  u8   parent index (bone 0 is the root; its parent byte is 0)
      +1  u8   flag (1 on leaf / attachment bones, DATA; not read by the code seen)
      +2  f32  qx, qy, qz, qw     local rotation quaternion   (0x10034690 copies +2..+0x11)
      +18 f32  tx, ty, tz         local translation           (0x10034690 copies +0x12..+0x1d)
+0x04+30N  trailing block (Hume male: 4600 bytes): u16 count (0x0080), u16 0xFFFF, then count attach points
      {s16 bone, f32[3] rotation, f32[3] offset} (26 bytes; effect-format §C.6); then three bounds sets
      {f32 top, bottom, x max, x min, z max, z min} (CODE 0x10035180; Hume M: -1.9 / 0, -2.1 / 0, -0.6 / 0: standing,
      riding, lying; the camera's model height, 0x1001f2c2 / 0x100cfa70); the head look-at limits at +0x48 and the
      soft-body spheres at +0x6c after them (`dat::Skeleton`)
```
World (bind) transform, row vectors (D3DX), computed parent-first in index order (0x10034ab0: `0x10027c40` multiplies
the bone's local matrix by the parent's world matrix): `W[i] = R(q_i)·T(t_i)·W[parent]`, `R` = D3DXMatrixRotationQuaternion.
Parents always precede children (DATA).

DATA (file 7072, Hume male, 94 bones): bone 0 root at the origin, 1/2 hips at y = −1.053, legs 26–31 / 32–37
(ankle y ≈ −0.14, toe y ≈ −0.04), spine 48→49→50→51→52 (head bone 52 at y = −1.63, 53 at −1.69), arms 60–73 / 74–88
mirrored in z (±0.27 elbow, ±0.33 wrist, hands at y ≈ −0.86, i.e. arms hanging), 89–93 helpers. The figure faces
**+x** (toes at larger x than ankles) with left/right along z, Y down. Bones 3–24 are children of bone 2 whose local
translation cancels the hip offset, so they sit at the origin in the bind pose: they are **weapon/prop attachment
bones that only motions position** (main weapon `wep0` binds to bone 3 for Hume, to bone 77 for Elvaan). Bone counts:
Hume 94, Elvaan 99, Taru 93, Mithra 108, Galka 107.

### 10.3 Os2 (type 42) mesh — CODE + DATA

Header (CODE: `CMoOs2_Init` 0x1002d720 ev_c9481673..., `CMoOs2_FixupSections` 0x1002d820 ev_2a9f5913...,
`Model_AddOs2` 0x10029ea0 ev_fd8f863b...):

| Off | Type | Meaning |
|---|---|---|
| 0x00 | u8 | version, 0 or 1 (else "Os2 Version Error") |
| 0x01 | u8 | 0 in files; the client sets 1 after init |
| 0x02 | u8 | bits 0..6: class. 0 = skinned mesh (VB class `CYyOs2VtxBuffer`, vt 0x1032a7b0); 1 = other class (0x100223a0, size 0x1c8; not decoded). **bit 7: bone refs index the mesh bone table** (else they are skeleton bone numbers) — DATA, 621/621 meshes |
| 0x03 | u8 | 0x00 face/hands, 0x01 head, 0x11 body, 0x21 legs, 0x31/0x32 feet (DATA; INFERRED slot/part code) |
| 0x04 | u8 | bit 0: **mirrored copy exists** (0 on weapons, 1 on body parts, DATA); bit 1 set at runtime when the VB is built (CODE) |
| 0x05 | u8 | 0 |
| 0x06 | 6 × {u32 offset, u16 size} | sections; offset and size are in **16-bit words** from the payload start (CODE: `p + off*2`) |
| 0x2A.. | | u16 values, e.g. 0x2E = total vertex output count (DATA, not used here) |

Sections (DATA, consistent for every mesh checked):

| # | Content |
|---|---|
| 0 | polygon command stream (u16 words, terminated by 0xFFFF) |
| 1 | bone table: u16 skeleton bone numbers |
| 2 | u16 n1, u16 n2 = number of 1-bone and 2-bone vertices |
| 3 | bone refs: 2 × u16 per vertex (n1+n2 entries; the second word is unused for 1-bone vertices) |
| 4 | vertices: n1 × 24 bytes, then n2 × 56 bytes |
| 5 | empty in all samples |

Vertices (CODE: `Os2Vb_BuildMirrorCopy` 0x1002d270 reads 24-byte and 0x38-byte records at sec4 and sec4 + n1*24):
```
1-bone (24 B): f32 x, y, z, nx, ny, nz                       (bone-local)
2-bone (56 B): f32 x1, x2, y1, y2, z1, z2, w1, w2, nx1, nx2, ny1, ny2, nz1, nz2
               (p1/n1 in bone A's space, p2/n2 in bone B's space, w1 + w2 = 1; positions and normals are
               stored PRE-WEIGHTED: p1 = w1·pA, p2 = w2·pB — DATA, see the check below)
```
Bone ref u16 (CODE for the sign bits; DATA for the index fields):
```
bits 0..6   bone for the normal copy        (index into sec1 if hdr[2] & 0x80, else a skeleton bone number)
bits 7..13  bone for the mirrored copy      (same addressing)
bits 14..15 mirror axis: g_Os2MirrorSign[ref>>14] = (1,1,1), (-1,1,1), (1,-1,1), (1,1,-1)   (0x103515f8)
```
Skinning rule (bind pose, what `tools/char_to_obj.py` does; validated visually):
```
normal copy:   v = p1 · R[A] + w1 · T[A] + p2 · R[B] + w2 · T[B]    (W = R·T; 1-bone: p · W[bone])
               n = n1 · R[A] + n2 · R[B]
mirrored copy (only if hdr[4] & 1): p := p * sign[ref >> 14], n := n * sign[ref >> 14] per bone component,
               bone = (ref >> 7) & 0x7F; triangle winding reverses (reflection)
```
Pre-weighting check (DATA, 2026-10-17): in the bind pose, (p1/w1)·W[A] and (p2/w2)·W[B] coincide exactly (max error
< 1e-4) for every 2-bone vertex of face/body/hands/legs/feet across Hume M, Elvaan F, Taru and Galka; p1·W[A] vs p2·W[B]
differ by a median 0.06–0.14 and up to 0.39. The earlier rule `w1·(p1·W[A]) + w2·(p2·W[B])` therefore applied the
weights twice and pulled every joint vertex toward the bone origins (the "deformed" look of hips, knees, elbows, hands).

So hands, legs, feet and parts of body/face are stored once and drawn twice; the mirrored copy is built on the CPU by
0x1002d270 (1000 vertices per step, `+0x76` buffer) — CODE. Max two influences per vertex.

Polygon command stream (CODE: `Os2_CountPolyVerts` 0x1002cab0, ev_07a6b354572d71203adb3b6ef1d1256f5e2f41b56726e1961d9c5a9e62420559):

| op (u16) | size in words | meaning |
|---|---|---|
| 0xFFFF | — | end |
| 0x8000 (`(op>>4)&0xF == 0`) | 9 | texture name: 16 bytes, the texture chunk's name (sec. 4), e.g. `tim     hm_ba1_1` |
| 0x8010 (`(op>>4)&0xF == 1`) | 23 | material, 44 bytes (below): the **hide group** of the triangles that follow, lighting scale, reflections |
| 0x0054 'T' | 2 + 15n | triangle list: n × {u16 i0, i1, i2; f32 u0, v0, u1, v1, u2, v2} |
| 0x5453 'ST' | 12 + 5n | strip of n triangles: {u16 i0, i1, i2; f32 uv×3} then (n−1) × {u16 i; f32 u, v}; odd triangles swap the first two |
| 0x0043 'C' | 2 + 5n | untextured triangles: n × {u16 i0, i1, i2; u32 colour} (layout INFERRED from size) |
| 0x4353 'SC' | 6 + n | untextured strip: (n+2) × u16 index + u32 colour (INFERRED from size) |

Indices address the n1 + n2 vertex list (1-bone first). UVs are per corner, not per vertex.

**0x8010 material** (CODE 0x1002dfd9..0x1002e511, 0x1002cafa; DATA 8,011 records in Hume M / F face .. feet). No blend,
alpha-test or z-write state is set from it: the nibble once read as a blend mode is a hide group.

| off | type | meaning |
|---|---|---|
| +0x00 | u32 | colour -> TFACTOR (always the neutral 0x808080 in data) |
| +0x0C | u16 | bits 8..11: **hide group** 0..7 of the triangles that follow (0 = never hidden) |
| +0x0E | u16 | 1 = environment reflection from the actor's `evev` texture (camera-space normals) |
| +0x10 | f32 | lighting scale for this material (1.0; 1.05..1.6 on faces and hair) |
| +0x1C | u16 | 1 on a group-4 material: it counts as group 8 |
| +0x24 | f32 | reflection strength (TFACTOR alpha = value x 0.5) |
| +0x28 | f32 | 1.0 = specular from the global "cubemap spec" texture (metal armour) |
| +0x04, +0x08, +0x14, +0x18, +0x20 | | constants, unused |

Groups live on face (hair) materials 1..4 (and 8), hands 5, legs 6, feet 7. A worn mesh hides groups by its header
byte 3 (CODE 0x1002ded4): 0x02 -> 1; 0x03 -> 1, 2; 0x04 -> 1..3; 0x05 -> 1..4, 8; 0x06 -> 1..3, 8; 0x12 (body) -> 5
(glove cuffs); 0x22 (legs) -> 7 (boot tops); 0x32 (feet) -> 6 (trouser bottoms). Triangles under a hidden group are
skipped (CODE 0x1002e5ec). ffxi-linux: `CharacterAsset::hiddenGroups` from all worn meshes, applied at load.

**Lighting scale and reflections** (goal 17, 2026-10-18; drawn by `shaders/skinned.frag`):
- Lighting scale (+0x10): multiplies the 4 lights' diffuse and the ambient colour for the material (CODE 0x1002e02c).
- Both reflections are texture stage 1, MODULATEALPHA_ADDCOLOR (added on top of the lit texture), strength = TFACTOR
  alpha = +0x24 x 0.5 (CODE 0x1002e184 / 0x1002e2ea). The env branch wins; spec is its else (CODE 0x1002e2ca).
- **Environment** (+0x0E): the texture is found by its **chunk name `evev`** (type 0x20) in the actor's own model files
  (CODE 0x100d6cc2; DATA: Hume M heads 7335 / 7336 carry `evev` = `tim     ev`; 303 `evev` chunks in the install, most
  named `tim     ev`, `nom_ev`, `bell_ev2`, `crown_ev`). TEXCOORDINDEX = CAMERASPACENORMAL with the identity matrix
  0x10457108 (set at 0x1002b750): uv = the camera-space normal's x, y, wrapped.
- **Specular** (+0x28 != 0): the global texture is **file 0's chunk `spec`** (`cubemap spec`, a 64x64 DXT3 white
  highlight blob in the middle of the image; file 0 also has `cubemap plight` and `cubemap test32`). The `cubemap `
  name prefix only sets texture flags (CODE 0x1001d870: flags | 5); the texture is a plain 2D image. TEXCOORDINDEX =
  CAMERASPACEREFLECTIONVECTOR through the matrix 0x10457198 = diag(1, -1, -1) x [0x104568fc + 0x294] x the actor's
  light view 0x1045b7c0 (a look-at along the actor's main light, built at 0x100cc2b0; CODE 0x1002b713..0x1002b74b).
- **Stage-1 wiring** (CODE): COLORARG1 is CURRENT from the base setup (0x1002bc8a; the material paths set only op,
  arg 2 = TEXTURE, ALPHAOP ADD, ALPHAARG2 TEXTURE, 0x1002e1cc..0x1002e214), so colour = current + current.a x
  reflection; stage 0's alpha is MODULATE2X(texture alpha, TFACTOR alpha) (0x1002bbb5 / 0x1002bbe3) and TFACTOR alpha is
  the strength (+0x24 x 0.5, 0x1002e184), so the gain is 2 x texture alpha x strength (= the strength at the usual
  alpha 0x80). Stage 1 addresses CLAMP (0x1002bc42 / 0x1002bc5a, ADDRESSU / V = 3), so the env uv is clamped too.
  The spec matrix: diag(1, -1, -1) (0x1002b71d) x the camera block +0x294, the full inverse view with the eye's
  translation (0x100150e0: +0x294 = invert(+0x94), 0x100282f0) x the light look-at 0x1045b7c0 (0x100cc2b0: eye and at
  around the actor, the shadow light view), TTFF COUNT3.
- ffxi-linux: parts split on texture and material; env uses the asset's first `evev` texture; both reflections add
  2 x texture alpha x strength x the sample. Spec: a generated texture coordinate carries w = 1 (wined3d's fixed-function
  code builds `vec4(reflect(..), 1.0)`), so the matrix's translations apply and the coordinate is the light look-at's x, y
  of the point eye + 2 camRight (r . camRight) - r relative to the actor, clamped: the blob lights only when the camera is
  near the light's line through the actor (goal 28; `XI_SPEC_OLD=1` gives the earlier mapping, uv = 0.5 + 0.5 x the
  reflection vector in the light's basis). DATA (Hume M): faces carry light scales 1.05..1.6; bodies
  7380..7445 carry specular (strength 30..128); test "Os2 materials: light scale, env and specular reflections".

**Class 1** (payload[2] & 0x7F == 1; CODE ctor 0x10024f40, fix-up 0x100223a0, simulation 0x100231c0 / 0x10023630):
cloth and hair soft bodies (52 meshes in the install, all `hf_*`, in unique NPC models such as 1366 and 1571, the
Gigas' loincloth; never PC gear). Same section table plus: +0x2a / +0x2c anchored one- / two-bone vertex counts,
+0x2e / +0x32 a spring section ({u16 a, u16 b, f32 rest}, bit 15 = pinned end) in two sets (+0x34 / +0x36 sizes) with
stiffness (+0x38, +0x3c), damping (+0x40), gravity (+0x44), inertia (+0x48), collision sphere ids (+0x4c / +0x50);
the command stream follows at +0x52. Vertices have no normals: one-bone 12 bytes (x, y, z), two-bone 32 bytes
(x1, x2, y1, y2, z1, z2, w1, w2); bone = ref & 0x7F; never mirrored. The game skins the anchored vertices, runs damped
Verlet steps with gravity and wind on the rest, 4 spring passes, sphere collision, and recomputes normals each frame.

The simulation (CODE, traced 2026-10-18; per-vertex state 0x3c bytes: +0x0c position P, +0x18 previous P0, +0x24
velocity, +0x30 normal):
- **Rate:** one step per drawn frame, no time step: the draw (0x10022400) calls 0x100231c0 directly, so the game sways at
  its frame rate (30 fps by default; INFERRED that this is the intended rate).
- **Reset** (the draw context's flag +0x3c): every vertex skinned like an anchor, P0 = P, velocity 0.
- **Anchors:** skinned from the bone matrices each step (one-bone 0x100234f0, two-bone 0x10023550, pre-weighted).
- **Inertia** (0x100231c0): with the actor's motion vector m (the draw call's second argument is the actor's model
  object, actor + 0x674, via 0x100ccceb -> 0x1002b610 -> 0x1002c290 -> 0x1002a2e0 -> the part's vfunc +0x1c; m is its
  +0x34 = actor + 0x6a8, set each frame at 0x100c753b from the actor update 0x100c6300: the last tick's step of the drawn
  position toward its target, (target - position) x 0.125, or zero),
  free particles get P += (0.75 - inertia) m and P0 += 0.75 m: they mostly ride along with the body and lag by `inertia`
  (DATA 0.0 .. 0.4).
- **Step** (0x10023630): V = (P - P0) x damping; P0 = P; P += V; P.y += gravity x 0.1 (y down). Then, unless the draw
  context's +0x17c is set, a wind push of strength scene[0x10629fe4]+0x134 x 0.1 along a vector from the particle's
  normal and the scene wind direction (+0x124) (the exact combination 0x10026ed0 is not decoded).
- **Springs:** 4 passes, set 1 then set 2: d = Pb - Pa, L = |d|; if L > 1e-4, d *= (rest - L) x k / L; Pa -= d unless
  pinned, Pb += d unless pinned.
- **Collision** (0x10023b00): the section-7 ids pick spheres from a table of 0x12-byte records (+0x02 f32 radius, +0x06
  vec3 offset, transformed by a bone matrix; table = resource +0x24 via 0x100351c0, INFERRED the Sk2 trailing block). On
  the first step each spring is tied to the sphere it starts within 1.5 r^2 of; then each spring segment is swept
  against its sphere's motion in radius-sized steps and its free end pushed out (0x10023f50 / 0x10024340).
- **Normals:** recomputed from the triangles every frame (0x10024830).

ffxi-linux (`gfx/cloth.{h,cpp}`, CPU): the same step, springs, gravity and inertia (m = the actor's displacement),
run at a fixed 30 ticks a second and drawn between the last two ticks (so 60 fps shows smooth motion, not 30 fps
steps); a reset on the first frame or a jump of more than 8 yalms. Each particle is one extra palette matrix after the
skeleton's (translation = position, y axis = normal; the cloth vertices carry p0 = 0, n0 = +y), so the skinned shader
draws it unchanged; models whose bones plus particles exceed 256 stay rigid (none in the data: at most 100 + 36).
Wind (CODE 0x100236c7, scene object 0x10629fe4, update 0x10188960, init 0x10186f80): direction +0x124 = game (-1, 0,
0), set only at init; strength +0x134: gusts and lulls alternate on a timer of rand() % 60 ticks (1.2 x as long while
blowing); blowing, it is (+0x138 4 + rand() % +0x13c 10) x 0.0025 every frame; in a lull it falls 0.01 a frame to 0.
Each step a free particle moves by (normal + direction) x strength x 0.1 (direction - normal when that sum is shorter
than 0.1), skipped while the actor's +0x17c is set: the floor under it is a roofed placement (flag bit 2, CODE
0x100a5303 via the ground probe 0x101813e0 / 0x10168e50). ffxi-linux: `gfx::ClothWind` stepped once a frame,
`simulateCloth(..., wind)` with 0 on a roofed floor.
Body collision (CODE 0x10023b00): spheres from the Sk2 after the look limits (0x100351c0 = 0x100351a0(0) + 0x24):
0x12-byte records {s16 bone, f32 radius, f32 offset xyz} until the 0xCD padding (PC skeletons have none; NPC 1366: six,
radius 0.08-0.33); a cloth mesh lists the ones it meets (+0x4c word offset, +0x50 count). On the first step each spring
is tied to the last of its spheres its end a lies within 1.5 r^2 of; then each step, after the springs, every sphere
moves from its last centre (the bone's posed frame x offset, 0x1002b2c0) to now in radius-sized steps, and at each the
tied springs' unpinned ends are pushed out (0x10023f50 -> 0x10024340, codes 1 / 2 / 3 = end a / end b / both;
ffxi-linux takes the code from the closest point of the segment, INFERRED). Test: "Os2 class 1 soft bodies sway,
settle and stay bounded" also checks the wind drift (model 73 -0.051, 271 -0.011) and that tied ends stay out of their
spheres. Test: "Os2 class 1 soft bodies sway, settle and stay bounded" (model
73's 88-particle robe and the Gigas: settle exactly when standing; lag 0.33 / 0.58 yalm walking at 4 yalms / s).

Winding (DATA, 92,000 triangles over races 1/3/8, slots 0–5, normal and mirrored copies): 97% have
`(B−A)×(C−A)` opposite to the summed vertex normals, i.e. **clockwise front faces in game space**, the same as
zone meshes (5.5); strip parity and the mirrored-copy reversal check out (odd strip tris 396 cw / 2 ccw).

### 10.4 Mo2 (type 43) motion — CODE (`CMoMo2_Init` 0x10019660 disassembly) + DATA; evaluation not traced

```
+0x00 u8   version (must be 0, "Mo2 Version Error")
+0x01 u8   0 in files (runtime init flag)
+0x02 s16  element (bone track) count E
+0x04 s16  frame count F (keys per animated channel); the client stores F-1 at init (decrements it)
+0x06 f32  speed (0.25 idl0, 0.5 wlk0/run0 — INFERRED frames per tick)
+0x0A Element[E], 84 bytes = 21 dwords:
      dw0      u32 skeleton bone number
      dw1..4   rotation qx,qy,qz,qw: u32 key index  |  dw5..8   defaults
      dw9..11  translation x,y,z:   u32 key index  |  dw12..14 defaults
      dw15..17 scale x,y,z:         u32 key index  |  dw18..20 defaults
then f32 key data
```
Key index k: 0 = channel constant (use the default dword); otherwise the channel's F floats start at
`payload + 0x0A + 4*k` (CODE: `[obj + k*4 + 0x3a]`; DATA: first track index = 21·E, i.e. right after the element
array; tracks are F floats apart). 0x80000000 on the rotation indices (DATA, e.g. bones 0, 1, 26 in idl0) =
INFERRED "bone not driven by this motion". `Sk2` application (0x10034560): per bone, the motion rotation is combined
with the skeleton's local quaternion (`0x10032a80` quaternion multiply) and the motion translation is added to the
bind translation — operand order unverified.
Hume male base file 7072 holds 130 Mo2 chunks with 4-char names: `idl0` idle (16 tracks × 15 frames), `wlk0` walk
(19 frames), `run0` run (11 frames), `mvb0/mvl0/mvr0` move back/left/right, `jmp0/1`, `ded0/1`, `std0/1`, battle sets
(`bb*`, `bf*`, `gu*`, `sh*`, `sk*`, `ma*`, …) and face `eye3`/`mou4`. One chunk drives a subset of bones (idl0: 16),
so a full pose combines several same-named chunks across the base file and 7073–7076 (INFERRED, not verified).

**Motion application (CODE, 2026-10-16).** `0x10034560` walks the skeleton bones. For each bone it calls the quaternion multiply `0x10032a80`, with `this` = the motion's accumulated quaternion M (array at `0x1045f030`, 52 bytes per bone) and the argument = the bind quaternion B. It stores the result back and adds the bind translation to the motion translation.

`0x10032a80` (assembly ev_ff51a646…) computes the vector part as `M.w·B.v + B.w·M.v + M.v×B.v` and the scalar as `M.w·B.w − M.v·B.v`. That is the Hamilton product **M ⊗ B**. So the posed local transform is:
- rotation = `M ⊗ B` (bind rotation first, then motion);
- translation = `t_bind + t_motion`.

Verified visually in xi-viewer: the idle and run poses are natural.

**Bone scale (CODE 0x10034700, issue #83).** The sampled scale (dw15..20; lerped like the translation when motions
blend, 0x1001a350; reset to 1 each frame) is applied after the matrices: each bone's scale accumulates down the tree
(root: its own; child: parent's accumulated x its own), a child's translation is multiplied by its parent's
accumulated scale, and the bone matrices are built from rotation and translation only (parent's unscaled matrix).
Then (mode 2) every bone matrix is premultiplied by Scale(its accumulated scale), which shrinks the bone's own
vertices without shearing its children. DATA: the Ghrah (file 2460) holds every form's parts in one skeleton and each
form's motions scale the other forms' bones to 1e-5 (`idl0` bones 4..); Hume `idl1` / `wlk1` scale some upper-body
bones to 0.9.

**Motion pieces (DATA):** a motion `xxx` is split by bone group.
- `xxx0` in the race base file drives the root, pelvis and legs (bones 0–2, 25–37 for Hume male).
- `xxx1` in base+1 or base+2 drives the torso, arms and hands (3–24, 48–93).
- `xxx2` in base+3 or base+4 drives bones 38–47.
- The paired files (base+1/+2, +3/+4) are INFERRED to be stance variants. xi-client takes the first one found.

**Playback rate:** frames advance by `speed` per 1/30 s tick (INFERRED).
- run0: 11 frames × 0.5 → 0.73 s per cycle.
- idl0: 2 s.

### 10.5 Validation (DATA, `tools/char_to_obj.py`)
- Hume male, race 1, face 0, starting-style gear `0x1000 0x2008 0x3008 0x4008 0x5008`: files 7072 (Sk2), 7080 face,
  7112 head stub, 7376 body, 7632 hands, 7888 legs, 8144 feet. 94 bones, 11 meshes (6 + 5 mirrored copies),
  1305 vertices, 1882 triangles. Bbox (game, Y down): y −1.787..0.000 (feet exactly on y = 0), z ±0.363 symmetric,
  x −0.15..0.21 (facing +x). Textured render: a correctly assembled, upright, symmetric humanoid with correct textures
  on face, tunic, gloves, trousers and boots.
- Elvaan male, race 3, same look ids: height 2.084, 99 bones, 1385 vertices, 2126 triangles; Elvaan female 1.95;
  Hume female 1.67; Taru 0.94–1.01; Mithra ≈1.69; Galka ≈2.29 — all render as correct humanoids of the right race.
- NPC mode (`char_to_obj.py npc ID`): model ids 1, 100, 300 (files 1301, 1400, 1600) render correctly.
- Weapons (`wep0`) bind to attachment bones at the origin, so in the bind pose they sit at the feet; placing them
  needs the motion data (10.4).

### 10.6 Weapon motion class and PC battle motions: DATA

PC base files (race base + 0..4) hold locomotion, death and corpse motions but no battle stance or swings. Those live
in per-weapon-class files, found on 2026-10-16 by scanning every DAT for Mo2 names:

| Piece | File id | Hume male |
|---|---|---|
| 0 and 1 | race base + 2601 + class | 9673 + class |
| 2 | race base + 2729 + class | 9801 + class |

Class 10 (model 0, no weapon) uses base + 2727 and base + 2855 (9799 and 9927 for Hume male).

Each class file holds these motions:

| Stem | Motion |
|---|---|
| `btl` | battle stance |
| `at0`, `at1`, `at2` | melee swings (H2H also has `at3`/`at4` and `wa4`/`wa5`) |
| `in<x>`, `ot<x>` | draw and sheathe; the letter is a sheath style, not the class, and a file can hold two styles: the weapon's `info` byte 4 (N) picks scheduler `in N` / `outN` of the class file, which plays the stem (CODE 0x100d2e00 / 0x100d2e20; e.g. Galka swords 0 -> `inc`, 1 -> `ind`; a missing style falls back to 0, INFERRED) |
| `mv?`, `wlk`, `run` | battle locomotion |
| `dbm`, `dfm` | damage |
| `ded`, `cor` | death and corpse |
| `pym`, `gdm`, `am?` | not identified |

**The class comes from the weapon model.** Every weapon model file (`slotFileId(race, 6, look)`) has a 16-byte
chunk named `info`, of type 69. Byte 3 of it is the motion class. Byte 6 is in the range 0x70..0x78 and its meaning
is unknown. The other bytes are 0xFF.

The class is a **per-race index**. Model 95 (a great axe) is class 5 on Hume and class 6 on Galka. Correlating Hume
models with LSB `item_equipment.MId` and `item_weapon.skill` gives:

| Class | Hume weapons |
|---|---|
| 0 | axe, club |
| 1 | sword |
| 2 | hand-to-hand |
| 3 | dagger |
| 4 | great sword |
| 5 | great axe, scythe, staff |
| 6 | great katana |
| 7 | katana |
| 8 | polearm |
| 10 | no weapon |

Verification:
- Renders: a Hume sword stance and swing, and a Galka great-axe stance and swing. In both, the weapon moves from the
  back into the hands.
- Hume female's piece-2 files start at 12977 (10248 + 2729) and Galka's at 29081 (26352 + 2729).

Not yet confirmed in code. The client code that turns the class into a file id was not located: the offsets are not
stored as plain immediates or as data near the slot table.

**Casting motions.** These are in the PC base files (pieces 0/1/2 across base+0..+4).

| Stem | School |
|---|---|
| `mw0..2` | white magic |
| `mb0..2` | black magic |
| `mi0..3` | other: ninjutsu, summoning, songs (INFERRED) |
| `ma0..2` | monsters (in their own model files) |

From renders of `mb`:
- `x0` holds the hands gathered at the head; it loops while casting.
- `x1` sweeps the arms out; it is the release.
- `x2` holds the arms spread and returns; it is the recovery.

The client plays `x0` from the 0x028 type 8 cast start until the type 4 finish, then `x1` and `x2`. A type 8 action
whose result has no message is an interrupt (LSB `MagicInterrupt`), and stops the loop. Verified live: BotC
casting Cure.

### 10.7 Confidence and open items
- High: file mapping (CODE tables + DATA), Sk2 bone record and hierarchy (CODE + DATA), Os2 header/sections, vertex
  layouts, command stream sizes, mirror sign table (CODE); skinning rule and bone-ref addressing (DATA, visual).
- Medium: hdr[2] bit 7 meaning and hdr[4] bit 0 = "draw mirrored copy" (DATA, 621 and all checked meshes; not
  located in code); 'C'/'SC' payload layout (sizes from code, fields inferred); Mo2 channel semantics.
- Open: Sk2 trailing block (INFERRED: the class-1 collision spheres); the exact stage-1 wiring of the material reflections (see
  "Lighting scale and reflections" above); the class-1 wind and body collision (the sphere table); Mo2 evaluation
  (interpolation, how the per-bone partial motions are combined, operand order) and the idle/walk composition;
  where the 0x00E SubKind maps to the client's actor model type; the special scale fix in `CMoMo2_Init` for files
  0xC122/0xC40D/0xC129 and NPC kinds 0/1 ids 8..15 and 0x1B4..0x1BB.

### 10.7 Resting motions (CODE, 2026-10-18)

The status motion picker (CODE 0x100c8ae0, byte table 0x100c8da4 / jump table 0x100c8d88 by status - 1) loops
**`rx1`** for status 33 (resting: `/heal`, and the logout / shutdown wait, where LSB's LEAVEGAME applies HEALING),
`si1` for 47 (`/sit`), `cor ` for the dead (2 / 3), `fh1` / `ci1` for others. `rx0` / `rx2` are the kneel down and
stand up. (Earlier read as `sh0..2` by analogy; those are another motion.) Other stems in the same files: `si`, `sk`,
`std`, `gu`/`gh`/`gc`, `mn`, `ri`, `sh`, `sf`, `yu`, `bb`, `bf`, `cm`, `jmp`, `mv?`.

### 10.8 Weapon skill motions and action effect files (CODE + DATA, 2026-10-16)

**Key (CODE):** the file is chosen by the **animation id** of the first result, not by the weapon skill id.
- The animation id is the 12-bit `animation` field of the 0x028 result. The client stores it as u16 at result+0xE.
- For weapon skills, LSB fills it from `weapon_skills.animation` (`char_entity.cpp` `actionResult.animation = PWeaponSkill->getAnimationId()`).
- Example: Fast Blade (id 32) has animation 1; Red Lotus Blade (id 34) has animation 3.

**Action start, `0x100a9160` (CODE).** This runs per 0x028 action.

`0x100a8c20(type)` gives a base effect file per action type (jump table `0x100a8c5c`):

| type | base | used for |
|---|---|---|
| 3, 14, 15 | 3400 | WS finish, dancer, rune fencer |
| 4, 8 | 2800 | magic |
| 5, 9 | 4912 | items |
| 6, 10 | 4412 | job abilities |
| 7, 11 | 3900 | readying, mob skill finish |
| 13 | 49391 | pet skill |
| 12 | 0 | none |

Base adjustments by type:
- Type 7/11, by animation id: ≥ 0x200 → 0xC1EF (49647), ≥ 0x600 → 0xE739 (59193), ≥ 0x800 → 0x14B07 (84743).
- Type 5 with animation ≥ 100 → 0xDCA5 (56485).

The effect file is base + animation, and its scheduler `main` (0x6E69616D) is run. That is the general path, used by NPCs and monsters.

**PC path.** It applies when:
- the type is 3, 14 or 15;
- and the actor passes the class check `0x1002c820(actor, 0x10330ebc)`. INFERRED: this is the PC/humanoid model class.

The client then calls `Actor_LoadSkillMotionFiles` (`0x100d2510`, unnamed in Ghidra). Its arguments are:
- animation id;
- kind = 0 for type 3, 1 for type 14, 2 for type 15.

It loads two race-specific files and returns. The 3400 + animation file is not used for PCs.

```
race r = 0x10084350(actor) (0..8, as in 10.1; >= 9 -> nothing)
v      = (actor+0x881 == 1) ? 0 : 1            // body variant, see below
kind 0 (weapon skill):
  anim <  0x100: fileA = u16[0x1035f4cc][r] + anim     fileB = u16[0x1035f4e0][2r+v] + anim
  anim >= 0x100: fileA = u16[0x1035f494][r] + anim-256 fileB = u16[0x1035f4a8][2r+v] + anim
                 (fileB adds the full anim, not anim-256; CODE, 0x100d2559)
kind 1 (dancer):   fileA = u16[0x1035f504][r] + anim     fileB = u16[0x1035f518][2r+v] + anim
kind 2 (rune fencer): fileA = u32[0x1035f53c][r] + anim  fileB = u32[0x1035f560][2r+v] + anim
fileA = pieces 0/1, loaded via 0x100d4340(file, 1, fid, 6)
fileB = piece 2, loaded via 0x100d4340(file, 2, fid, 6), and only if the skeleton descriptor
        (vtbl+0x3d8)->byte 9 > 0
```

Tables, read from the DLL in race order 0..8 (HumeM, HumeM, HumeF, ElvM, ElvF, TaruM, TaruF, Mithra, Galka):

| table | values |
|---|---|
| `0x1035f4cc` WS A, anim < 256 | 33227 33227 33995 34763 35531 36299 36299 37067 37835 |
| `0x1035f4e0` WS B pairs (v=0, v=1) | (33483,33739) ×2, (34251,34507), (35019,35275), (35787,36043), (36555,36811) ×2, (37323,37579), (38091,38347) |
| `0x1035f494` WS A, anim ≥ 256 | 61451 61451 61499 61547 61595 61643 61643 61691 61739 |
| `0x1035f4a8` WS B, anim ≥ 256 | (61467,61483) ×2, (61515,61531), (61563,61579), (61611,61627), (61659,61675) ×2, (61707,61723), (61755,61771) |
| `0x1035f504` / `0x1035f518` dancer | 58041 58041 58425 58809 59193 59577 59577 59961 60345; pairs from (58169,58297) |
| `0x1035f53c` / `0x1035f560` rune fencer (u32) | 88035 88035 89571 91107 92643 94179 94179 95715 97251; pairs from (88547,89059) |

Each race uses 3 × 256 consecutive files (A, B v0, B v1; HumeM 33227 → next race 33995).

**Body variant v (CODE + DATA).** `actor+0x881` is set from byte 9 of the **body** model's `info` chunk (type 69). Source: `0x100d0b78`, `0x100cf32f`, slot case 2 of the info switch at `0x100d0b71`.
- DATA: Hume and Galka body models have byte 9 = 1 (about 105 models) or 2 (about 195).
- Look 0 (no body) has byte 9 = 0.
- Byte 9 = 1 selects the first entry of each pair (v = 0).
- Piece 2 drives bones 38–47 (10.4). INFERRED: those are the robe/skirt bones, so v picks the cloth variant.
- The same switch selects piece 2 for the stance files: `0x100d2874` `cmp [esi+0x881],1` against tables `0x1035f5d0`/`0x1035f5d2`.

**Motion names come from the WS file's scheduler, not from code (CODE + DATA).** Each fileA holds:
- Generater chunks (type 5);
- Scheduler chunks (type 7): `main`, sometimes `mot0`/`mot1`, `tgt*`, `hit*`;
- the Mo2 pieces 0/1;
- a type-84 chunk named `<stem>a0`. INFERRED: a weapon trail, played by scheduler op 0x2C.

FileB holds only the piece-2 Mo2s.

**Scheduler bytecode, interpreter `0x10057ee0`.** The driver loop is at `0x10057e3d`.

Records start at payload offset `u32[payload+0x14] − 0x10`. Record layout:

| offset | field |
|---|---|
| +0 | u8 op |
| +1 | u8 length in dwords (`>>8 & 0x1F`) |
| +4 | s16 delay in ticks **after** this record, before the next one (post-delay; CODE driver `0x10057d5e..0x10057ed3`; corrected 2026-10-17, see effect-format §S.1) |
| +6 | s16 duration |

Op 0 ends the list.
- **Op 5 = play motion** (`0x1005ca53`). +8 is a 4CC motion name.
  - If the high byte is `?`, the name is a stem: `0x100cd850` plays all pieces of that stem.
  - The Mo2 (type 0x2B) is looked up first in the scheduler's own file (`0x1005e420(.., 0x2b, name & 0xFFFFFF, 0xFFFFFF)`), then in the actor's motions (`0x100ce180`).
  - `ded?` and `cor?` are special-cased.
  - Fields passed to `0x100cd850`: f32 +0x10; u16 +0x18 and u16 +0x1C converted to float; u8 +0x1E & 0x7F; mode nibble +0x1F >> 4. INFERRED: blend-in / blend-out frames and loop flags. Values seen: +0x18 = 20 for the first motion, 10 for the next.
- **Op 3** starts another scheduler by name (`mot0`, `hit1`, `eis1`, `mdam`, `proc`, …).
- **Op 2** starts a generator.

**Which scheduler runs.** The PC wait task (vtable `0x1032e7c0`, tick `0x100a9430`) waits until the files are loaded. It then looks up a scheduler with `0x100ce3b0(actor, 7, name)` and starts it with `0x10056ba0`.
- The name comes from the action record +4. It was not traced, so treat it as INFERRED.
- The NPC path defaults to `main`, and every PC WS file has a `main` that runs the motions directly or through op 3 → `mot0`/`mot1` (DATA, all 256 HumeM files).

**Stems (DATA).** The stems are per file, not per weapon type. Survey of HumeM 33227–33482:
- `b0`–`b4` and `a0`–`a7` families, `ws0`–`ws5`/`wsa`/`wsb`, and 3-digit stems like `010`/`030`.
- A typical WS has three motions: `x0` windup (11 frames), `x1` hold (2 frames) and `x2` strike/recover (45 frames).
- `main` usually plays only `x0?` then `x2?`. Multi-hit weapon skills play one motion per hit: Mithra Combo (37083)
  plays `a20?` t0 dur 30 (wind-up), `a22?` t30 dur 50, `a02?` t45 dur 35, `a52?` t70 dur 60 (the three punches; the
  target flinches, op 0x25, at t40 / t57 / t80). Each op 5 replaces the motion playing, so a motion is cut at the next
  one's tick even when its duration runs longer (DATA, post-delay timing).
- Client (xi-client, #112): `loadWeaponSkillMotions` keeps every op 5 of `main` (and the schedulers it starts with
  op 3) with its tick, duration and blend-in; the animator plays them in order on the scheduler's 60 Hz clock, each
  stretched to fill its duration (effect-format §S.3), holding a motion's last frame if it ends before the next one's
  tick. The blend-in (op 5 +0x18) is used as the cross-fade length in ticks (INFERRED unit). Monster TP moves use
  the same sequencing. Before #112 only the first and last motion played, so Combo showed one punch.
- Animation 0 (file 33227) plays `ma0?` from the actor's own set.

**Worked example (CODE tables + DATA): Galka (race 8), Fast Blade (WS 32, animation 1).**
- fileA = 37835 + 1 = **37836**. It holds `b000 b001 b010 b011 b020 b021` (+ `b0a0` type 84, schedulers `main`/`hit1`/`hit2`).
- fileB = **38092** if the body info byte 9 == 1, else **38348**. Each holds `b002 b012 b022`.
- `main` timeline in ticks:

| t | op |
|---|---|
| 10 | op 5 `b00?` (dur 35) |
| 35 | op 3 `eis1` |
| 35 | op 5 `b02?` (dur 70) |
| 55 | op 0x2C `b0a?` |
| 60 | `hit1` |
| 80 | `hit2` |
| 100 | `mdam` |
| 152 | `proc` |

So the motions played are `b000`+`b001`+`b002`, then `b020`+`b021`+`b022`. `b01?` is present but unused by `main`.

Red Lotus Blade (animation 3):
- fileA = **37838**, with the same `b0` motions (identical sizes) and different generators;
- fileB = 38094 or 38350;
- `main`: `b00?` at t = 10, `b02?` at t = 35.

Hume male Fast Blade = 33228 and 33484/33740.

**Readying, type 7 (CODE + DATA).** LSB sends animation 0 with action id `'cate'`. File 3900 + 0 has a `main` with no op 5 (it runs `stnm`, `mdam`), so there is no PC motion for readying.

**Monster TP moves, type 11 (CODE, motions DATA only).**
- The file is 3900 + `mob_skills.mob_anim_id`, with the high ranges above.
- Its `main`/`cst0` scheduler plays motions such as:
  - `ma0?`, `at1?`, `cr0?`, which come from the monster's own model;
  - or full names like `wz20`, kept in the effect file itself.
- Example: 3901, animation 1 (LSB Foot Kick), plays `wz20`.
- Not verified visually.

**Related lead (CODE, not reconciled).** `0x100d2d60(race, kind)` returns `u16 T[race] + class`, where class = `actor+0x87b` (main weapon info byte 3), or `+0x88a` for kind 2.
- T = `0x1035d764` (9672 for HumeM) for kind 0, `0x1035d79c` (40431) for kind 2, `0x1035d7d4` (40815) for kind 1.
- Piece-2 pairs are at `0x1035d778` (9800, 9928), `0x1035d7b0` and `0x1035d7e8`.
- This is the 10.6 battle-class mapping. Note the base is 9672 + class, while the DATA-derived formula in 10.6 says 9673 + class; the off-by-one was not resolved.

### 10.x Child races (CODE + DATA, 2026-10-18; RE notes npc_race_re.md)

The per-race tables (base 0x1035f420, slot ranges 0x1035f388) run to race 36; the look builder accepts races 0..8 and
29..36 and turns any other into 32 (CODE 0x100d2ede). Race 0 is Hume male. Races 29..31 are children on a `kids`
skeleton with one-piece motions and no weapons (CODE 0x100d3b33): 29 Mithra child (base 30696; face / head / body /
hands / legs / feet ranges of 64 from 30376 / 30568 / 30312 / 30504 / 30632 / 30440), 30 Hume / Elvaan girl (29912;
29592 / 29784 / 29528 / 29720 / 29848 / 29656), 31 boy (30304; 29984 / 30176 / 29920 / 30112 / 30240 / 30048). Elvaan
children use indices 20 and up (`em_*` files; INFERRED from names). Races 32..36 are chocobos (bases 55295 ... 55431).
Live: Southern San d'Oria's children (Leuveret, race 31) draw.

### 10.y Chocobos and riding (CODE + DATA, 2026-10-18; RE notes chocobo_re.md)

- **Races 32..36** are the yellow / black / blue / red / green chocobos: bases 55295 / 55329 / 55363 / 55397 / 55431
  (`choc` skeleton, 58 bones; one-piece motions `chi0` idle, `wlk0`, `run0`, `mvb0`, `chh0` mount, `ch10` dismount,
  `dam0`; no `idl0`). One range per slot, no face or weapons: head 8, body 8 (index 1 has no saddle), hands 4 (the
  tail), legs 4 (index 0 an empty file, 1 the Noble chocobo's armour), feet 8 (yellow: 55313 / 55297 / 55321 / 55325 /
  55305; the others +34 per colour). A look index past the ranges falls back to index 0 (all races).
- **NPCs:** S2C 0x00E look kind 7: the race is word 0's **low** byte (CODE 0x1009cd80), then the slot words as for a PC.

**S2C 0x00E look kinds** (CODE 0x1009c837: a switch on kind & 7, table 0x1009cdb8). Kind 0 (0x1009c8cc): actor type 2
(+0xee), the model id in look slot 0 (+0xfc). Kind 6 (0x1009ccd2): actor type 7, slot 0 cleared and the word in slot 9
(+0x10e) = id | 0x9000; ids 0x7B8..0x7CB and 0x7D3..0x7DA also set +0xf6 = 2 (meaning not traced). An actor of type 7
loads slot 9's low 12 bits as a standard NPC model (0x100d0ce8 -> 0x100c50f0(0, id & 0xFFF), the 1300 / 51795 / 99907 /
101739 ranges). LSB sends Ghrah (Awghrah, model 0x48C = file 2464) and automatons with kind 6. xi-client draws the NPC
model; INFERRED: what else type 7 composes on top of it (an automaton's head / frame) is not done.
- **Riders:** status 5 (chocobo) / 85 (mount). 0x00D +0x20 bits 5..7 ChocoboIndex (1 rented, 2..4 own / Noble),
  +0x34 / +0x38 the own chocobo's looks, +0x44 >> 4 the mount; own: 0x037 +0x28 bits 13..15, +0x5B. The chocobo is a
  separate actor at the rider's position and facing (no offset): rented 39275 (39276 in the [S] zones 80..99 / 136 /
  137), own `race 32 + (p >> 9 & 7), head p & 7, body 7, tail p >> 6 & 7, feet p >> 3 & 7` (Noble: bit 0 of the second
  word: 32 / 3 / 3 / 3 / legs 1 / 3), mounts 102705 + index - 1. The rider plays `chi` / `wlk` / `run` from the race's
  riding file (38603, 38603, 38699, 38795, 38891, 38987, 38987, 39083, 39179 for races 0..8; + 3 in the [S] zones,
  + 5 on a mount; piece 2 from a second file by the body variant). The motions have the chocobo's frame counts, which
  lifts the hips onto the saddle. Live: `!mount 0` (BotC, gmlevel 1) draws the chocobo and the seated rider.
- **Own chocobo's looks** for ourselves come in S2C 0x067 (char sync, kind 2): +0x18 / +0x1C = the two CustomProperties
  words (LSB char_sync.cpp); others' in 0x00D +0x34 / +0x38.
- **Mount / dismount** (schedulers `chor` / `coff`, CODE 0x1008e6b9 / 0x1008ba8b): mounting, both play `chh`; getting
  down, the rider plays `ch1` then `ch2`, the chocobo `ch1` then `run` (it runs off). Mount riding files (+5) and the
  mounts have no `ch1`: the rider just stands and the mount runs off at once. The leaving is the mount file's own
  `coff` scheduler (post-delay records, 60 ticks a second): 39275 `ch1?` (gap 100), `run?` (gap 2), op 0x6E 12 yalms
  a second for 72 ticks (gap 48), op 0x29 to alpha 0 over 24 ticks: run from 1.67 s, moving 1.70..2.90 s, faded
  2.50..2.90 s; mounts 102705.. `run?` at once, 10 a second, faded by about 1.2 s (102707: over 12 ticks).
  `dat::leavePlan` reads it (test "effects: a dismounted chocobo / mount leaves as its coff scheduler paces it");
  live 2026-10-20 (BotD, `!mount 0` / `!mount 3` then `/dismount`): the chocobo ducks, then runs off and is gone by
  3 s; the tiger runs off at once and is gone by 2 s.
- **Weapons while riding** (CODE 0x1008c271 / 0x1008c7a5): all weapon slots shown, then slot 2 (ranged) hidden.
- Live (2026-10-18): `!chocobo green head tail feet`, `!chocobo black`, `!mount 34` (Noble), `!mount 3 / 10 / 12`
  (tiger, Fenrir, moogle) on BotC; BotB's `!chocobo red head tail` seen from BotC; `/dismount` from a
  chocobo and from a mount.

## 11. Game text: battle messages and name tables (DATA, 2026-10-16)

I found these by searching every DAT for known strings, in plain text and in the two encodings below. Code is in
`src/dat/messages.*`; there is a test in `tests/test_zone.cpp`.

| File | Format | Contents |
|---|---|---|
| 7027 | dialog | basic battle messages, indexed by MsgBasic id (1024 entries) |
| 55701 | `d_msg`, fixed 80-byte entries | weapon skills (index = WS id; Combo = 1, Raging Axe = 64), job abilities from 512 |
| 55702 | `d_msg`, fixed 140-byte entries | spells (index = spell id; Cure = 1) |
| 55732 | `d_msg`, indexed, bit-inverted | status effects, two fields each: noun, adjective ("paralysis", "paralyzed") |
| 55650 | `d_msg`, inverted | UI/config strings (not battle text) |


### 10.8b Engaged movement (DATA, 2026-10-17)

The weapon class files (9672 + class for Hume male, §10.6) hold `run1`, `wlk1`, `amf1`, `amb1`, `aml1` and `amr1`:
upper-body pieces only. xi-client builds the engaged walk / run from the race's walk / run with those pieces in place
of the base ones (dat::CharacterMotions::battleWalk / battleRun). `amb1`, `aml1` and `amr1` (INFERRED: the engaged move
back / left / right) replace piece 1 of the race's `mvb` / `mvl` / `mvr` (legs). While engaged, xi-client keeps the
character facing its target and picks forward / back / left / right from the movement relative to that facing. This
was only lightly checked: BotA sidestepped with the sword drawn.

### 10.9 Emote motions (DATA, 2026-10-17; stem mapping INFERRED)

- **Files:** for every race, emote motions live in **race base file + 2984 + group** (pieces 0 and 1) and **+ 64**
  (piece 2). A second piece-2 copy sits at + 128, like the stance variants. The layout is identical for races 1–8
  (Tarutaru M/F share base 19776). For Hume male, files 10056.. hold:

  | Group | Hume male file | Stems |
  |---|---|---|
  | 0 | 10056 | bow kne lau poi sl1 sl2 sl3 wee |
  | 1 | 10057 | che clp den gla nod wav wel |
  | 2 | 10058 | clp cmf pok sig sta sur |
  | 3 | 10059 | ang blu nod pan ups why |
  | 4 | 10060 | fum gut rs0 thk |
  | 5 | 10061 | def |
  | 6 | 10062 | dig |
  | 7 | 10063 | mow |
  | 8 / 9 | | xe? / ye? |
  | 10 | | rs0 |
  | 11 | | tos |
  | 12 | | dc0..dc3 |
  | 13–16 | | bel |
  | 17.. | | jb? (job emotes), ce?, mq? |

- **Groups follow LSB's emote ids** (DATA): ids 0–5 are group 0, then eight ids per group (6–13, 14–21, 22–29, 30–37).
  - Gathering: Excavation 41 → dig (group 6), Harvesting 42 → mow (group 7).
  - Hurray 43 → rs0, Toss 44 → tos.
  - Dance1–4 (65–68) → dc0–dc3.
- **The stem within a group is matched by name (INFERRED).** For example, Cry → wee (weep), No → den (deny),
  Joy → gla (glad), Doze → nod in group 3. The renders look right for bow, kneel, cry, no, wave, cheer, clap, comfort,
  angry, think, toss and dance 1.
- **Unmapped emotes:** ups, why, gut, def, xe, ye, bel and the job emotes. Smile, Slap, Amazed, Stare, Muted, Grin and
  Doubt have no matched stem; some are probably face-only.
- The client's own emote → motion table was not found: the stems are not stored as strings in FFXiMain.dll or in any
  scheduler chunk checked.
- `xi-viewer --motion emote:N char ...` previews one; `dat::loadEmoteMotion` loads it.
### 11.1 `d_msg` (string tables)

The header is 0x40 bytes:

| Offset | Field |
|---|---|
| +0x00 | `"d_msg\0\0\0"` |
| +0x0A | u16; 1 = the body is bit-inverted (~b) |
| +0x14 | file size |
| +0x18 | header size (0x40) |
| +0x1C | index size |
| +0x20 | entry size |
| +0x24 | data size |
| +0x28 | entry count |

There are two layouts:
- **Indexed** (index size > 0): the index is (offset, length) u32 pairs, and offsets are relative to the end of the
  index.
- **Fixed** (index size = 0): entries of `entry size` bytes.

An entry starts with a u32 field count, followed by (u32 offset, u32 type) per field. A type-0 field is a string
stored 28 bytes after its offset.

### 11.2 Dialog format (also used by zone NPC text)

The file starts with a u32 size, ORed with 0x10000000. Everything after it is XOR 0x80. It begins with a table of u32
message offsets, relative to the end of the size word; the first offset is also the table's byte length. The
messages follow back to back.

### 11.3 Control codes in battle messages

| Code | Meaning |
|---|---|
| `01 01 10` / `01 01 11` | actor / target name |
| `01 01 01` | item count (not shown) |
| `01 05 T 82 (0x80+k) 80 80` | name from table T for number slot k: T = 0x13 status noun, 0x14 status adjective, 0x26-0x28 items, 0x2A/0x42 key items |
| `05 k` | name from file 7021 (dialog format): skills 1-57, synthesis tiers 59-63, check ratings 64-71 ("like easy prey") |
| `10 k` | spell name |
| `12 k` | number |
| `16 k` | weapon skill (k < 256: file 55701) or monster TP move (k ≥ 256: file 7035 entry k − 256, e.g. 257 Foot Kick, 334 Sharp Sting; 55701 holds only "." at 256-511). The k ≥ 256 rule is from the data (INFERRED: the battle formatter's own branch is not traced) |
| `07` | line break |
| `1F c` | colour |
| `7F 31 00` | end of message |
| `7F 84 00` | indent |
| `7F 86 k [a/b]` | plural by number slot k |
| `7F 87 k [a/b]` | verb agreement; singular is the first option |
| `7F 88 k [The /]` | article; nothing for players. k = 0 actor, 1 target |
| `7F 8F k` | job ability name (abilities table + 512) |
| `7F 90` / `7F 91 [his/her]` | pronoun |
| `7F 9B k` | tenths ("0.2") |
| `7F 97` / `7F B0` / `7F B1` / `7F B4` / `7F B7` with k | numeric fields |

**Number slots.** Message 2 puts a spell name in slot 0. Messages 1, 67 and 185 put damage in slot 1. Messages 161
and 163 put additional-effect values in slot 2. Messages 14, 33 and 44 put counter and spikes values in slot 3.
Filling them:
- For S2C 0x028: slot 0 = action id, slot 1 = result param, slot 2 = add-effect param, slot 3 = spikes param.
- For 0x029 / 0x02D: slot 0 = Data, slot 1 = Data2.

Verified live: hits, misses, skill-ups and defeats print the same text as the retail client.

### 11.4 Zone dialog (NPC text)

File **6420 + zone** holds the NPC text for zones below 256 (zone 230 → 6650, 16,940 messages). It uses the same
dialog format as §11.2, but its own codes:

| Code | Meaning |
|---|---|
| `0A k` | number k |
| `0C k [a/b/c…]` | option chosen by number k |
| `0B` | menu: the following lines are options |
| `7F 92 k [/s]` | plural |
| `7F 85 [sir/ma'am]` | player gender |
| `01 05 T 82 (0x80+k) 80 80` | item / key-item names (not loaded yet) |
| `7F 34` / `35` / `36` / `38` x | pauses and prompts |
| `7F A0` .. `AC` | dates and times |
| `1F c` | colour |
| `EF x` | element icons |

The numbers come from S2C 0x02A `num[4]`. S2C 0x036 carries none; there, bit 15 of `MesNum` means "do not show the
speaker's name".

Zones 256 and above use another range, not mapped yet (the same open question as the entity-name files).

## 12. Items: names and equipment data (DATA, 2026-10-16)

Item records are **0xC00 bytes**, and every byte is rotated left by 3 bits (decode: `(b >> 5) | (b << 3)`). Each
file holds one contiguous id range, starting at the id of its first record (the record's u32 at +0). These were found
by searching for a rotated item name; code is in `src/dat/items.*`, with a test in `tests/test_zone.cpp`.

| File | First id | Records |
|---|---|---|
| 73 | 0 | 4096 |
| 74 | 4096 | 4096 |
| 75 | 16384 (weapons) | 6656 |
| 76 | 10240 (armour) | 6144 |
| 77 | 8192 | 512 |
| 55667 | 28672 | 1024 |
| 55668 | 23040 | 5632 |
| 55669 | 61440 | 512 |
| 55670 | 29696 | 1024 |
| 55671 | 8704 | 1536 |
| 55672 | 57344 | 4088 |
| 55673 | 61952 | 1024 |

Record fields:

| Offset | Field | Applies to |
|---|---|---|
| +0x00 | u32 item id | all |
| +0x08 | u16 type: 4 weapon, 5 armour, 7 usable, 1 general, 8 crystal | all |
| +0x0E | u16 level | weapons and armour |
| +0x10 | u16 equip-slot mask: bit n = slot n (0 main, 1 sub, 2 ranged, 3 ammo, 4 head, 5 body, 6 hands, 7 legs, 8 feet, 9 neck, 10 waist, 11/12 ears, 13/14 rings, 15 back) | weapons and armour |
| +0x12 | u16 races | weapons and armour |
| +0x14 | u32 jobs | weapons and armour |

**Names.** After a type-specific header comes a `d_msg`-style string block: u32 field count n, then (u32 offset, u32
type) pairs, the first offset being 4 + 8n. The block was found at +0x18 for general items, +0x1C for crystals, +0x2C
for armour and +0x38 for weapons. Strings sit 28 bytes after their offset. The fields are: name, a flag, the log name
in singular and plural, and the description. Battle messages use the log names: table 0x26 is the name, 0x27 the log
name singular, 0x28 the log name plural, and `01 01 01` is the article ("a" / "an") before it.

The weapon-model `info` chunk is a different thing; see §10.6.

## 13. Spell data, file 86 (DATA, 2026-10-16)

The file is 0xC0000 bytes: 768 records of **0x400 bytes**, where record n is spell id n. Every byte of a record is
rotated left by r bits, and r differs per record (Cure uses 2, Cure II uses 6). The decoder picks the r that makes the
record's u16 at +0 equal its index, with every job level either 0xFF or 99 or lower. That rule is INFERRED; the
client's own decoder was not traced. Code is in `src/dat/spells.*`.

| Offset | Field | Notes |
|---|---|---|
| +0x00 | u16 id | |
| +0x02 | u16 magic type | 1 white, 2 black, 3 summoning, 4 ninjutsu, 5 song, 6 blue, ... |
| +0x04 | u16 element | 0 fire .. 7 dark |
| +0x06 | u16 valid targets | 0x01 self, 0x02 player, 0x04 party, 0x08 ally, 0x10 NPC, 0x20 enemy |
| +0x08 | u16 skill | 33 healing, 36 elemental, ... |
| +0x0A | u16 MP cost | |
| +0x0C | u8 cast time | quarter seconds |
| +0x0D | u8 recast | quarter seconds |
| +0x0E | u8 levels[24] | indexed by job id; 0xFF = cannot learn |

Example: Cure decodes as id 1, white magic, light, healing magic, 8 MP, 2 s cast, 5 s recast, WHM 1 / RDM 3 / PLD 5.

**Checked against LSB `spell_list.jobs`.** All 767 records decode. 421 of 665 match exactly, not counting SCH, which
LSB grants through Addendum. The rest are high-level entries where the 2026 retail data and LSB's era settings differ
(Cure VI, Diaga), so the server stays authoritative and the client uses the table only to filter its spell list.

### 13.1 Ability data, file 85

The file has 1024 records of **0x400 bytes**, indexed by job-ability id (Provoke = 35, Mighty Strikes = 16). Each
record is rotated left by a per-record r, as in file 86. Some ids read the same under two rotations (34 = 0x22), so
the decoder also requires the name at +0x0A to decode as printable text.

| Offset | Field | Notes |
|---|---|---|
| +0x00 | u16 id | |
| +0x02 | u8 type | |
| +0x03 | u8 list icon | |
| +0x04 | u16 MP | |
| +0x06 | u16 recast group | see below |
| +0x08 | u16 valid targets | Provoke 0x20, self abilities 0x01 |
| +0x0A | char name[32] | |
| +0x2A | description | |

**The recast group does not match LSB for most abilities.** It matches LSB `abilities.recastId` for 238 of 616 (for
example Provoke 5 and two-hour abilities 0). It differs by small offsets for the rest: Steal is 61 here and 60 in
LSB, and Fight/Heel/Leave/Sic are 101-104 here and 100-102 in LSB. Since S2C 0x119 reports LSB's ids, the client
learns each ability's group from the first timer that starts after it uses the ability, and uses +0x06 only until
then.

## 14. Interface: sprite sheets, window layouts, fonts (CODE + DATA, 2026-10-16)

Full byte layouts with code addresses: `docs/ui-format-notes.md`. Summary:

- **Files.** The client picks interface files per language from tables in the DLL (around 0x61788..0x61ef8 in the
  file image, one table per language). English: **39542** (`ROM/119/51.DAT`, in-game menus: 61 textures, 404 layouts,
  41 sprite sheets) and **39541** (`ROM/119/50.DAT`, title and lobby). The same table lists 73..77 / 55667.. (items),
  55701 / 55702 (ability and spell names) and 7027 (battle messages), which confirms it is the English one. Window skins
  ("Window type" 1..8) are files **14..21** (`ROM/0/14..21.DAT`): textures `newtex`, `hfr1`, `corner`, `vfr1` and a sheet
  `menu win0N`.
- **Sprite sheet (type 0x31, "Msb").** `name[16]; u8 nTex; tex[nTex][16]; u16 nSprites;` then per sprite `u8 nQuads`
  and 61-byte quads: `s16 xy[4][2]` (TL, TR, BL, BR, offsets from the sprite origin), `u16 w, h, u, v` (texel rect),
  `u8 flags` (bit 0 mirror U, bit 1 mirror V), `u8 rgba[4][4]`, `u8 blend[4]`, `char texture[16]`. The quad's own texture
  name is what is drawn; the header list is not used. Every sheet in the English and skin files parses exactly.
- **Colour.** PS2 convention: vertex colour 0x80 = 1.0. On load R, G, B get +1 (unless 0xFF) and A becomes
  `min((A + 1) * 1.5, 255)`. Colour and alpha stages are MODULATE2X, so the output is `clamp(2 * vtx * tex)`.
- **Blend** `{enable, A, B, C}` is a GS-style `(A - B) * C + Cd` with 0 = source, 1 = frame, 2 = zero. Data uses
  `01 00 01 01` (normal alpha), `01 00 02 01` (additive, SRCALPHA/ONE), `01 02 00 01` (Cd - Cs*As, the darkened title
  strips) and `00 ..` (opaque).
- **Layout (type 0x30, "Meb").** A 0x20 header (`name[16]`, u8 1, u8 nElements), then a window record and nElements
  element records, each self-sized by its leading `u16 length`.
  - Window: x, y in a 640x480 design space; cursor offset; frame w, h and offset; anchor (bit 0 right, bit 1 bottom);
    then 20-byte subs `{u16 type, u16 sprite, char sheet[16]}` and two decimal strings (text ids).
  - Element: x, y relative to the window, a hit rect, a 1-based id, and six navigation bytes (prev, next, up, down,
    right, left; 0xFF = not selectable).
  - Sub type 0 is always drawn. The sub whose type matches the element state replaces it (type 4 = disabled).
    Window sub type 6 is the pointing hand; sprite 0 there means it is animated, stepping every 3 ticks.
  - Sheet names "menu frames" and "menu key1top" are language aliases ("menu framesus", "menu key1topu" in English).
- **Placement.** Anchoring treats the canvas as 512x448: a right-anchored window is at `x + W - 512`, a
  bottom-anchored one at `y + H - 448`. The chat log ignores its layout position; the client places it.
- **Window frame.** A nine-slice from the skin sheet:
  - sprite 0 (or 9) is the background, W x H, tiled;
  - sprites 2 and 7 are the top and bottom edges, (W-48) x 5;
  - sprites 4 and 5 are the left and right edges, 5 x (H-48);
  - sprites 1, 3, 6 and 8 are the 24 x 24 corners.
- **Main menu.** Element labels are sprites of "menu windowps": 13 Status, 15 Equipment, 17 Magic, 19 Items, 21 Abilities.
- **Equipment grid.** "menu equip" boxes use framesus sprites 40..55 in grid order: Main Sub Range Ammo / Head Neck Ear Ear /
  Body Hands Ring Ring / Back Waist Legs Feet.
- **Text font.** `font moji` (1024x2048) is a grid of 16x16 cells, 64 per row. Cell 0 is ' ' and ASCII follows in order,
  then Shift-JIS (lead bytes 0x81-0x9F, 0xE0-0xEF, 0xFA-0xFC; pairs mapped by 0x1011c870). Advance widths: a u8 table
  of 0x440 cells at VA 0x1036c438, which is in the DLL's unpacked data section. The client reads it from the user's
  FFXiMain.dll by fingerprint (`dat/dll_data.h`); measured ink widths are the fallback.
- **Menu font.** `font fontshp`: glyph = sprite (c - 0x20), advance = quad width - 1. The party and target windows use it.
- **Colours.** On load, alpha 0 and 0xFF stay as they are; other values become (A+1)*1.5. A caller's colour multiplies
  per channel, `min(255, c*m >> 7)`, never replaces. A window's own sprite is drawn from quad 1; quad 0 is its
  background, which the skin frame replaces.
- **Party window** (`menu ptw1..6` by party size). Member i is drawn in element i's rectangle, with offsets from its
  corner:
  - name at (0,0), 7 characters plus an ellipsis (sprite 108) when longer than 8;
  - HP gauge at (26,7): windowps sprites 97 (fill) and 38 (frame);
  - MP gauge at (49,17): sprites 98 and 39;
  - HP right-aligned at (88,1), coloured by `menu hpcol` 0..3 at 75/50/25%;
  - MP right-aligned at (98,10);
  - TP right-aligned at (48,10), grey, or blue at 1000 or more.
- **Gauges.** The fill is scaled by the value. A "ghost" at alpha 0x30 covers the gap between the shown value and the
  real one, and the shown value eases 15% per frame. The frame is dimmed when the value is 0.
- **Target window.** The name sits at (5,12), or on two lines at (5,6) and (5,18) when wider than w-13. The HP gauge is at (28,29).
- **Help text.** A layout element's first string id indexes file 55651 (help lines, e.g. 321 "View status."). The second
  indexes 55652 (short titles). Both are `d_msg` files (§11.1). Shown in `menu helpwind`, which scrolls long lines.
  Spell and ability descriptions are 55734 / 55733, indexed like the names in 55702 / 55701. Area names by zone id:
  55465 (full, "Southern San d'Oria") and 55661 (short, "S.San d'Oria"; the party list shows members elsewhere this way).
- **Zone maps.** These come from a table of 14-byte records in FFXiMain.dll (VA 0x103821f0 in the reference build). The
  client reads it from the user's DLL by fingerprint (its first 16 records; `dat/zone_map.h`). The table ends at the
  first record whose scale is 0; there are 829 records. Record layout:
  - +0 s16 zone
  - +2 s8 map id: the collision placement's map id (§7.4)
  - +3 position in the zone's map list
  - +4 low nibble: texture bank
  - +5 s8 scale
  - +8 s16 file
  - +10/+12 s16 offX/offY

  The texture file is 5312 / 53295 / 54295 + file for banks 0 / 1 / 2, else 5522 (the "NO MAP" page). Map pixel of a
  world (x, z) is `trunc(x*k + 0.5) - offX`, `-trunc(z*k + 0.5) - offY`, with `k = |scale|/2560*512` (CODE 0x101f7120).
  Grid label: column 'A' + (u-16)/32, row 1 + (v-16)/32. Textures are 512x512 with alpha 0x80 = opaque; most are
  8 bpp (0xB1) and stored bottom-up like every non-DXT texture (§4 orientation, #79). Many of them
  are not named "menumap", so use the table, not names. The player marker is `menu keytops3` sprite 159, rotated to
  the heading. Details: `docs/ui-format-notes.md` §6.
- **Map key item.** The 14-byte map records (DLL 0x103821f0) carry a u16 at +6: the area's **map key item = 384 + value**. Value 0 means no key item is needed (174 of 829 records); 255 appears only for zone 44. Checked against LSB's MAP_OF_* ids: - 1 → 385 San d'Oria area (Ronfaure and both cities); - 2 Bastok, 3 Windurst, 4 Jeuno, 5 Qufim, 7 King Ranperre's Tomb, 8 Dangruf Wadi, 10 Bostaunieux Oubliette,  11 Zeruhn Mines. Every non-zero value except 255 lands on a MAP_OF_* key item. xi-client hides the map unless that key item is held (S2C 0x055).

- **Status icons.** File 87 (`ROM/119/57.DAT`) holds 0x1800-byte records indexed by icon id. Each record has its icon at
  +0x280, as in item records (32x32). The HUD strip is `menu buff`: 24x24 slots, nine per row, at (142,48).
- **Item icons.** In the item record (§12) at +0x280: `u32 size` then a texture payload as in chunk type 0x20
  (32x32, 8-bit paletted). Palette alpha is 0 or 0x80 (opaque), with a few antialiased edge values in between; never
  above 0x80. The game's icon sprite (CODE 0x100268c0, scaled 0x10026a00; item icons via 0x1015b4b0) turns
  ALPHABLENDENABLE on (with BLENDOP ADD) only when its caller passes the flag, and the item grids pass 0, so icons are
  drawn with blending off and alpha-tested. Under MODULATE2X with vertex alpha 0x80 a blended icon would be half
  see-through (issue #90). INFERRED: the inherited render state is blending off. The scaled draw sets linear filtering
  and ALPHAREF 0x60 for the draw, then back to point / 0.

## Open questions
- Meaning of the flag bits 26..31 (Weather = 4 always), and of the texture flags' low nibble.
- Roles of Meb (48), Msb (49), Vtx (62), Cib (69), Spw (88) and Max (94). Check their slot-8 functions:

  | Type | Slot-8 function |
  |---|---|
  | Meb | 0x10116ed0 |
  | Msb | 0x1011deb0 |
  | Vtx | 0x10063b80 |
  | Cib | 0x100718f0 |
  | Spw | 0x100379e0 |
  | Max | 0x10038850 |

  Many of these addresses have no Ghidra function; read them in the disassembly.
- ~~Whether collision lives in Mzb~~ — yes (CODE, 5.4: Mzb header +0x08 → 0x10167e60). Its full layout and the raycast consumers are still open.
- Geometry-specific open items: see 5.7.
- Why the factory rewrites the type of 0x20 objects to 0x1B before the load, which then overwrites it again (`case 0x20` in 0x100711a0).
- 0x10178d00 also has a special case for zone 0x95 (149), which sets camera floats.

## Next steps (resume here)
1. Q1: Apply the names in the table above with `set_addresses_names` and add evidence comments. Then fill in the "?" roles using the slot-8 addresses listed. Vtables for classes outside the table: 5 → 0x1032b114, 0x1c → 0x103366c4, 0x2e → 0x10336698, 0x2f → 0x103367e8, 0x36 → 0x103366f0. Slot 8 is at vtable+0x20.
2. Q2: Done, apart from the meaning of the flag bits. Grep the disassembly for `shr reg,0x1a` (33 sites) to see who reads bits 26+.
3. Q3: Done for the model DAT. Optionally confirm what the caller of 0x10178d00 passes (zone id from the zone-in packet) and look at `[mgr+0x39498]`.
4. Q4: Done for DXT and 8 bpp paletted. Still to do: decode one texture end to end, for example DXT3 "arch" in file 330 at file offset 0x2cb010 (data starts at chunk+16+0x45), and check the BGRA order of paletted textures visually.

5. Zone geometry (sec. 5): decode + placement done and validated. Next: port `tools/zone_to_obj.py` to C++
   (decode in place, build one VB/IB per piece, bind texture by the 16-byte name, apply W per placement, flip y for a
   right-handed Y-up renderer); then type-2 morph animation, alpha/blend from piece flag bits, and the collision block.

Scratch tools used: Python chunk walker and VTABLE/FTABLE resolver (`xi.py` in the session scratchpad, lost on reboot; the logic is fully described above).
