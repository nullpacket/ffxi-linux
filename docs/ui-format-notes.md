# FFXI UI data formats: Meb (0x30) layouts and Msb (0x31) shape sheets

Reverse-engineered from `FFXiMain.unpacked.dll` (image base 0x10000000) and checked
against ROM/119/51.DAT, ROM/119/50.DAT and ROM/0/14..21.DAT.

Tags: **CODE** means read from the disassembly or decompilation at the cited address.
**DATA** means checked by parsing the real files. **INFERRED** means a reasonable reading
that the code does not prove.

Both payloads start right after the 16-byte DAT chunk header. The registration slots
(Meb 0x10116ed0, Msb 0x1011deb0) pass `chunkObj+0x30` to the managers.
All integers are little-endian.

Validation (DATA): with the layouts below, all 460 Meb chunks in 50/51.DAT and all 50 Msb
sheets (26,695 quads) in 50/51.DAT and ROM/0/14..21 parse to the exact end of their chunks.

---

## 1. Msb: shape sheet (chunk type 0x31)

Parser: 0x101201c0 creates the 0x40 sheet object, then 0x1011dfe0 reads the header,
0x1011e290 reads each shape, 0x1011e410 reads each quad and 0x1011e600 computes UVs.
Lookup by name: 0x10120380 (manager 0x104e1bf8) does a 16-byte memcmp against
sheet+0x24 and returns `&sheet->shapes` (sheet+0x20). Shape n is `shapes[n]`.

### File layout (CODE 0x1011dfe0 / 0x1011e290 / 0x1011e410, DATA-validated)

```
SheetHeader:
  char  name[16];            // e.g. "menu    windowps"          -> sheet+0x24
  u8    nTex;
  char  texNames[nTex][16];  // copied into a list of 0x1c objects; NOT used when drawing
  u16   nShapes;             // -> sheet+0x3c
  Shape shapes[nShapes];     // a shape that fails to parse is stored as NULL

Shape:
  u8    nQuads;
  Quad  quads[nQuads];

Quad (61 bytes, packed):
  +0x00 s16 x0,y0, x1,y1, x2,y2, x3,y3   // vertex i = (x_i, y_i). Order is TL, TR, BL, BR
  +0x10 s16 w                           // texel width   (runtime +0x08)
  +0x12 s16 h                           // texel height  (runtime +0x0a)
  +0x14 s16 u                           // texel left    (runtime +0x0c)
  +0x16 s16 v                           // texel top     (runtime +0x0e)
  +0x18 u8  flags                       // bit0 = mirror U, bit1 = mirror V (runtime +0x1c)
  +0x19 u8  rgba[4][4]                  // per vertex, in R,G,B,A order, vertex-major
  +0x29 u8  blendEnable                 // runtime +0x2d
  +0x2a u8  blendA                      // runtime +0x2e   0=Texture(Cs) 1=Frame(Cd) 2=Zero
  +0x2b u8  blendB                      // runtime +0x2f   same encoding
  +0x2c u8  blendC                      // runtime +0x30   same encoding
  +0x2d char texName[16]                // texture used to draw this quad (runtime +0x32)
```

Quad notes (all CODE unless marked):
- The quad's own `texName` is the texture that gets drawn. 0x1011e600 resolves it via
  0x10315628 (INFERRED: lowercases in place, since the data mixes "MENU    newtex" and
  "menu    newtex") then 0x10039740/0x1003abb0, the texture-by-name lookup. The header
  texture list is not used for drawing.
- UVs (0x1011e600). texW/texH are u16 at texture+0x24/+0x26.
  `U0=u/texW, U1=(u+w)/texW, V0=v/texH, V1=(v+h)/texH`.
  v0=(U0,V0), v1=(U1,V0), v2=(U0,V1), v3=(U1,V1). flags bit0 swaps U0/U1 and bit1 swaps V0/V1.
- Vertex buffer: 4 x 0x1c-byte D3D vertices at runtime +0x50 {x,y,z=0,rhw=1,diffuse,u,v},
  FVF 0x144 (XYZRHW|DIFFUSE|TEX1). Drawn with `DrawPrimitiveUP(TRIANGLESTRIP, 2, ...)`
  (0x1000cd00 called with 5,2,verts,0x1c in 0x1011f230). The vertex order is therefore a strip:
  TL, TR, BL, BR.
- Colour load (0x1011e410). For each vertex, R, G and B are incremented by 1 unless 0xFF,
  so the usual 0x7F becomes 0x80. A is set to `min(int((a+1)*1.5), 255)` unless a is 0 or 0xFF.
  The constant 1.5 is at 0x1032a3ac. The result is packed as D3DCOLOR ARGB.
- Colour at draw time (0x1011ffe0). Each channel is `vtx*mod >> 7`, clamped to 255. The
  default mod is 0x80808080 (= 1.0 per channel).
- Texture stage (CODE 0x1011c0f8, 0x1011f47d): COLOROP = MODULATE2X (5) and ALPHAOP =
  MODULATE2X. Vertex colour 0x80 therefore means x1.0. This is the PS2 convention.
- ADDRESSU/V are set to WRAP (1) while a quad is drawn, then back to CLAMP (3).
- Data: almost every RGB value is 7F7F7F. Common alphas are 0x7F, 0x20, 0x70, 0x40 and 0x60.

### Blend modes (CODE 0x1011f230; the debug strings "Cell <A> <B> <C>" are at 0x1036c248..)

The four quad bytes {enable,A,B,C} select a PS2-GS-style blend `(A-B)*C + Cd`, mapped to D3D
render states. The table below gives SRCBLEND/DESTBLEND (D3DBLEND values). `flag913` is
`[device+0x913]` (INFERRED: the device supports BLENDOP).
- enable=0: ALPHABLENDENABLE=0 (opaque). Data `00 00 02 01` (386 quads).
- (0,1,1): SRCALPHA, INVSRCALPHA. **Normal alpha blend.** Data `01 00 01 01`, about 10k quads.
- (0,2,1): SRCALPHA, ONE. **Additive.** Data `01 00 02 01`, about 9.4k quads.
- (0,2,2): SRCALPHA, ZERO. Data 145 quads.
- (0,2,0): blending disabled.
- (0,1,0): if flag913, ONE, SRCALPHA with BLENDOP=SUBTRACT. Otherwise blending disabled.
- (1,0,0): INVSRCALPHA, SRCALPHA.
- (1,0,1): if flag913, SRCALPHA, ONE with BLENDOP=REVSUBTRACT. Otherwise there is a 3-pass
  emulation: invert the frame (INVDESTCOLOR/ZERO with a white quad), add, invert again.
- (1,1,0): blending disabled.
- (1,2,0): ONE, SRCALPHA.
- (1,2,1): DESTCOLOR, SRCALPHA.
- (1,2,2): ZERO, SRCALPHA.
- (2,0,1): same as (1,0,1).
- (2,1,0): if flag913, ONE, SRCALPHA, SUBTRACT. Otherwise blending disabled.
- (2,1,1): if flag913, ZERO, INVSRCALPHA. Otherwise blending disabled.
- Any other combination only logs "Cell ..." and keeps ALPHABLENDENABLE=1 with the previous
  blend factors.
- Data `01 02 00 01` (6.7k quads, e.g. hfr1 window header strips) is (2,0,1), i.e. a
  subtractive darken (Cd - Cs*As).

Callers can override the quad's blend with a packed `mode` argument: A=mode&3,
B=(mode>>2)&3, C=(mode>>6)&3. For example 0x44 = (0,1,1), normal alpha. When the
override is non-zero, blending is always enabled.

### Drawing a shape (CODE)
- 0x1011fa90 `drawShape(x, y, colorMod, blendOverride, firstQuad)` draws each quad i>=firstQuad
  through 0x1011e8b0 with scale 1.0.
- 0x1011fbb0 `drawShapeScaled(x, y, sx, sy, colorMod, center, blend, firstQuad)`. If sx=sy=1
  it falls through to the call above.
- Per quad (0x1011e8b0): `vx_i = x_i*sx + x - 0.5`, `vy_i = y_i*sy + y - 0.5`. **Quad xy
  are offsets from the shape origin passed by the caller.** Quads are clipped against the
  UI rect at 0x105eed94 and the UVs are adjusted to match.
- Shape bounding box (computed at load in 0x1011e290, stored in the shape object):
  +0x20 minX, +0x22 maxX, +0x24 minY, +0x26 maxY over all quad vertices.

### Stretched / tiled quad (used for window frames), 0x1011ff30 -> 0x1011ee90
`drawQuadRect(x, y, dx, dy, w, h, sx, sy, colorMod, blend, stretchTex)` draws shape[n].quad[0]
into the rectangle `[dx, dx+w] x [dy, dy+h]`, scaled by sx/sy and offset by x,y. **The quad's
own xy are ignored.** The UV span is the drawn w/h in texels (`U1 = U0 + w/texW`), so with
ADDRESS=WRAP the texture **tiles**. With stretchTex=1 it uses the quad's w/h, which stretches.
All callers in the window frame pass 0, so they tile.

---

## 2. Meb: menu layout (chunk type 0x30)

Parser: 0x1015de90 creates the 0x68 menu object. 0x1011af40 reads the header, 0x1011b2e0 reads
the window record and 0x1011b5e0 reads each element record. The menu is built from this in
0x10117e70, its parts are updated in 0x10117450 and it is drawn in 0x10117700.

### File layout (CODE + DATA)
```
MebHeader (0x20 bytes):
  +0x00 char name[16]     // "menu    yesno   "                          -> menu+0x46
  +0x10 u8   unk          // always 1 (DATA)
  +0x11 u8   nElements    // number of element records after the window record (CODE)
  +0x12 u8   pad[14]      // all zero (DATA)
then: WindowRecord, then nElements x ElementRecord, packed back to back.
```
Each record begins with `u16 recLen`. The parser jumps to `rec + recLen` (CODE 0x1011b2e0
end), so records are self-sizing. The 0x50/0x40 values seen in data are recLen, not flags.
recLen is the 0x20 fixed header plus subs plus the two strings, rounded up to 16 (DATA).

**WindowRecord** (fixed 0x20 header, CODE 0x1011b2e0 / 0x10119300 / 0x10119320 / 0x10117700):
```
  +0x00 u16 recLen
  +0x02 s16 x, +0x04 s16 y    // window origin in the 640x480 design space (see section 3)
  +0x06 s16 cursorDX          // cursor (anchor) offset applied to every element (CODE 0x10119d60)
  +0x08 s16 cursorDY
  +0x0a s16 w, +0x0c s16 h    // frame size -> 9-slice background (see below)
  +0x0e s16 frameDX           // offset of the frame/background from the window origin
  +0x10 s16 frameDY           //   (CODE 0x10117700: raw+0xe/+0x10, scaled, + window pos)
  +0x12 u8  unk (0)
  +0x13 u8  anchor            // 0 none, 1 right, 2 bottom, 3 right+bottom (CODE 0x10119320)
  +0x14 u8  nSubs
  +0x15 u8  str1Len           // length of string 1, excluding its NUL
  +0x16 u8  str2Len
  +0x17 u8  noAnchor          // nonzero means the re-layout at 0x1011a8df skips anchoring (CODE)
  +0x18..0x1f zero
  +0x20 Sub subs[nSubs]       // 20 bytes each, see below
        char str1[str1Len+1]  // decimal ASCII, "-1" = none
        char str2[str2Len+1]
        pad to recLen
```
**ElementRecord** (fixed 0x20 header, CODE 0x1011b5e0 / 0x10119980 / 0x10119a00 / 0x10119d60):
```
  +0x00 u16 recLen
  +0x02 s16 x, +0x04 s16 y    // relative to the window's screen position (CODE 0x10119a00)
  +0x06 s16 cursorDX          // extra cursor offset for this element (CODE 0x10119d60)
  +0x08 s16 cursorDY
  +0x0a s16 w, +0x0c s16 h    // hit / selection rect (CODE 0x10119af0 point-in-rect)
  +0x0e s16 unkE              // usually 0. Seen 26..28, 240, -2 etc. (DATA). INFERRED: label/text offset
  +0x10 s16 unk10             // usually 0. -3 is common (89 elements). INFERRED: as above
  +0x12 u16 id                // element id, 1-based in data (-> part+0x3a, compared
                              //   with menu cursor index menu+0x4c in 0x10117700)
  +0x14 u8  unk (0)
  +0x15 u8  navPrev           // INFERRED: previous/next in tab order (mostly id-1/id+1)
  +0x16 u8  navNext
  +0x17 u8  navUp             // DATA (equip 4x4 grid + yes/no + main menu): element id of the
  +0x18 u8  navDown           //   neighbour in that direction. Self = no move.
  +0x19 u8  navRight          //   0xFF = not selectable (e.g. title element). CODE: 0x1011b150 reads
  +0x1a u8  navLeft           //   +0x18 / +0x17 / +0x19 / +0x1a for events 1 / 2 / 3 / 4 (Down / Up / Right / Left)
  +0x1b u8  nSubs             // CODE
  +0x1c u8  unk (0)
  +0x1d u8  str1Len, +0x1e u8 str2Len   // CODE
  +0x1f u8  zero
  +0x20 Sub subs[nSubs]; char str1[]; char str2[]; pad to recLen
```
The navigation byte meanings come from data and code. Example from "menu    equip": element 6
at column 1, row 1 has `05 07 | 05 07 0a 02`, i.e. up=5, down=7, right=10, left=2. The layout's arrow handling
(0x10118190 -> 0x1011b150) reads +0x18 / +0x17 / +0x19 / +0x1a for menu events 1 / 2 / 3 / 4, so those events are
Down / Up / Right / Left (#121, §10.14).

**Sub** (20 bytes in the file; the runtime copy is 0x1c with a cached sheet pointer at +0x18):
```
  u16  type        // which visual state this sub belongs to (see below)
  u16  shapeIndex  // index into the sheet's shape array (CODE: sheet->shapes[shapeIndex])
  char sheet[16]   // Msb sheet name, e.g. "menu    windowps"
```
- On load (CODE 0x1011b2e0/0x1011b5e0/0x1011b840) the sheet name is rewritten. "menu    frames"
  becomes table[0][lang], "menu    comwin" becomes table[1][lang], "menu    key1top" becomes
  table[5][lang], and "menu    lobbyps2" always becomes "menu    lobbywin".
  The table is 8x4 at 0x1038a8c0 (0x1025b100). Columns are languages 0..3; for EN, column 1
  gives "menu    framesus", "menu    comwin  ", "menu    key1topu".
- Choosing a sub for the current state (CODE 0x10119480 window, 0x10119b30 element): every sub
  with `type==0` applies, and so does every sub with `type == map(state)` (0x101197a0:
  state 0->0, 1->1, 3->3, 5->4, 7->2, 9->5, anything else ->6). The last match wins.
  Data: type 4 on elements is the state-5 (disabled/greyed, drawn with colour 0x40402810 or
  0x30808080) alternative shape.
- **Window sub type 6 is the cursor** (CODE 0x1011af40 -> 0x1011b550(6) -> 0x1011b840). It is
  turned into a 0x26-byte cursor object {sheet name, shapeIndex at +0x20, window cursorDY at +0x22,
  cursorDX at +0x24}. In data this is "anc     anc_s" etc. (the pointing hand).
  If shapeIndex==0 the cursor is **animated**: it steps through shapes 0,1,2,... every 3 ticks
  until it reaches a NULL shape (0x10119da0, 0x10119e50). Otherwise it is a fixed shape.
  Cursor position = `elem.cursorDX + win.cursorDX + elemScreenX` and the same for Y
  (CODE 0x10119d60).
- The window's type-0 sub, e.g. ("menu    framesus", 20) or ("menu    comwin", 11), is drawn as a
  normal shape at the window origin (part+0x1e/+0x20), on top of the 9-slice frame.
- Element subs (e.g. "menu    windowps" #13) are the element graphic, mostly pre-rendered
  label text using the "menu2fon" texture. They are drawn at the element's screen x,y with
  colour 0x80808080. A selected element (id == cursor) uses 0x80805020 and is shifted by
  (-1,-1). A pressed element is shifted by (+1,+1) (CODE 0x10117700).

**str1 / str2** (CODE 0x1011b2e0): each string is parsed with atoi (0x103123f2). When the
result is >= 0, it is resolved through the string-table service: 0x10152e50(0xe, n) for str1 and
0x10152e50(0xd, n) for str2. The results are stored as `char*` at elemPart+0x40/+0x44
(0x10119980). INFERRED: help/description text ids, e.g. 321..331 on the main menu items.
I did not resolve which DAT the two string-table categories load from (0x10153c76 loads them
with 0x1025b0c0(0xc/0xd)).

### Window frame / background: 9-slice from the skin (CODE 0x102345f0)
The frame object is 0xc bytes {u32 W, u32 H, u8 altBg}, built from window w/h
(0x102345d0). It is drawn at `winScreen + frameD*scale`, scale = (sx,sy) of the menu. The pieces
come from the active skin sheet (0x10220870 returns `skinSheet->shapes[n]`, global 0x105eedd4).
Each piece is drawn with the tiling rect draw described in section 1:
```
piece 0 (or 9 if altBg) background  at (0,0)        size W x H
piece 2 top edge                    at (24,0)       size (W-48) x 5
piece 7 bottom edge                 at (24,H-5)     size (W-48) x 5
if H > 48:
  piece 4 left edge                 at (0,24)       size 5 x (H-48)
  piece 5 right edge                at (W-5,24)     size 5 x (H-48)
piece 1 TL corner (0,0), 3 TR (W-24,0), 6 BL (0,H-24), 8 BR (W-24,H-24)   all 24x24
```
If the menu flag at menu+0xd0 is set, only the background is drawn (no border).
The frame colour is `alpha<<24 | 0x808080`. The blend override is 0x44 (normal alpha) when
alpha < 1, or 8 under menu flag 0x800000.
DATA: ROM/0/14.DAT ("win0" chunk) is sheet "menu    win00" with 10 shapes of one quad each:
shape 0 = 128x128 newtex background, 1/3/6/8 = 24x24 "corner", 2/7 = 80x5 "hfr1",
4/5 = 5x80 "vfr1". This matches the code. INFERRED: ROM/0/15..21 hold win01..win07, one per
Window-type option.

### Runtime parts built from a Meb (CODE 0x10117e70)
- Part type 0: the window (0x50 bytes, 0x10119230). It owns the frame object (+4), the shape (+8),
  the screen pos (+0x1a/+0x1c), the unanchored pos (+0x1e/+0x20) and size (+0x22/+0x24).
- Part type 1: one per element (0x60 bytes, 0x10119800). It owns the screen rect
  (+0x2a..+0x30), the raw rect (+0x32..+0x38), id (+0x3a), state (+0x10) and str ptrs (+0x40/+0x44).
- Part type 2: the cursor (0x34 bytes, 0x10119cd0), present only if the window has a type-6 sub.

---

## 3. Coordinate system (CODE 0x101600e0, 0x10119320, 0x10119a00)

- The UI rect is at 0x105eed94: s16 left, s16 top, s16 W, s16 H. W and H are loaded from the
  config object [0x104568fc]+0x14/+0x16 (0x10164fab; INFERRED: this is the configured UI/menu
  resolution).
- Scale (only when the menu's scale flag menu+0x99 is set):
  `sx = W * 0.0015625 (= W/640)`, `sy = H * 0.0020833334 (= H/480)`.
  **Layouts are authored for 640x480.** Otherwise sx=sy=1 and the layout is in raw pixels.
- Window screen pos = `ftol(x*sx) + baseX`, `ftol(y*sy) + baseY`. Then anchoring:
  `anchor&1` (1 or 3) adds `x += W - 512`, and `anchor==2|3` adds `y += H - 448`.
  The anchor offsets are unscaled; 512x448 is the PS2 canvas.
  Example: "menu    menuwind" has x=384, anchor=1, so it sits 128 px from the right edge.
- Element screen pos = window screen pos + `ftol(elem.x*sx)`, `ftol(elem.y*sy)`. When scaled,
  size = `ftol(w*sx)`, `ftol(h*sy)`.
- Shapes are drawn with that sx/sy (0x1011fbb0), and the frame W/H are pre-scaled (0x10119480).

## 4. Fonts (partial)

- **"font    fontshp"** (in 51.DAT; 146 shapes; textures "font    font", "MENU    ustatsHD"):
  the glyph for byte c is `shapes[(c & 0xFF) - 0x20]` (CODE 0x10120440: `shapes + c*4 - 0x80`).
  The glyph's minX/minY are returned as metrics. So this is indexed by character code - 0x20,
  not by a separate glyph table.
- Character advance (CODE 0x1011cdf0):
  - c < 0x20 gives 0.
  - ASCII 0x20..0x7E gives `widthTable[c-0x20]`, with the u8 table at 0x1036c438 (95 entries:
    space=7, '0'-'9'=9, 'A'=11, 'W'=14, 'i'=5 ...).
  - Otherwise c is a 2-byte Shift-JIS code converted by 0x1011c870 to an index k.
    For 0x2115 < k < 0x2144 the glyph comes from **"font    usgaiji"** (element icons etc.):
    shape `k-0x2115`, width = maxX-minX. For k < 0x440 the width is `widthTable[k]`; else 14.
- "font    moji" (string at 0x1036c234) is a *texture* looked up in 0x1011ba10 (INFERRED: the
  main glyph atlas). "font    high" (0x1011c500) creates a 256x32 dynamic texture header
  (INFERRED: high-res text cache).
- "menu    ncol" (0x1011c7e0) is a colour palette sheet: 23 ARGB entries are read from its quad
  vertex colours into 0x104e1b98 (default 0x80808080).
- "other   dg_font" (98 shapes) and the "menu2fon" texture were not traced.

## Open items
- Element +0x0e/+0x10 and the window/element bytes I marked unk.
- The code that consumes nav bytes +0x15..+0x1a.
- The string-table categories 0xd/0xe (file ids).
- The exact text rendering path for menu strings, and dg_font.

---

## 5. Findings 2026-10-16 (second pass): frame colour, party/target content, moji widths, help text

Tags as above: CODE / DATA / INFERRED.

### 5.1 Vertex colour, alpha and blend for the window frame (CODE)

**Quad colour storage (correction to section 1).** The file stores the colours vertex-major (R,G,B,A per vertex),
but 0x1011e410 writes them into the runtime quad **channel-major**: R0..R3 at +0x1d, G0..G3 at +0x21, B0..B3 at +0x25,
A0..A3 at +0x29. It then packs vertex i as D3DCOLOR `A_i<<24 | R_i<<16 | G_i<<8 | B_i` into the vertex buffer
(+0x60 + i*0x1c). `0x1011fa30(quad q, vertex k)` returns that packed ARGB of quad q, vertex k (used to read
palette colours out of sheets such as "menu hpcol").

**Load (0x1011e410).** `R' = R==0xFF ? R : R+1` (same for G, B). `A' = (A==0 || A==0xFF) ? A : (u8)ftol((A+1)*1.5)`.
Data alphas are <= 0x7F, so A' <= 192.

**0x1011ee90 (stretched/tiled quad).** Arguments (x, y, dx, dy, w, h, sx, sy, colorMod, blend, stretch):
- Per vertex: `C_i = 0x1011ffe0(quadColor_i, colorMod)` = per channel `min(255, (c * m) >> 7)`. **The caller colour
  MULTIPLIES the quad colour** (0x80 = 1.0); it does not replace it. The original vertex colours are saved and restored
  after the draw (CODE 0x1011f19e).
- The four quad vertex colours stay attached to the four corners of the destination rect (TL, TR, BL, BR) and are
  Gouraud-interpolated by D3D. Primitive: TRIANGLESTRIP TL,TR,BL,BR, so the split diagonal is TR-BL.
- Blend: `blend != 0` -> override (A=blend&3, B=(blend>>2)&3, C=(blend>>6)&3, always enabled).
  `blend == 0` -> the quad's own {enable,A,B,C}; enable=0 goes through 0x1011f200 (blending off).
- Texture stage (section 1): COLOROP = ALPHAOP = MODULATE2X, so `out = min(1, 2 * tex * C/255)` for rgb and for a.
- ADDRESSU/V = WRAP during the draw (tiles), back to CLAMP after.

**0x102345f0 (9-slice) arguments.** (x, y, sx, sy, colorMod, bgBlend, noBorder). It passes `colorMod` to every piece,
but `bgBlend` **only to the background piece** (0 or 9). All eight border pieces are drawn with blend 0, i.e. with the
quad's own blend from the skin.

**Caller 0x10117700 (window part, part type 0).** (asm 0x10117a32..0x10117b10)
```
alphaByte = ftol(part.alpha * 128.0)          // part+0xc float, constant 128.0 at 0x1032ae60
colorMod  = alphaByte << 24 | 0x808080
bgBlend   = (menu.flags34 & 0x800000) ? 8 : (part.alpha < 1.0 ? 0x44 : 0)
noBorder  = menu+0xd0
0x102345f0(winScreen + frameD*scale, sx, sy, colorMod, bgBlend, noBorder)
then the window's own type-0 sub shape: 0x1011fbb0(part+0x1e, part+0x20, sx, sy, colorMod, 0, 0, firstQuad=1)
```
Note firstQuad=1: the window sub shape (e.g. framesus #20, comwin #11) is drawn **without its quad 0**.

**Window alpha (part+0xc), 0x10119480.** It is not a config value (INFERRED: no user setting feeds it).
- Default 1.0 (constructor 0x10119230). So normally colorMod = 0x80808080 (identity) and bgBlend = 0.
- part+0x4c != 0 -> 0.58 (0x3f147ae1). Set to 1 together with menu+0xd0 = 1 (borderless) by a few specific menus
  (0x101fee87, 0x1020619d, 0x1023f9d7, 0x10244c3d via 0x10118150 = "get window part").
- Fade state part+0x44: 1 = hidden (0.0), 2 = fade in (`t*0.05`), 3 = fade out (`1 - t*0.05`), t = part+0x48 ticks;
  values > 1 are replaced by 0.9.
- Open animation part+0x26 (counts down from 25, DAT 0x1036c1a0 = 25):
  `alpha = sx = sy = (25 - n) / 25`. The window zooms in and fades in together.

**Font glyph alpha (issue #70).** The "font fontshp" glyphs (texture "font font", texel alpha 0x77 / 0x88, quads
7F7F7F7F) also keep the loader's 1.5 on their quad alpha: with A + 1 alone, white text was about half transparent and
read as grey. Measured, a party-window name over the dark window: ours 152, with the 1.5 210, the lossless retail capture
(window type 6) about 200; a player name over the paving: 213 -> 244. `XI_UI_FONT_ALPHA_OLD=1` gives the old value.

**Formula to implement (skin pieces).** For each skin vertex (file bytes r,g,b,a):
```
R = (r==255 ? r : r+1); G, B likewise
A = (a==0 || a==255) ? a : int((a+1)*1.5)
M = (int(winAlpha*128), 0x80, 0x80, 0x80)          // (A,R,G,B)
vtx = min(255, (X * M_x) >> 7) per channel
fragment: rgb = min(1, 2*tex.rgb*vtx.rgb/255), a = min(1, 2*tex.a*vtx.a/255)   // interpolated vtx
pieces 1..8: quad blend 01 00 02 01 -> dst = dst + rgb*a        (SRCALPHA, ONE)
background:  winAlpha == 1: quad blend (win00 in 14.DAT: 01 00 01 01 -> SRCALPHA/INVSRCALPHA;
                                       15.DAT: 00 .. -> blending off, opaque)
             winAlpha <  1: 0x44 -> SRCALPHA/INVSRCALPHA
```
DATA (ROM/0/14.DAT): the skin textures newtex/hfr1/corner/vfr1 are **DXT1**, so tex.a is 0 or 1 (no PS2 half-alpha).
Worked numbers for win00, winAlpha = 1:
- Top/bottom edges (2, 7): all vertices a=0x40 -> A=97 -> fragment a = 0.76. A visible additive grey double line.
- Left/right edges (4, 5): a = 0x00/0x08 -> A = 0/13 -> a <= 0.10. Almost invisible.
- Corners: one bright vertex (a=0x40) next to the horizontal edge (TL corner: TR vertex; TR corner: TL; BL corner: BR;
  BR corner: BL), others 0. The line fades out towards the sides. (In 15.DAT the corners fade vertically instead.)
- Background 0: top vertices 40404020 -> rgb x0.51, a = 2*49/255 = 0.38; bottom 7f7f7f60 -> rgb x1.0, a = 1.0 (clamped).
So "invisible border" only applies to the vertical edges; that is the authored look.

### 5.2 Party window and target window content (CODE + DATA)

**Shared sheet table (0x101c8500, getter 0x101c87e0(i) = [0x10630330 + 4*i]).**
0 "font fontshp", 1 "menu windowps", 2 "menu keytops3", 3 "menu pname", 4 framesus (lang alias), 5 "menu contents",
6 "anc anc_l", 7 "anc anc_s", 8 "menu numctrl", 9 "anc anc_scan", 10 "anc anc_regi", 11 "anc anc_equi",
12 "anc kaipage", 13 "menu hpcol", 14 "font usgaiji", 15 "menu fep", 16 "menu netmeter".

**Text with "font fontshp" (all party/target text).**
- Glyph for byte c: `fontshp[c-0x20]` for 0x20..0xAB; the pairs `85 DA` and `85 C8` use shapes 95 and 96; a lone 0x85
  byte is skipped (0x101c9100/0x101c9360/0x101c9700).
- Advance = `(maxX - minX) - 1` of the glyph shape (1 px outline overlap). DATA: digits and capitals are 9x11
  (drawn from 19x23 texels of "font font"), so digits advance 8 px.
- Left-aligned draw 0x101c9330(x, y, str, color) -> 0x101c9360: draw glyph at pen x, then pen += adv.
- Right-aligned number draw 0x101c9700(xRight, y, str, color, 0, sx, sy): walks the string backwards,
  `pen -= adv*sx` **before** drawing each glyph. So the text ends at xRight.
- Width 0x101c9100 = sum of adv.
- Colours are passed as colorMod (multiply) to the glyph quads (fontshp quads are 7F7F7F7F, blend 01 00 01 01).
- Name with ellipsis 0x101c9d30: if strlen > 8, draw the first 7 chars, then shape 108 (0x1b0/4, the "..." glyph).
- Width-limited draw 0x101c9900(maxW, x, y, str, color): stops when 8 + sum(adv) >= maxW.
- Two-line name 0x101c9b50(maxW, x, y, str, color, vcenter, recurse): if it fits, one line at y + (vcenter ? 6 : 0).
  Otherwise split at ':' or at the last ' ', '-', '@', '\'' in the back 3/4, line 1 at (x, y) and line 2 at
  (x+4, y+12) with maxW-4.

**HP colour (0x10201f60, sheet "menu hpcol").** `hpp < 25 -> shape 3, < 50 -> 2, < 75 -> 1, else 0`, colour =
vertex 0 of quad 0 (0x1011fa30(0,0)). DATA: after the load fix-up these are ARGB C0808080 (white), C0808041 (yellow),
C0806141 (orange), C0804141 (red).

**Gauge (0x101caab0 -> 0x101ca8b0).** gauge(x, y, fillShape, frameShape, pct u8, &shown u8):
```
shown == pct : f1 = pct/100, f2 = 0
pct  < shown : f1 = pct/100,   f2 = shown/100     // drop: ghost segment
pct  > shown : f1 = shown/100, f2 = pct/100       // rise: ghost segment
fill  : drawShapeScaled(fillShape, x+4, y+1, sx = f1, sy = 1, color 0x80808080, blend 8)   if round(f1*100) > 0
ghost : drawShapeScaled(fillShape, x+4 + ftol(fillW*f1), y+1, sx = f2-f1, 1, color 0x30808080, blend 0)  if round(f2*100) > 0
frame : drawShapeScaled(frameShape, x, y, 1, 1, f1 == 0 ? 0x30505050 : 0x80808080, blend 0)
shown += ftol((pct - shown) * 0.15); if that step is 0, shown = pct     (per frame)
```
fillW = fill shape maxX-minX. The fill is scaled about its left edge (scale pivot = shape origin).
Sprites (sheet "menu windowps", DATA):
- HP: fill 97 (64x6, texture "menu gauge" u0 v1, red 60202070/60181860), frame 38 (72x8: 4 px caps + 64 px middle at
  alpha 0x20). Wrapper 0x101cac40(x, y, pct, &shown).
- MP: fill 98 (48x4, 48482070/48481860), frame 39 (54x6). Wrapper 0x101cad60.
- 0x101cabf0 is the HP gauge with optional swapped args (used by event windows).

**Party list (0x10203d80 loop -> 0x102020e0 per member).**
- The "menu partywin" menu swaps its layout (0x1011a820) to "menu ptw{n}" (n = party size 1..6, table 0x10385298 +
  32*n) or "menu ptw0". Member i (1..6) origin = element i's screen rect left/top (0x10118830; it is shifted by -1,-1
  when that element is the selected cursor element). DATA: elements are at (3, 7 + 20*(i-1)) in the window, 106x14.
- Member record: 0x10630fb8 + 0x7c*(i-1) (valid flag at +0x7a). +0x06 name, +0x54 alt name (used if +0x50 != 0),
  +0x24 HP, +0x28 MP, +0x2c TP, +0x30 HPP, +0x31 MPP, +0x32 zone, +0x78/+0x79 shown HP/MP gauge bytes.
- Same zone as the player (offsets relative to member origin x, y):
  1. HP gauge at (x+26, y+7), MP gauge at (x+49, y+17).
  2. Name at (x, y), colour 0x80808080, ellipsis after 7 chars if longer than 8.
  3. MP number `%d` right-aligned at x+98, y+10, colour 0x80808080.
  4. TP (when 0x101937c0(0xae,0) is set and 0x10201a60(member) is false): `%d` right-aligned at x+48, y+10 (0x102024b0),
     colour 0x80606060 (grey), or 0x802060d0 when TP >= 1000.
  5. HP number `%d` right-aligned at x+88, y+1, colour from hpcol (above).
  Numbers are drawn after the gauges, so they sit on top of the bars.
- Other zone: "(%s)" zone name right-aligned at (x+105, y+11) colour 0x80606060; name colour 0x80606060; no gauges.
- Then 0x10201ba0 (party/target markers at (x-3, y+10) and (x+99, y+3)), 0x10201ff0 (an icon at (x-28, y+2) with a
  centred number/string), and 0x10202450(x-98, y) (buff icons, colour 0x80808080). Not decoded further.
- Solo (0x10203d80 with [this+0x40]): element 1 rect, gauges with pct 100 and shown bytes this+0x41/0x42, name
  this+0x48, MP this+0x46 at (x+98, y+10), HP this+0x44 at (x+88, y+1), all 0x80808080.

**Target window (0x101500d0; rect at this+0x4c left, +0x4e top, +0x50 right).**
- HP gauge at (left+28, top+29) with target HPP (entity+0xec); the shown byte (this+0x64) is reset to the HPP when the
  target id (entity+0x78) changes, so switching targets does not animate. Skipped when entity+0x128 bit 3 or
  entity+0x130 bit 20 is set.
- Name: entity+0x214 if (u16)entity+0x210 > 0, else entity+0x7c; copied (max 0x31) to this+0x14, drawn with
  0x101c9b50(maxW = right-left-13, left+5, top+6, name, color, vcenter=1, 0), i.e. one line at top+12 or two lines at
  top+6 / top+18. color = 0x80808080 unless the target-state object [0x1057876c] is active (INFERRED: then a
  claim/state colour read from that object's +0x78).
- With a sub-target: name1 width-limited at (left+5, top+6), "(" at (left+9, top+18), name2 width-limited
  (maxW = right-left-21), then ")".
- fontshp glyph (0xA9 or a status code from 0x10097430) at (right-24, top-8) as an icon.
- this+0x5c shape at (left, top) when [0x1057876c+0x5c] & 3.
- Target arrows: shapes this+0x78[frame] (frame this+0xba advances every 3 ticks, count this+0xb9) at (+0xbc,+0xbe)
  colour 0xE0808080 and at (+0xc0,+0xc2) with 0x101504d0's choice of 0xE04040D0 (blue), 0xE0D04040 (red),
  0xE0D0D040 (yellow).
- DATA: "menu targetwi" is window-only: 112x44 at (384,252), anchor 3, sub ("menu windowps", 5).

### 5.3 "font moji" text: glyph cells and widths (CODE)

- Byte to cell (0x1011cdb0): a byte is a Shift-JIS lead byte if 0x81..0x9F, 0xE0..0xEF or 0xFA..0xFC (0x1011d960).
  Single byte b -> cell `b - 0x20` (this includes 0xA1..0xDF half-width kana -> cells 0x81..0xBF). Two bytes
  (lead, trail) -> cell from 0x1011c870 (JIS row/column to cell; port it as is). Byte 0x04 is skipped by the width
  routine. "85 9F".."85 DF" (0x1011c850) is FFXI's accented-Latin range and goes through 0x1011c870 like other pairs.
- Advance (0x1011cdf0, measured on the cell index k): c < 0x20 -> 0; ASCII 0x20..0x7E -> `table[c-0x20]`;
  otherwise k = 0x1011c870(pair): 0x2115 < k < 0x2144 -> width of "font usgaiji" shape k-0x2115 (maxX-minX);
  k < 0x440 -> `table[k]`; else 14.
- **The width table at 0x1036c438 is u8[0x440] indexed by moji cell** (cell 0 = ' '). It is the moji table, not a
  fontshp table (correction to section 4). ASCII widths: ' '=7 !=7 "=7 #=10 $=8 %=13 &=11 '=5 (=8 )=8 *=9 +=9 ,=5 -=8
  .=5 /=7 0-9=9 :=5 ;=5 <=9 ==9 >=9 ?=9 @=11 A=11 B=11 C=10 D=10 E=10 F=9 G=11 H=10 I=6 J=9 K=11 L=10 M=12 N=10 O=11
  P=10 Q=12 R=11 S=10 T=10 U=10 V=11 W=14 X=11 Y=11 Z=11 [=8 \=8 ]=8 ^=9 _=9 `=7 a=9 b=9 c=9 d=9 e=9 f=7 g=9 h=9 i=5
  j=6 k=9 l=5 m=12 n=9 o=10 p=9 q=9 r=6 s=8 t=7 u=9 v=9 w=11 x=9 y=9 z=8 {=7 |=6 }=7 ~=12. Cells 95..0x43F are mostly
  14 (755 of them), the rest 4..16.
- String width 0x1011cea0(str, maxBytes) = sum of advances.
- Not traced: the moji draw loop and its line height (0x1011ba10 only binds the "font moji" texture, or the cached
  "font high" render target when 0x1011c780 says so).

### 5.4 Help / title strings for Meb str1 / str2 (CODE + DATA)

- 0x10152e50(cat, n): looks up cat in the table at 0x10371db8 (28 x {u32 cat, obj*}); the objects are loaded by
  0x10153b40 with `Dat_LangFileId(i)` (0x1025b0c0; per-language u32 tables at 0x1038a540 for English, 0x1038aaf8 JP).
- cat 0xE (Meb **str1**) -> object 0x10578508 -> LangFileId(0xC) -> EN file **55651** (ROM/165/75.DAT).
  cat 0xD (Meb **str2**) -> object 0x10578518 -> LangFileId(0xD) -> EN file **55652** (ROM/165/76.DAT).
  (cat 0xC -> 0x10578698 -> LangFileId(0xE) = 55653, ROM/165/77.DAT.)
- DATA: both are `d_msg`, indexed, bit-inverted (dat-format.md 11.1), one string field per entry.
  55651 has 1099 entries of help text: 321 "View status.", 322 "Equip weapons and armor.", 323 "View spell and song
  lists.", 325 "View job abilities, weapon skills, etc.", 331 "View the balance of power for each region."
  55652 has 201 short titles: 0 "Search", 1 "Region Info", 4 "Layout", 14 "Bazaar", 50 "Global".
- So str1 = the one-line help shown in the help bar, str2 = a window/element title.

## 6. Zone map: position marker, sub-map choice, map table (2026-10-16)

### 6.1 Map parameter table in the DLL (CODE + DATA)

- **VA 0x103821f0** (.rdata, file offset 0x595f0 in FFXiMain.unpacked.dll): 14-byte records, terminated by a record whose
  scale byte (+5) is 0 (0x101f6fe0 / 0x101fc520 stop there). 829 records in this build; the terminator is at 0x10384f46.
  Do not copy it into the repo: read it from the user's DLL at startup. Fingerprint for finding it in other builds:
  first record is zone 100 / map 0 with offsets (-312, -248), i.e. it starts `64 00 00 01 00 01 01 00 01 00 c8 fe 08 ff`;
  the record stride is 14 and the code references `0x103821f5` (first scale byte) and `0x103821fa` / `0x103821fc`.

| Off | Type | Meaning | Evidence |
|---|---|---|---|
| +0 | s16 | zone id (negative ids = region / event maps, e.g. -5 "m_san_*", -100.. "eventmap em_*") | CODE 0x101f700b |
| +2 | s8 | map id (the collision map id of 6.3; also the `_NN` of the usual texture name) | CODE 0x101f7016 |
| +3 | u8 | position in the zone's map list (1-based; 0 = not listed) | CODE 0x101f7004, 0x101fc553 |
| +4 | u8 | low nibble = texture file bank, high nibble = key-item bank (below) | CODE 0x101f81ad, 0x101f7058 |
| +5 | s8 | scale b (1..4 seen; |b| is used) | CODE 0x101f717d |
| +6 | s8 | key-item offset: <0 always available (0x17f), 0 = no map (0x180), else bank + value | CODE 0x101f703d |
| +7 | u8 | 0 in every record | DATA |
| +8 | s16 | texture file offset | CODE 0x101f81ca |
| +10 | s16 | offX | CODE 0x101f70fd |
| +12 | s16 | offY | CODE 0x101f7107 |

- **Texture file id** (0x101f81a0): low nibble of +4: 0 -> `0x14c0 (5312) + s16@+8`, 1 -> `0xd02f (53295) + v`,
  2 -> `0xd417 (54295) + v`, else 0x1592 (5522, "m_000_00", the placeholder). The same placeholder 5522 is loaded when
  the player does not own the map (0x101f7f10, arg 3 == 0), with offX/offY moved to window +0x6c/+0x6e instead of +0x54/+0x56.
- **Key item** (0x101f7030): high nibble of +4: 0 -> 0x180 + v, 0x10 -> 0x73f + v, 0x20 -> 0x8fd + v. The result goes to
  0x101cb810 (INFERRED: "player has key item"); 0x17f = always shown, 0x180 = no map exists.
- DATA: all 829 records resolve to a 512x512 map texture through FTABLE. The texture name at file +0x31 has varying 8-char
  prefixes ("menumap ", "ex_data ", "ex2_data", "ex3_data", "ex4_data", "ex5_data", "dl_data ", "gf_data ", "us_data ",
  "eventmap", "20th    "), so scanning for "menumap" finds only 165 of them. Some zones share another zone's textures
  (133 and 189 use "m_275_NN"; 227/228 use m_220_00/m_221_00; 191 map 1 uses m_191_00). Use the table, not the names.
- Example, zone 230 map 0 (record 127): list 1, bank 0, scale 4, key item 0x181, file `5312 + 164 = 5476` (ROM/18/59.DAT,
  "menumap m_230_00"), offX = -288, offY = -272.

### 6.2 World -> map pixel (CODE 0x101f7120, 0x101f7170, 0x101f70a0)

```
k      = |b| / 2560 * 512                  // = 0.2*|b| map pixels per world unit; b = s8 at +5 (b == 0 -> inf, never in data)
rawX   =  trunc(x * k + 0.5)                // _ftol at 0x103117ac truncates toward zero (FPU RC=11)
rawY   = -trunc(z * k + 0.5)                // north (+z) is up
texU   = rawX - offX                        // pixel in the 512x512 map texture (0x101f70fd / 0x101f7107)
texV   = rawY - offY
column = 'A' + (texU - 16) / 32             // C division (toward zero), 0x101f727c
row    =  1  + (texV - 16) / 32             // label "(%c-%d)" at 0x10384fec; "(?-?)" (0x10384fe4) if no record / no map
```
x and z are the entity position floats [0] and [8] (y = [4] is height). The 15x15 grid is 32 px cells starting at
pixel 16. The player's label uses the same formula on the marker position (0x101fa5df).

- **Check, zone 230:** k = 0.8. BotB at (15.2, 1.7, -29.3): rawX = trunc(12.66) = 12, rawY = -trunc(-22.94) = 22,
  texU = 12 + 288 = **300**, texV = 22 + 272 = **294**, grid **I-9** (just north of the South Auction House entrance).
- DATA validation: projecting the centroid of every zone-230 collision triangle (tools/zone_collision.py) with this formula
  lands exactly on the streets drawn in m_230_00.

### 6.3 Which sub-map (CODE 0x101810e0, 0x101811a0, 0x10181120, 0x10168ee0)

- `mapId = 0x101810e0(coll = [0x105ff404], x, y, z)`: a vertical collision query (dat-format.md 7.6) from (x, y-0.51, z)
  to (x, y+9.49, z) (downward, the game is Y-down). If nothing is hit, a second query from (x, y+0.51, z) to (x, y-14.49, z).
  The map id is the hit placement's flags field (dat-format.md 7.4): `(f>>26 & 3) << 3 | (f>>3 & 7)` (stored by the query
  at coll+0x178+0x34154, read by 0x10168ee0). If both queries miss, mapId = 0.
- Record = first record with zone == current zone ([0x104dfd98]+0x3c350) and +2 == mapId (0x101f6fe0(zone, mapId, 0)).
  No record -> no map / "(?-?)". 0x101f6fe0(zone, _, 1) instead returns the first *listed* record (+3 != 0) of the zone, and
  0x101fc520 builds the map list menu from the (+2, +3) pairs of the zone's listed records.
- DATA: the set of collision map ids equals the set of table map ids in zones 151 {1..6,15}, 157 {4..9},
  194 {1..5,14}, 196 {1,14..17} (zone 151 also has 36 placements with id 0, which has no record). Zone 230: all 0.

### 6.4 Marker sprite and drawing (CODE 0x101f8a50, 0x101ee360, 0x101ee200; DATA 119/51.DAT)

- The map window keeps 32-byte icon items: +0 u16 flags, +2 s8 shape slot, +3 s8 alt slot, +4 s8, +5 u8 alpha,
  +6 s16 rawX, +8 s16 rawY, +0x14 float angle. The player item (window+0x674, set in 0x101f8a50) has flags 0x9f, slot 2,
  position from 0x101f8b60 (6.2 *raw* values of the local player's position) and angle = player vtable[0x1c0]()+4
  (INFERRED: heading). Flag 0x02 = draw rotated (0x1011fe30 with the angle); flag 0x10 = do not centre on the shape box.
- Slots map to shapes through the s16 table at **0x1038214c** (20 entries) into shared sheet 2 = "menu keytops3"
  (section 5.2 table; file ROM/119/51.DAT). Slot 2 -> **shape 159**. Shape 159 has two quads: a direction wedge from
  texture "ANC btwait" (u 1, v 41, 20x23) at x 5..16, y -5..5, tinted 7f,30,40; and a 16x16 dot from "menu marker"
  (u 32, v 16) at -6..6. So the dot sits on the position and the wedge points along +x before rotation.
  Other slots: 1 -> 151, 3 -> 150, 4 -> 152, 5 -> 154, 6 -> 155 (all 16x16 cells of "menu marker"); entry 20 (0x10382174)
  -> 429 (32x32, "menu marker" 0,32) is a separate cursor item.
- A second item at window+0x694 (flags 0x99, slot 11 -> shape 105, alpha 0x80) is placed from an object returned by
  0x100f9400 (its +4 / +0xc as x / z) (INFERRED: a destination or target marker).
- Screen position of an item (0x101ee3b5..0x101ee3fa): `sx = trunc((rawX - win+0x6c - win+0x24) * win+0x58 + 0.5)`,
  same for y with +0x6e / +0x28. +0x58 is the zoom and +0x24/+0x28 the scroll (INFERRED). With the map owned,
  +0x6c/+0x6e = 0 and the texture item gets +0x54/+0x56 = (offX, offY), so texture pixel (u, v) sits at raw (u+offX, v+offY)
  (INFERRED from 0x101f8031, consistent with 6.2).

### 6.5 Markers: party and alliance members (CODE 0x101f7810, 0x101fbc20, 0x101f8cc0; DATA 119/51.DAT; 2026-10-23)

- **"menu map0"** (registry 0x10374538, object 0x10630d64, vtable 0x1033a870, OnEvent 0x101f7810): element 1 "Markers"
  (help 308 "Mark map.") opens **"menu mapv3"**; element 2 wide scan (309, a job ability: 0x100f9400 / "menu scanlist");
  element 3 "Change Map" (690) opens "menu maplist". Cancel closes the map (0x101f9fd0).
- **"menu mapv3"** (object 0x10630d6c, vtable 0x1033a828, OnEvent 0x101fbc20, draw 0x101fba70) is a 10-row list with
  four pages, mapframe +0x106. Left / Right (events 3 / 4, and the arrow elements 11 / 12) step the page with 0x101fa920,
  skipping empty ones: page 0 your own markers (only with the map owned, +0xa46), page 1 always, page 2 party / alliance
  (+0x10e party count, +0x10f alliance count), page 3 more alliance members (+0xa48). The map window keeps 4 x 10 icon
  items of 32 bytes at mapframe +0x174 (6.4 layout; +0x18 = the name) and 4 x 10 records of 0x16 bytes at +0x6d8
  (name +0, present flag +0x10).
- Opening Markers the first time for this map (+0xa45 clear) sends **C2S 0x0D2 {u32 ZoneNo}** (0x101f8c20 ->
  0x100f05f0, 8 bytes with the header). LSB answers with one **S2C 0x0A0 MAP_GROUP {UniqueID u32 @4, zone i16 @8,
  x / y / z f32 @0xC}** per alliance member in the zone (us included). The callback 0x101f7a70 skips our own id and other
  zones, keeps only members whose collision map id at their position (0x101810e0) equals the shown map, looks the member
  up in the party list (party number = flags & 3; equal to ours -> party, else alliance), projects the position with 6.2
  and calls 0x101f8cc0. Opening the map (0x101f7960) does the same at once for party members whose actor is loaded.
- 0x101f8cc0 puts party members from item 20 on, alliance members from 0x13 + their alliance party index (the first free
  item or the one with the same name), sets item flags 0x88, shape slot 5 (party) or 6 (alliance), the raw position, +0xa
  0x10, +0xc 1, +0x10 label colour 2 and the name pointer; a member landing in items 30..39 marks page 3.
- Item flags (draw 0x101ee360, fade 0x101eeb40): **0x01 = shown** (alpha fades up to 0x80; without it down to 0), so only
  the page the list last showed is on the map (0x101fa8f0 clears bit 0 everywhere, 0x101fa8b0 sets it on that page's
  present items); **0x08 = no name label**; 0x10 = do not centre on the shape box; 0x80 = present.
- List rows (0x101fba70): icon at the row + 0x10, the name at + 0x1f, + 2 in colour 0x80808080. The icon index is page 0:
  0 / 1 (absent / present), page 1: 2 / 3, page 2: 5 below the party count else 7, page 3: 7, into the shape table at
  **0x10382178** = 151 150 153 152 154 154 155 155 ("menu keytops3"). On the map, slot 5 -> **shape 154**, 6 -> **155**
  (table 0x1038214c); the label colour table 0x10382084 entry 2 is 0x606070.
- Confirm on a row (0x101fbc20 event 5): page 0 opens "menu mapv2" (centre on / place-remove / rename the marker, help
  314..316); page 1 centres the map on it (0x101f8990 -> 0x101ef540); pages 2 / 3 set flag 8 on the page's ten items,
  clear it on the chosen one (its name shows) and centre the map on it (0x101f8c70). Cancel closes "menu mapv3" only.
- **The list is never empty.** Pages 0 and 1 always draw all ten rows, each with an icon (absent 151 / 153, placed 150 /
  152) and the record's name; page 1 is always selectable and page 0 whenever the map is viewable. The player's own
  marker on the map is a separate item (mapframe +0x674, after the 40 page items): shape slot 2 -> **159**, flags 0x9f
  (shown, no label), placed from the player's actor (0x101f8a30); a wide scan track target is the next item (+0x694,
  shape 153, 0x101f8b0e). Neither is a list row: the party page skips our own id (0x101f7aac).
- **Page 0, your own markers** ("menu mapv2", object 0x10630d70, vtable 0x1033a988; init 0x101fbed0, OnEvent 0x101fbfb0,
  draw 0x101fc070). The window opens beside the list's selected row (x = row x - its width, y = row y; 0x101fb8e0 /
  0x10119040). Element 1 (help 314 "Display map centered around marker") centres the map on it (0x101f8990) and is
  dimmed while the slot is empty (state 5; the cursor then starts on element 2); element 2 (315 "Place/remove selected
  marker") draws keytops3 **29 "Markers"** for an empty slot and **35 "Delete"** for a placed one (the window's two label
  sprites, +0x18 / +0x1c); element 3 (316, "Change Name") asks for the name (0x101fb940, 15 characters). Delete clears
  the slot (0x101f8950); place (0x101f86d0) starts a cursor at the map view's centre (0x101f85b0; speed +0x70 = 8.0),
  Enter puts the marker there (0x101f8720 -> 0x101f88a0: map pixels, flags 0x80) and opens the name entry.
- **Page 1, event markers.** Event op 0x8B (0x100b93c0) {zone @1, slot @3, x @5, z @7 (x 0.001), name[16] @9, '_' =
  space} opens the map under the player (0x100b24a0(4, zone, -1)) and fills page 1 slot `slot` (0x101f8900 / 0x101f89e0
  with page 1); a 0x1B-byte variant (0x100b9580) also names the map; 0x100b96b0 / 0x100b9750 remove one (0x101f8950).
  So page 1 is the quest markers events leave on a map, not a server list.
- **Saved per map:** pages 0 and 1 (items 0..19) are written to the character's USER folder as **"M%03d%02d.MRK"**
  (zone, map; 0x101f7e60, when the map closes after a change, dirty flag +0xa44) and read back on opening (0x101f7c60;
  an older "M%03d%02d_%08d.MRK" with the character id is read when the new one is missing). 0x1C0 bytes: u32 magic
  0x58494D4B ("KMIX"), u32 CRC-32 of the records (0x101c8800, table 0x1037ce90: the standard reflected CRC-32), then 20
  records of 0x16 bytes {i16 x, i16 y (map pixels), char name[16], u8 present @0x14, u8 pad}; a 0x1C8-byte file has the
  CRC at 8 and the records at 0x10. Loaded items get shape slot 3 (own, **150**) or 4 (event, **152**), label colour 2.
- This client: all four pages as above (page 0 with the map viewable, page 1 always, 2 / 3 with members); the list keeps
  its page between openings (+0x106) and steps over unavailable pages; the shown page's markers are drawn on the map
  (own / event markers with their names, members with the confirmed one's name); "menu mapv2" places (an arrow-key
  cursor, Enter, then the name), deletes and renames own markers, written to `USER/<id>/M<zone><map>.MRK` at once
  (retail: on closing the map). Op 0x8B now stores its marker as page 1 slot `slot` (earlier read as a map id).
  **Not done / INFERRED:** centring (the map here does not scroll), the cursor's step per key press (8 map pixels, from
  +0x70) and its sprite (the own marker shape, blinking), reading the older per-character MRK name, the 0x1B-byte op
  variant and the remove ops, and the name label's exact offset (INFERRED from +0xa / +0xc).

---

## 7. Main menu pages (2026-10-16)

Tags as above: CODE / DATA / INFERRED.

### 7.1 There is no per-page label table: page 2 is its own layout (CODE + DATA)

The client does **not** swap label sprites on "menu menuwind". The main menu has **two pages**, and each page is a
separate Meb layout with its own labels, help ids (str1) and titles (str2), all in ROM/119/51.DAT:

- page 1 = **"menu    menuwind"** (registry slot at 0x103722d8, handler object global 0x1057823c, ctor 0x1013fb10, vtable 0x10333ff0)
- page 2 = **"menu    socialme"** (registry slot at 0x10372304, global 0x10578468, ctor inline at 0x1016548e, vtable 0x103363d8)

The menu registry at 0x103722d8 is 0x2c-byte records `{char name[16]; ...; +0x20 obj**; +0x24 u32 flags; +0x28 u32}`
(lookup 0x1015db80, bind 0x1015e860). Both pages are the same 112x202 window at (384,48), right-anchored.

The per-slot actions are a `switch (elementId)` in each handler (vtable slot 6, `OnEvent(s16 event, ?, s16 elementId)`):
menuwind 0x10140330 (element jump table 0x10140644), socialme 0x1014e6e0 (element jump table 0x1014e9b8). The event
byte maps are 0x10140628 / 0x1014e99c (index = event - 3).

### 7.2 Page switching (CODE, event names INFERRED)

- Events **3, 4, 7 and 28** all go to the same case: open the other page (`0x1015e260(name, 1, 0)`), move its cursor
  to the same row, and close the current page (`0x1015e6a0(menu, 1)`). So Left and Right both toggle; there are only two
  pages. (INFERRED: 3/4 are left/right; the code treats all four identically.)
- Selecting element 13, 14 or 15 (left arrow, right arrow, header) re-dispatches as event 4 (`this->vtbl[6](4, ...)`,
  0x101405f5 / 0x1014e8fe), i.e. clicking an arrow flips the page.
- Event 5 = select element (dispatch on element id). Event 6 = cancel: page 1 closes itself; page 2 re-opens
  menuwind hidden (`0x1015e260(.., 0, 0)`), restores its cursor, then closes both (0x1014e90c).
- Row mapping table at **0x1036ffa8**: u8 pairs (menuwind id, socialme id), zero-terminated:
  `(1,1) (2,2) (3,3) (4,4) (5,6) (6,7) (7,8) (8,10) (9,9) (10,5) (11,11) (12,12)`.
  0x101402d0 maps menuwind -> socialme (default 1), 0x10140300 maps back. The pairs keep the screen row, because element
  ids are not in row order (menuwind id 10 Synthesis is row 4; socialme id 10 Current Time is row 8).
- Arrow/header element states on open: 13 and 14 -> state 0xF, 15 -> state 0x10 (0x10140148, 0x1014e650).

Element state setter 0x10118a90(state, elementId) -> 0x101198a0: 0 = restore base, 3 = cursor highlight,
**5 = disabled** (also becomes the base), **0xB = enabled** (base), 0xF / 0x10 = arrow / header (INFERRED: not cursor
targets). The cursor code (0x10118ce0) never highlights a state-5 element.

### 7.3 Page 1, "menu menuwind" (DATA labels/help/titles, CODE actions)

Row = screen row (y = 6 + 16*row). Label = windowps sprite of sub type 0 (sub type 4 = disabled variant). Help = str1
(file 55651), title = str2 (file 55652). Action from the switch at 0x10140330.

| row | id | label | help | title | action (CODE address) |
|---|---|---|---|---|---|
| 0 | 1 | 13 Status | 321 | 81 Status | Monstrosity: 0x105782b0->0x10144c40(0). Else open **"menu persona"** (0x101403c3) |
| 1 | 2 | 15 Equipment | 322 | 20 Equipment | Monstrosity: 0x10141ad0 -> **"menu mp_mncst"**. Else if flag 0x10485824 bit0 and main or sub job == 18 (PUP): **"menu pupequip"**. Else equipment window 0x105781a8->0x10133060 (0x10140405) |
| 2 | 3 | 17 Magic | 323 | 76 Magic List | **"menu mgcmenu"** (0x10140469) |
| 3 | 4 | 19 Items | 324 | 2 Items | inventory 0x1063430c->0x1021c300(0,1) (0x10140485) |
| 4 | 10 | 680 Synthesis (dis 764) | 816 | 170 Synthesis | **"menu cmbmenu"** (0x10140502) |
| 5 | 5 | 21 Abilities | 325 | 68 Abilities | Monstrosity: **"menu mp_mnabi"**, else **"menu abimenu"** (0x101404bc) |
| 6 | 6 | 20 Party (dis 765) | 326 | 19 Party | 0x10201290: **"menu prtyjoin"** if an invite is pending (also closes "menu alarm"); **"menu prty3"** if you lead the party; **"menu prty7"** / prty3 for the alliance case; **"menu prty2"** if in a party; else **"menu prty1"** |
| 7 | 7 | 28 Trade | 327 | 10 Trade | 0x1013fda0: trade with the current target; message 0x7f when it cannot |
| 8 | 8 | 18 Search | 328 | 0 Search | **"menu searchma"** (also 0x10631974->0x1020a1b0(0)) (0x10140556) |

Search window (CODE, 2026-10-18): searchma rows have no Search button; each row starts the search at once or opens a
picker whose choice does (CODE jump table 0x1020d140): 1 Area ("menu arealist"), 2 Name ("menu scname"), 3 Job
("menu searchjo", element id = job id), 4 Country, 5 Race, 6 Level ("menu scchar", min / max), 7 Rank, 8 Friend,
9 / 15 Linkshell 1 / 2, 10 Ballista, 11 Comment, 12 Edit Comment, 13 Monstrosity, 14 Unity, 16 Request, 17 Master
Level. Results open in "menu scresult" (15 rows); a player opens "menu scoption" (1 Tell = "/tell <name> " in the chat
line, 3 Invite = C2S 0x06E with ActIndex 0, 4 / 5 party request, 2 PlayOnline message). Opening Search from the main
menu resets the criteria to the current area. xi-client: Area (this / all areas), Name and Level as prompts, Job,
the result list, Tell and Invite.
| 9 | 9 | 16 Linkshell | 329 | 71 Linkshell | if 0x101cbf00(1,1): error beep 0x10037b80; else linkshell window 0x10630a00->0x101e4020(0,0,0) |
| 10 | 11 | 25 Region Info | 331 | 1 Region Info | **"menu region"** (0x101405c1) |
| 11 | 12 | 23 Map / 44 Mog House | 40 / 41 / 42 | 8 Map / 9 Mog House | in a Mog House: **"menu myroom"**; else map 0x101f73c0(0) (0x10140583) |

Code-driven changes on page 1 (open = vtable[1] 0x1013ffc0, refresh = vtable[10] 0x101401a0):

- **Map / Mog House slot (id 12).** "In Mog House" = zone flag 0x100 from 0x100fc090 (0x100f0b80; the same bit makes
  /map refuse, 0x1007d1f0). In a Mog House and not in Monstrosity the slot gets title 9 "Mog House" and object+0x14 = 1;
  otherwise help 40 "View map of current area.", title 8 "Map", +0x14 = 0. The refresh disables it (state 5, help 42
  "You cannot open the Mog House menu while in another person's Mog House.", +0x25 = 0) unless
  `0x10097a70() && 0x101cbc50()` (INFERRED: it is your own Mog House); then state 0xB and help 41 "Open the Mog House
  menu." (+0x25 = 1). The slot's label is drawn in code (vtable[4] 0x1013fec0) from three windowps sprites cached by the
  ctor at sheet+0x5c/+0xb0/+0x6ec, i.e. **sprite 23 Map, 44 Mog House, 443** (index = +0x25 ? +0x14 : 2; INFERRED: 443 is
  the greyed Mog House label). Sheet sprite n is at sheet+4n (CODE, consistent with 23/44).
- **Synthesis (id 10)** disabled (state 5) when 0x1063098c->0x101e1170() is true: `now - synth+0x74 < 0x17`
  (INFERRED: synthesis cooldown just after a synth). Otherwise state 0xB. There is **no crystal check**.
- **Party (id 6)** disabled when 0x10201ab0() is true: over the party table at 0x10630fb8 (0x7c-byte entries, flag
  +0x7a), exactly one entry fails 0x10201a60 and at least one passes (INFERRED meaning not resolved).
- **Monstrosity** (0x10097c80: player+0x40db6 != 0 or main job byte 0x10485640 == 0x17): id 2 gets sprite 644
  (0x1013ffa0 = windowps sheet+0xa10), help 700 "Customize monster.", title 165 "Customization"; id 5 help 693
  "Display monster skills and abilities."; id 6 Party and id 10 Synthesis disabled.

### 7.4 Page 2, "menu socialme" (DATA labels/help/titles, CODE actions)

| row | id | label | help | title | action (CODE address) |
|---|---|---|---|---|---|
| 0 | 1 | 24 Missions | 468 | 85 Missions | **"menu missionm"** (0x1014e79d) |
| 1 | 2 | 26 Quests | 469 | 100 Quests | runs text command **"/quest"** -> **"menu quest00"** (0x1014e7b9) |
| 2 | 3 | 22 Key Items | 470 | 59 Key Items | runs **"/keyitem"** -> **"menu evitem"** (0x1014e785) |
| 3 | 4 | 31 View House | 471 | 107 View House | 0x1014e825: if in a Mog House (0x101cbc20(0)) and 0x100f95b0(player) is false -> beep; if 0x100f9530(player) != 0 -> message 0xbc + beep; else **"menu mogcont"** |
| 4 | 5 | 30 Bazaar | 472 | 15 Set Bazaar | **"menu bazaar"**; disabled in Monstrosity (0x1014e686) |
| 5 | 6 | 29 Macros | 473 | 80 MacroPalette | **"menu mcrmenu"** |
| 6 | 7 | 27 Config | 474 | 54 Config | **"menu configwi"** |
| 7 | 8 | 36 Help Desk | 475 | 60 Help Desk | 0x10217070 -> 0x10631a10->0x102161b0 (INFERRED: "menu faqmain") |
| 8 | 10 | 34 Current Time | 477 | 111 Current Time | runs **"/clock"** |
| 9 | 9 | 766 Communication | 926 | 179 Communication | **"menu commenu"** (Friend List / Emote List) |
| 10 | 11 | 35 Shut Down | 479 | 113 Shut Down | 0x10160870: yes/no **"menu comyn"** (dialog object 0x10631a00, ctor 0x10210950) with question 631 "Shut down?", or 632 "Cancel shut down process?" when a shutdown is running (zone flags bit 0x80000), or 73 when a logout is running |
| 11 | 12 | 33 Log Out | 478 | 112 Log Out | 0x10160900: "menu comyn" with 72 "Log out?", or 73 "Cancel current logout process?", or 632 during a shutdown |

Socialme events require a player (0x104dfd98 != 0). The handler runs text commands through 0x10080cf0(cmd, 0).
Command table (CODE + DATA): names are 0x18-byte records at 0x10355e..: `{char name[20]; u16 id; u16 ?}`. Handlers
are 12-byte records `{u32 id; fn*; u32 0}` (e.g. id 0x4c at 0x103571a0). Id 0x4c /keyitem -> 0x1007d150 "menu evitem"; 0x4d /quest -> 0x1007d170
"menu quest00"; 0x4e /mission -> 0x1007d1a0 "menu miss00" (after 0x1022d730/0x1022d7d0); 0x4f /map -> 0x1007d1f0.

Not on either page (no reference in these layouts or handlers): Friend List (14), Mog Locker
(45), Delivery Box (46), Layout (47), Diary (48), Languages (49), Play Time (32). Mog Locker / Delivery Box labels are used
by the View House submenu "menu mogcont" (e.g. id 3 sprite 45), which has its own code (INFERRED: 0x1014ea20, which disables
ids 4, 5, 8..11 when key-item / flag checks 0x100eb870 fail).

### Open: alpha test for the interface (2026-10-17)

Some quads draw as solid black bars:
- `menu keytops3` 49/50, the empty list rows: blending off, vertex colour 0, texture `menu newtex`;
- `menu windowps` 74, the `quset` rows: (0,2,2) = SRCALPHA/ZERO over transparent texels.

The original client probably drops these pixels with an alpha test, but neither 0x1011f230 nor its callers set
ALPHATESTENABLE (render state 15), so where the state comes from is not traced. xi-client skips the element sprites
of the quest-area list. `xi-dat ui-sprite SHEET INDEX` prints a sprite's quads.

### 7.5 Social: blacklist, mute list, friend list, Unity / Request / Master Level search (2026-10-18)

- **Blacklist** (CODE): `/blacklist` / `/blist` (command id 0x3b, handler 0x1007c5f0) with fewer than three words opens
  the blacklist window (manager 0x10630974, open 0x101deac0); `add NAME` first runs a name search (0x100e28c0) for the
  id, then C2S 0x03D. "menu blklist" (366x248, 15 rows, title 55652 #126 "Blacklist") lists up to 300 names (0x101de870,
  row cell = name); none -> help line 55646 #103 "No entries in blacklist.". A row opens "menu blkmain" (one row, help
  55651 #593 "Delete from blacklist."); confirming (0x101deb70) sends 0x03D mode 1 for that entry (0x100e0af0) and closes
  it. xi-client: the same; LSB answers 0x042 and the list updates. LSB keeps the list (char_blacklist, sent at login
  as S2C 0x041, edits 0x03D -> 0x042) and itself only refuses party invites from a listed player (0x06E validator);
  hiding their say / shout / tell lines is the client's job (xi-client drops S2C 0x017 from listed names). Live with
  BotC: add / list / Delete, and a tell to itself hidden while it was listed and shown again after the delete.
- **Mute List** (CODE): `/mutelist` (id 0x3c, 0x1007c970) clears the criteria, sets flags 2 = 0x20000000 (0x10107da0)
  and searches every area; results show the title 55652 #189 "Mute List", and a row opens "menu acoption" (one row 55651
  #1046 "Remove from the Mute list", yes/no #1050; players get #1052 "You do not have the ability..."). The Mute list is
  the Assist-channel moderation list (#1043..#1052: warning, thumbs up, mute), not the blacklist. LSB marks muted players
  (char_flags.muted) with 0x20000000, but data_loader tests `flags >> 22` as a Unity id before the flag bits, so 0x20000000
  (= Unity 128) matches nobody: always "No applicable entries" (seen live with BotC muted).
- **Search entries** (CODE 0x10104130, the bit-packed criteria writer): Master Level is **type 7** with min / max 8 + 8
  bits (setter 0x101077f0, criteria bit 0x80); Friend is a bare type 0x0C, the friend ids follow the bit stream as u16
  count + u32 ids (up to 200; 0x100d8b90); the request history is type 0x18 + u16 count + 16-byte names (64); flags 2 is
  a bare type 0x16 + 32 bits, sent last. LSB's search_handler has no type 7: its default case consumes no bits, so the
  rest of the request is misread.
- **Unity** (CODE 0x1020ee40 / 0x1020eff0): "menu arealist" in Unity mode, 12 rows: row 0 "Unity" (55653 #442, your own:
  player byte +0x40de6), rows 1..11 the leaders, names 55653 #402 + i (0x100a35f0: Pieuje, Ayame, Invincible Shield,
  Apururu, Maat, Aldo, Jakoh Wahcondalo, Naja Salaheem, Flaviria, Yoran-Oran, Sylvie). Picking sets flags 2 =
  `(i & 0x3f) << 22` (0x100e3130; 0 -> 0xF800000) and searches. LSB filters `unity_leader == flags >> 22`. Live: BotC
  with unity_leader 1 was the one match among about ten players.
- **Request** (CODE 0x1020d0fe): flags 2 := 0x10000000, search. LSB never sets the flag (C2S 0x11C PARTY_REQUEST is a
  stub that only logs) and its flags-2 test reads 0x10000000 as Unity 64: always none.
- **Friend List** (DATA + CODE): 55651 #330 "Use the PlayOnline viewer's Friend List functions.", #337..339 (handles).
  The list (300 entries of 0x100 bytes at 0x104df770+0xa90, count +0x132) belongs to the PlayOnline community object,
  fed through polcore's function table (0x10456a2c, set at 0x1001572f; the friend calls are thunks 0x1030fa9d.. ->
  table +0x294..), not by the game server; Search's Friend row sends the online friends' ids from it and is dimmed while
  it is empty (0x100e6a40). LSB has no friend service and logs the 0x0C entry without filtering. "menu commenu"
  (Communication, socialme row 9): element 1 Friend List -> "menu flistmai" (Edit Friend List / ... ), element 2 Emote
  List -> 0x10224fc0 ("menu emote" and 0x103861d0). xi-client keeps Communication dimmed.

### 7.6 Mog House: exit door, 2F, layout mode, mannequins (2026-10-18)

Code: `src/client/mog_house.*` (positions, packets, door modes), `apps/xi-client/mog_house.*` (actors, menus, keys).
Tags CODE (FFXiMain.unpacked.dll) / DATA / INFERRED. LSB refs: `0x0fa_myroom_layout.cpp`, `0x03b_subcontainer.cpp`,
`0x026_item_subcontainer.cpp`, `char_sync.cpp`.

- **Door** (CODE 0x101d2870 / 0x101d5bf0): a client-side door entity "Door: Back to Town" on door group `_720`; Confirm on
  it: on 1F with exit bit 0 a yes / no ("menu comyn"; 55653 #46 "Moogle: Heading outside, kupo?" in our own room, #47
  "Exit your Mog House?"); otherwise **"menu mogdoor"** (draw 0x101d5e60, select 0x101d60f0, strings 55648): title #69
  "Which area do you want to exit to?", rows 1 #184 "Stay in your room.", 2 #70 "Area you entered from." (#241 "Leave Mog
  House." outside our own room), 3 #293 "Change floors." (dimmed unless 2F is available), 4 #185 "Select an area to exit
  to." -> the area list (zone names, DATA table 0x1037ec30 by exit bit: 1 = 230 231 232 280; 280 = Mog Garden -> mode
  127, refused with msg 0xBE when locked). The choice opens the door (0x100ad110), then C2S 0x05E `zmrq` with the exit
  bit and mode (row 2 -> 0, row 3 -> 125 on 1F / 126 on 2F, area i -> i).
- **2F available** (0x101d5dd0 +0x1a): own room (0x101cbc50) and the 2F flag; that flag is S2C **0x067** kind 2 **+0x27
  bit 0** (MogExpansionFlag, LSB char_sync.cpp; CODE 0x1009cfc6 -> g+0x3c43a). 2F is MyroomMapNumber 0x267 + style ->
  files 83806..83809.
- **Wall slots** (DATA, chosen by floor through 0x10337df4): 1F 5 slots (0x1037d9c8, San d'Oria native 0x1037da18);
  2F **92** slots at 0x1037da68 = 23 places x heights -5.6 / -3.2 / -2.5 / -1.8: west x -6.2 at z -6.2 -5.15 -4.1 -1 0 1
  4.1 5.15 6.2, north z 8 at x -4 0 4, east x 6.2 (as west), south z -8 at x -4 4. Default rotation per slot: 1F
  0x1037d960 (1 1 3 3 3), 2F 0x1037d968 (3 west, 0 north, 1 east, 2 south).
- **Layout mode**: "menu myroom" element 5 Layout. Confirm then opens (CODE 0x101d5aa0) "menu rmlo1" with nothing
  targeted (Place / Clear all / Quit; 55651 #409..411), "menu rmlo2" on a furnishing (Move / Clear away / Place / Clear
  all), "menu rmlo4" on a mannequin (Mannequin #604 / Move / Clear away / Place / Clear all); handlers 0x101f4300 /
  0x101f43ae / 0x101f449e. Place -> "menu bankmenu" (Mog Safe / Mog Safe 2, 2 dimmed without 2F; 0x101d6880) -> the
  safe's furnishings. In layout mode the furnishings are targets (Tab).
  - C2S **0x0FA** (CODE 0x100f0bb0, 0x10 bytes): ItemNo @4, Index @6, Category @7, FloorFlg @8, x = col @9, y = level
    @0xA, z = row @0xB, v = rotation @0xC; wall pieces send x = slot, y = z = 0; ItemNo 0 when leaving layout mode. C2S
    **0x0FB** (0x100f0c70, 8 bytes): ItemNo @4, Index @6, Category @7 puts one away. The server answers with 0x020.
  - Floor cells (LSB): 1F col 0..5 and 14..19 for rows 0..23, cols 6..13 only from row 18 (the walkway stays free);
    2F 20 x 26.
  - **Placing keys** (CODE 0x101cdf10, action context 0x42; actions in the table 0x1036d6c0):
    - Confirm (action 0x8f: Enter / keypad Enter) places, Cancel (0x90: Escape) gives up the piece.
    - The arrows (actions 0x31 Up, 0x33 Right, 0x32 Down, 0x34 Left) are directions 0..3, turned round by the table
      0x10337e04 (2 3 0 1) when the camera's +0x4c >= +0x58 (0x101ccc10; INFERRED: the eye and look-at z, so the arrows
      follow the camera when it looks toward -z) unless config 193 is 1 (not traced).
    - Floor pieces: up = row + 1, right = col + 1, down = row - 1, left = col - 1 (0x101cccb9 -> 0x101ccd00); the piece
      moves anywhere in the room's grid (0x101cca70 clamps), and blocked cells only stop the Confirm.
    - Wall pieces go to the neighbouring slot by direction: tables by floor at 0x10337de4 (DATA 0x1037d7dc 1F 5 x 4,
      0x1037d7f0 2F 92 x 4); the slot's facing from 0x10337dec (0x1037d960 / 0x1037d968).
    - Turning: action 0x8d (keypad minus) a quarter back, 0x8e a quarter forward (no keyboard key by default: gamepad).
  - **Clear all** (rmlo1 row 2, 0x101f435d -> 0x101d3870): a yes / no with 55651 **#615** "Clear away all furniture?"
    (table 0xE entry 0x267; 0x102109e0, answered by 0x101d3780). rmlo1 row 3 Quit -> state 0 (0x101d2f30).
  - The room state machine 0x101d2f30: 0 normal (leaving layout mode sends 0x0FA with ItemNo 0), 1 layout ("menu rmlo1"
    when nothing is targeted), 2 Place (the safe list 0x101d6840), 3 / 4 put away.
  - Where a new piece starts: traced since, §7.8 (the earlier "0x101cca30 preview default" was wrong: that
    function returns the wall-slot facing table). INFERRED: Escape closes a layout menu without leaving layout mode.
- **Furniture collision** (CODE 0x1016d490, called by the move resolver at 0x1016ef49): furniture is tested as boxes
  (12 triangles from an axis-aligned box, turned and placed with the piece). Here: the footprint (0.6 per cell, w / d
  swapped by an odd turn) and the furnishing table's height; wall pieces, pieces on tables and ones lower than 0.5 do
  not block (INFERRED: the game's box size comes from the model, not traced).
- **Text**: item names are Shift-JIS; "font moji" holds ASCII in cells 0..95 then Shift-JIS from 0x8140 in code order,
  188 cells per lead byte (DATA), e.g. the mannequins' 0x8189 male sign. The menu font ("font fontshp") has ASCII only;
  ffxi-linux draws other characters there from "font moji" (INFERRED).
- **Mannequins** (item type 14): a PC actor (CODE 0x101ccf60) of race = exdata +0x12 in the looks of S2C **0x026**
  ITEM_SUBCONTAINER (is_used @4, container @5, index @6, race_hair @0xA, head .. range @0xC..0x1B, race @0x1C, pose
  @0x1D); the face is 31 (LSB sends 1; its comment says mannequins use face 31). Pose = exdata +0x13 -> motion `mq<n>?`
  (table 0x1037e028); DATA: stems mq00..mq61 in the race's emote files base + 2984 + 32..44 (which one the game takes:
  INFERRED, the first that has it). Gear: exdata +0x0A..+0x11 = the Storage slots on main .. feet; dressing is C2S
  **0x03B** {Kind @4 1 equip / 2 unequip / 5 all, Category1 @8, ItemIndex1 @0xC, ContainerIndex @0xD, Category2 @0x10 =
  2 Storage, ItemIndex2 @0x14}. Here the gear slots are an "menu inventor" list (the game's own mannequin window was not
  found: INFERRED).
- Live (BotB): the door menu (with its title #69), 1F -> 2F -> 1F, a Simple Bed placed on 2F (exdata floor 1, col 9,
  row 15), Bronze Leggings put on a Hume mannequin (exdata +0x11 = Storage slot 4) and drawn. 2026-10-18 evening, 1F:
  Place (arrows + keypad minus, col 9 row 18 turned), Move (two cells left), Clear away, Clear all (the #615 question;
  the mannequin went back too), Quit; walking into the placed bed stops at its edge (z 3.1 for a box from 3.6).

### 7.7 Communication, Emote List, linkshell name markers (2026-10-18)

- **Communication** (socialme row 9, help 926, title 179): **"menu commenu"**, element 1 Friend List (help 330, PlayOnline's
  list: dimmed here), element 2 Emote List (help 928, title 180) -> CODE 0x10224fc0: "menu abisortw" and **"menu emote"**
  (12 rows, keytops3 row sprites; rows drawn as text here), filled by 0x102254c0 / 0x10225250.
- **Entries** (DATA + CODE): file **96** read raw (0x101c42f0(0x60, buf, 0x1000)): u32 count (112), then 32-byte records
  `{u16 id; u16 label; u16 help; u16 parent; char command[16]; char argument[8]}`; 0x10225160 finds a record by id,
  0x10225220 keeps those whose parent equals the folder shown (0xFFFF = hidden, id 0 empty). Label / help are entries of
  the language text file Dat_LangFileId(0x23) = **55675** (English; "/bow", "Bow politely."). Records without a command
  are folders: 4 /bell (children 51..75: C3 .. C5, "+" sharps), 23 /jobemote (76..97: war .. run), 38 /sitchair (98..110:
  stool, chairs 1..12). The visible order comes from a 128-entry id table, the object 0x1063463c (CODE 0x10225be0):
  0..127, unless the player's own `USER/<id>/eix.dat` (0x101b9ad0 with 0x1037cc10, callback 0x10225c20) reorders it;
  so by default record id order, which is the file's (DATA: ids 0..111 ascending). Confirming (0x102257d0 case 1)
  never closes the list: a folder opens its entries (0x10225600); a job emote (ids 76..97) whose S2C 0x11A bit (id -
  76) is clear runs its text command at once with sound 0004 (0x10224f40 -> 0x102256f0, "/%s %s" 0x1038624c); a chair
  1..12 (ids 99..110) whose chair bit is clear prints 55646 #194 "You do not possess that chair." (0x10224f90,
  0x10152e20(7, 0xc2)); any other emote raises the sub-target cursor (0x10225770 -> 0x10159370 type 0x7f, starting
  on the current target) and runs on the pick (0x102257b0). The unlocks come from C2S 0x119 / S2C 0x11A (JobEmotes
  u32 @4 by job bit, Chairs u16 @8; 0x100fd4b0 / 0x10225010), asked for when the list opens. xi-client: the same
  (`/bow <st>`), live 2026-10-20 with TestA: /bow -> the <st> cursor -> "TestA bows.", the list still open;
  /sitchair 1 -> "You do not possess that chair."; LSB sent job emotes 0x3f, chairs 0.
- **Emote packets** (LSB 0x05d_motion): Param @8: /bell note C3 .. C5 = 6 .. 30, /jobemote = 0x1E + job; /sit 0x0EA,
  /sitchair 0x113, /jump 0x11D. /upset is /disgusted (26).
- **Linkshell marker** (LSB char_update / char_status): other PCs 0x00D General block: Flags1 +0x20 bit 17 LinkShellFlag,
  Flags2 +0x24 r, g, b; our own 0x037 +0x28 bit 25, +0x31 r, g, b. LSB sends each 4-bit colour as c << 4 | 15. The
  marker is the name's first icon, "font fontshp" glyph 0x92 (the pearl, 16 x 16 from "menu ustatshd") drawn in the
  linkshell colour (§10.x "Icons before the name", traced for goal 20); our own name shows over our head too.
- **/names** [on | off] [otherpc] (CODE 0x10079010, command 0x1a8): on shows names (0x1047d694 = 0), off hides them,
  no argument toggles; when that hides them, "otherpc" (the second word if given, else the first) sets mode 4, else 0
  (0x1047d695). The name pass (0x100d0440, case table 0x100d0600) then draws nothing in mode 0, the target's name
  included, and in mode 4 hides only other players' (keeps our own, NPCs' and monsters'); modes 1..3 have cases but no
  writer. There is no other own-name option: our own name also goes in first person (0x10487f80). Live 2026-10-20
  (TestA, Southern San d'Oria): off -> no names; off otherpc -> our own and the NPCs' back.
- Live (BotB + BotA, Southern San d'Oria): Emote List open (51 rows, "Bow politely." in the help line), /bow chosen ->
  "BotB bows." in both logs; each client shows the other's marker (red / blue) and its own.

### 7.8 Mog House: gardening, Open Mog House, Remodel, new pieces, mannequin poses (2026-10-19)

Code: `src/client/mog_house.*` (packets, plant menus, new-piece start), `apps/xi-client/mog_house.*`. Tags as in §7.6.
LSB refs: `0x0cb_myroom_is.cpp`, `0x0fc..0x0ff_myroom_plant_*.cpp`, `exdata/flower_pot.h`, `char_sync.cpp`.

- **"menu myroom"** (object 0x105782b4, vtable 0x10334470; select 0x10145390, jump table 0x101454ec): 1 Storage, 2
  Delivery Box, 3 Change Jobs ("menu jobchang"), 4 Gardening -> runs `/garden`, 5 Layout -> `/layout` (0x10080cf0 runs a
  text command), 6 Open / Close Mog House -> C2S 0x0CB, 7 Remodel -> 0x101d6440. Element texts (55651 help / 55652
  title): 1066 / 195 Storage, 350 / 87, 43 / 67, 356 / 89 Gardening, 357 / 4 Layout, 666 / 147 Open Mog House, 936 / 182
  Remodel. Text commands (table at file offset 0x2d911): `/layout` 0x7E, `/remodel` 0x7F, `/garden` 0x80; their handlers
  (0x1007e418..0x1007e7f5) refuse outside our own Mog House and while it is open (message 0xBC of object 0x10578158, not
  traced); `/remodel` also needs 0x100f96d0.
- **Element states** (0x10145170, per refresh; 0x10118a90(id, state): 5 dimmed, 0xB enabled): visiting -> 5, 6, 7 dimmed;
  open to visitors (0x100f9610: player +0x40c40, LSB S2C **0x067** kind 2 **+0x24** MogHouseFlag) -> Layout and Remodel
  dimmed, element 6 shows "Close Mog House" (55652 #148, help 55651 #667; here: the element's state-1 sprite, windowps
  608); a request in flight (+0x16) -> 5, 6, 7 dimmed; else Layout when in a residential area (0x10097a70), Remodel also
  needs 0x100f96d0 (INFERRED: the 2F flag). Open / Close: C2S **0x0CB** {Kind @4: 1 open, 2 close, 5 remodel; Param1 @5;
  Param2 @6}; LSB answers with 0x067 (and a party reload).
- **Gardening** = room state 3 (0x101d2f30 case 3, 0x101d2fb6): no installed flowerpot -> 7031 #59 "I can't raise
  plants without a flowerpot, kupo..." and back to state 0; else the **pot cursor** (object 0x10631900; its filter
  0x10207090: item type 12 with exdata byte 1 bit 0x40; action context 0x42, the placing keys' context; 0x10206fa0 prints
  7031 #61 "Choose a flowerpot, kupo."). Confirm on a pot (0x102071ab) opens **"menu plnt<n>"** by the pot's exdata Step
  (byte 0; DATA table 0x103854f4 = plnt 1 6 2 2 3 2 2 3 2 2 4 6 for Steps 0..11), "menu plnt6" when Dried (byte 1 bit 7);
  Cancel (0x10207107) leaves gardening: state 0, which opens "menu myroom" again (0x101d3097; layout's Quit does the same).
  Window title 55652 #89 "Gardening". The menus (elements; help 55651):
  | Menu | Elements | Handler |
  |---|---|---|
  | plnt1 (empty) | Examine 369, Plant Seeds 370, Return 371 | 0x10206800 |
  | plnt2 (growing) | Examine, Dry 372, Throw away 373, Return | 0x102068d0 |
  | plnt3 (wants a crystal: Step 4 / 7) | Examine 374, Feed 375, Dry, Throw away, Return | 0x10206980 |
  | plnt4 (ripe, Step 10) | Examine, Harvest 376, Dry, Throw away, Return | 0x10206aa0 |
  | plnt5 (not in the table) | Examine, Harvest, Return | 0x10206b70 |
  | plnt6 (sown, withered, dried) | Examine, Throw away, Return | 0x10206bf0 |
  - Examine (0x102063e0): C2S **0x0FD** {ItemNo, ItemIndex, Category}, then the moogle's remark (0x100a1790 = a 7031
    standard message in the log; file 7031 is LangFileId(0x67), message category 6): dried #139; Step 0 #62, 1 #63, 2 3 5
    6 8 #65, 4 7 #67, 9 #69, 10 #71, 11 #73, each but 0 plus a coin toss (0x10314520 & 1). LSB adds battle messages ("In
    this flower pot: / Seeds sown: ..").
  - Plant Seeds / Feed: "menu bankmenu" (Mog Safe / Mog Safe 2), then that safe's items: seeds = item type 11
    (0x10206ce0), crystals = items 0x1000..0x1007 (0x10206e00); #75 "Choose which type of seeds to plant, kupo." / #76
    (crystal); the pick asks 55653 #48 "Sow seeds?" + 7031 #83 / #50 "Nourish the plant with this crystal?" + #84; yes ->
    C2S **0x0FC** {PlantItemNo @4, AddItemNo @6, PlantItemIndex @8, AddItemIndex @9, PlantCategory @0xA, AddCategory @0xB}.
  - Harvest: 55653 #51 "Gather current plant yields?" + 7031 #78, #85 -> C2S **0x0FE** {ItemNo, ItemIndex, Category,
    CancellFlg @8 = 0}. Throw away: #52 "Throw this away?" + #79, #87 -> 0x0FE with CancellFlg 1. Dry: #49 "Dry out this
    plant?" + #77, #86 -> C2S **0x0FF** {ItemNo, ItemIndex, Category} (0x10206600 / 0x10206680 / 0x10206640).
  - Here the arrows step the cursor through the installed pots (INFERRED: up / right forward); the pots are named
    targets while gardening and the target arrow follows the cursor.
- **Remodel** (0x101d6440 / 0x101d64b0): **"menu roomlist"** (object 0x106306c0) rows 55653 #450 "No changes" (value 0),
  #446..#449 = styles 615 San d'Orian, 616 Bastokan, 617 Windurstian, 618 Mog Patio (dimmed without key item 0xBEB,
  0x10097db0). "No changes" closes; a style asks #451 "Make changes?" (0x101d66d0), yes -> C2S 0x0CB Kind 5, Param1 =
  object +0x50 (0 here, INFERRED), Param2 = the style (0x101d6680). LSB answers "Your second floor has been
  successfully remodeled." and re-zones when on 2F.
- **A piece taken from the safe** (CODE 0x101cd354..0x101cd44f, the piece constructor for an item not installed): floor
  = the current floor; a wall piece starts in slot 0 with that slot's facing (0x101cca30(floor)[0]); a floor piece at col
  9 row 12 level 0 turned 0 — or, when the room's +0x7e6 names a floor piece (the piece last moved in this layout session:
  set by Move, 0x101d353d; cleared on entering layout mode, 0x101d2ffe), at that piece's col / row / level — then moved by
  (-(w' / 2), -(d' / 2)) through 0x101ccd00, which clamps it into the room (0x101cca70). 1F col 9 row 12 is in the
  walkway, so the piece must be moved before it can be placed.
- **Mannequin pose** (CODE 0x101cd0f2..0x101cd1a5): p = exdata +0x13 (0x100fe8f0(.., 0xe, ..)); the actor is built
  (0x100ab440) with stem table 0x1037e028[p & 7] (`mq0?` .. `mq7?`) as its idle stems (+0x7c8..+0x7d4) and p >> 3 in
  actor +0x8a0 (0x100ab599); p == 8 is special (stem `mq6?` with 1, the race from the looked-up item record +0x28). The
  actor's state 0x31 (0x100ab543) picks motion loader 5 (0x100d10c0 -> 0x100d1163), which loads base + 2984 + 32 +
  (p >> 3) (u16 table 0x1035d84c by race) and its piece 2 (+ 64 / + 128 by body variant, 0x1035d85e; 0x100d1ea0): the 13
  files + 32 .. + 44 are exactly the ones holding `mq` motions. xi-client: `MogHouseView::poseMotion` (goal 28).
- Live (BotD, 2026-10-19; PLANS goal 26): gardening (place, sow, examine, throw away),
  Open / Close, Remodel and the Mog Garden (exit mode 127 -> zone 280).

### 7.9 Macros: the game's macro files, the runner, the palette and the Macros windows (2026-10-20)

**Files (DATA, checked on the bots' `USER/2..5` in the lsb-dev install; CODE 0x101b9430 writes, 0x101b9ad0 reads).**
Every file is a 24-byte header `{u32 1, u32 ?, md5[16] of the rest}` and its payload (`src/dat/macro_file.*`):
- `mcr.dat`, `mcr1.dat` .. `mcr399.dat`: one set each, index = book * 10 + set (0-based; CODE 0x101a21c0 `"%s%s%d%s"`).
  Payload 0x1db0 bytes: 20 macros of 0x17c, `{u32 0, char line[6][61], char name[10]}`; 0..9 the Ctrl row (keys
  1..0), 10..19 the Alt row. A line holds 60 bytes (CODE 0x101a1450), a name 8 characters (0x101a14f0). Text is
  Shift-JIS, auto-translate as `FD <4 bytes> FD`.
- `mcr.ttl` / `mcr_2.ttl`: books 1..20 / 21..40, 16 bytes a name, so **40 books**. A fresh character's are
  "Book01".."Book40" (and ours default to those when the files are missing, INFERRED).
- `mcr.sys`: u32 book * 20 + set * 2 (the manager's +0x1dbc; +0x1db8 = set * 2, +0x1dc0 = book).
- A size or MD5 mismatch resets the set (d_msg 55646 #200 "Macro Book %u Set %u contains invalid data and has been
  reset."); here the set starts empty and the file is only rewritten once the set changes.

Where we keep them (`[ffxi-linux] macro_dir`, `test_hooks.cpp`): empty = our copy in `~/.config/ffxi-linux/USER/<id>`
(`<id>` = the content id in hex; LSB's is the character id), filled once from the install's `USER/<id>` (only
`mcr*` files), or else from our older `macros/<Char>.txt` (written out as the game's files); `install` = the install's
own folder, shared with the Windows client; any other value = `<value>/USER/<id>`. Every change is written at once
(beside the file, then renamed over it).

**Macro manager** (object 0x1062fc74, 0x2360 bytes, ctor 0x101a15d0): set setter 0x101a2440 (wraps 0..19 in steps of
2) and book setter 0x101a24d0 (wraps 0..39), each saving the old set (0x101a2210) and loading the new (0x101a2270);
next / previous set 0x101a2600 / 0x101a2620; book names 0x101a25a0 (get) / 0x101a2530 (set) / 0x101a2390 (save both
.ttl). `/macro book N` / `/macro set N` (0x10079e40) wrap and are refused while the editor is open.

**Runner** (0x101a1810 -> 0x101a12f0 starts: the macro's lines are copied and it starts at line 0, so a new macro
replaces the running one; 0x101a1870 runs one line a frame; `dat::macro::stepFor`):
- an empty line ends the macro; lines starting `//` are skipped;
- `/wait N` (`"%s %d"` against an exact "/wait"; N 1..60, none = 1) waits N s and is not run; another N does nothing;
- `<wait N>` (`"<wait%d>"`, 0..60, none = 1) cuts the line: the part before it runs, then the wait;
- lines run through 0x10080cf0(line, 2) (mode 2 = macro);
- the wait clock holds while DAT_1057876c +0x59 / +0xc4 are set: INFERRED to be the chat line being open.

**Keys** (fixed table 0x1036d6c0, 3 x u16 a action, `dik | flags << 8`): Ctrl+1..0 = actions 0x6c..0x75, Alt+1..0 =
0x62..0x6b, Ctrl / Alt alone 0x76 / 0x77 (the palette); **(Ctrl|Alt)+Shift+1..9, 0** = 0x59..0x61, 0x58: set 1..10
(palette handler 0x101a3100); **Ctrl / Alt + Down / Up** = next / previous set while the palette shows (palette events
1 / 2 with the modifier held, 0x101a35e0). Confirm on a palette button runs it and closes the palette (0x101a3510).

**Titles:** the palette and the edit palettes put string table 10 (file 50, an XISTRING file; category -> file from
LangFileId table 0x1038a540) **#39 "Macro Set %s (#%d)"** (book name, set) in the title window (0x101a38b0 /
0x101a4390); the book list **#109 "Macro Book (#%d)"**, with the hovered book's name as the help line (0x101a60c0).
Our menu font runs wider than the game's, so a title longer than the 112-wide title window is drawn in the narrower
outlined font (INFERRED stand-in).

**Windows** (registry 0x103724e8; objects 0x1062fc8c..0x1062fcb0; layouts in menu DAT 39542):

| Window | Layout | What it does |
|---|---|---|
| Macros entry | "menu mcrmenu" 112x42 @384,48 | Main menu page 2 row 6 (help 473). Row 1 Edit Macro (829) -> the book list; row 2 Edit Equip. Set (830, "menu mcres20" / "mcresmn") dimmed here. |
| Book list | "menu mcrselec" 112x330 @384,48 | Rows 1..20 = books, 21 / 22 page arrows (2 pages; Left / Right flip too); opens on the palette's book. Handler 0x101a6470: confirm -> the options. |
| Book options | "menu mcrselop" 112x90 | Authored at 0,0; CODE 0x101a65d0 puts it left of the list's selected row (row x minus its width) at the row's y, kept above the list's bottom. Rows (handler 0x101a66c0): Edit (set the book, 0x101a24d0, open the palettes, 0x101a4080), Change Name (input line, 0x101a6220), Copy (0x101a28e0), Paste (yes / no 55648 #239, 0x101a6680), Delete (#240, 0x101a66a0). |
| Edit palettes | "menu mcr1edit" / "menu mcr2edit" (Compact) or "mcr1edlo" / "mcr2edlo" (Full), both at 16,48 | One row shown (focus +0xc: 1 Ctrl, 2 Alt). Down walks Ctrl(s) -> Alt(s) -> Ctrl(s+1), Up back (0x101a4500 / 0x101a4bc0). Confirm on a key -> the editor (0x101a4170). Event 8 -> "menu mcrbedit" for the key (+0x1c = 1, +0x18 = key); without a key, the set (help 668..670). |
| Button / set options | "menu mcrbedit" @384,134 | Copy / Paste / Delete (help 659 / 318 / 317), handler 0x101a5d30; for the set, Paste and Delete ask 55648 #237 / #238. |
| Editor | "menu macro" 366x248 @16,48 (vtable 0x10337460, handler 0x101a5240) | Elements 1 / 2 Prev / Next (keytops3 102 / 103), 3 the name (112,32 88x24), 4..9 the six lines (188,68.. 336x24, box from x - 168). Confirm on a field opens the game's input line (0x101c64d0; event 17 commits, 0x101c5a00); event 8 on a line -> "menu mcrledit". Cancel saves (0x101a5950, 0x101a41e0). |
| Line options | "menu mcrledit" @384,48 | Copy / Paste / Delete line (help 660 / 320 / 319). |

Here (INFERRED where the code was not followed): event 8 is the Select Window key (Num + / F); Select Window again in
"menu mcrbedit" switches it between the key and the whole set; Prev / Next step Ctrl 1..0 then Alt 1..0, wrapping; the
key's label ("Ctrl5") sits in the box left of the name; the options window closes on Edit, and the Macros entry and
book list hide while the palettes are edited; Delete line clears the line (no shifting).

### 7.10 Trading with a player (2026-10-20; goal 36; `src/ui/game_ui_trade.cpp`)

**Objects (FFXiMain.dll, unpacked lsb-dev build).** The trade manager is the global 0x10578484 (ctor 0x10150f70, 0x148
bytes, vtable 0x10335060); "menu trade" in the registry points at it. Its windows: "menu gift" 0x105781e8 (the
partner's), "menu money" 0x10578248, "menu itemctrl" 0x10578200 (how many), "menu moneyctr" 0x1057824c (gil).
Open (0x10151be0) opens money, gift and trade and sets both states (+0xc0 ours, +0xc4 the partner's) to 3 = editing;
close is 0x10151cc0.

| Window | Layout | Contents (CODE) |
|---|---|---|
| "menu trade" | 16,48 182x94 | Elements 1..8 boxes, 9 OK, 10 Cancel, 11 the gil field (numctrl 84x16 at 76,7). Our gil right-aligned in 11 (0x10140840); item icons with the count over 1; our name at (+4, +5), clipped before the gil field, orange 0x80804020 once we confirmed (state 4) (0x10151300). |
| "menu gift" | 16,144 182x94 | The partner's eight boxes (44-byte records at 0x104dfd98 + 0x19308), their gil right-aligned at (right - 14, 8), their name (manager +0x7c) at (+4, +5), orange once they confirmed (0x10138f20). |
| "menu money" | 16,240 | Our held gil. |
| "menu itemctrl" | 16,240 112x56 | One 24x16 numctrl field. |
| "menu moneyctr" | 130,240 366x56 | One 84x16 numctrl field. |

**Keys (0x10151510; box map 0x10371d70: element 11 is box 0 = gil, elements 1..8 boxes 1..8).**
- Confirm on an empty box opens the item list; on a filled box empties it (0x101510d0 with count 0); on the gil field
  opens "menu moneyctr" (0x10140cc0, starting at the gil offered; callback 0x101514f0).
- Confirm on OK sends 0x033 Kind 2 (0x100eb9e0) and locks (state 4): while locked, confirm on anything else moves the
  cursor to OK, the cursor stays on OK / Cancel and Up / Down step between them.
- Confirm on Cancel sends Kind 1 (0x100eb940) and closes at once (the server tells only the partner).
- Back moves the cursor to Cancel; it does not end the trade.
- Select Window, or Down from boxes 5..8, moves into the gift window to browse the partner's items (their info in the
  item info window 0x10578204). Not done here: the cursor stays in our window.

**Changing the offer (0x101510d0).** An item already in a box goes back into that box. When locked, 0x033 Kind 3 goes
first (LSB ignores it), then 0x034, then both states go back to 3. A partner change (0x023, 0x10152380) also unlocks
both sides; LSB resets both accepts on any change.

**The item list (row colour 0x10150d90).** Dimmed (0x80404040): bazaar-listed or locked items (0x101cb510 / 0x101cb5d0
flag 2), equipped items (0x10131e40), item flags & 0x4000 (record +0xcbc, byte +1 & 0x40), pearls worn in the linkshell
slots (0x100e9660 kinds 1 / 2). Orange (0x80804020): already in the trade (it can still be picked). Picking
(0x10150f20) opens itemctrl through 0x1013a7a0 (start 1, max the stack, callback 0x10150ee0); a stack of one goes in at
once.

**Quantity (itemctrl, 0x1013a5f0).** Down -step (at least 1), Up +step (up to the stack), Left 1, Right the stack;
Confirm sets the box, Back returns. Holding a key 60 ticks raises the step from 1 to 10 (0x1013a540; here 30 key
repeats, INFERRED).

**Gil (moneyctr, 0x10140af0).** Value +0x28, max = held gil +0x24, digit step +0x20. Down -step when the value allows,
Up +step capped at the max; Left a digit lower (at the units digit the value goes to 0); Right a digit higher while
max / step >= 10, else the value goes to the max. Confirm sets box 0 (ItemNo 0xFFFF, slot 0).

**Range.** The refresh (0x101517e0) cancels and closes at 6 yalms or more: squared 3D distance >= 36 (0x101523c0,
constant 0x103344b4 = 36.0).

**Messages.** The request logs string table 10 (file 50) #20 "You offer to trade with %s." (0x10151a00, 0x10152e20(10,
20)). Client-side checks use standard messages 25 already sent, 26 cannot trade now, 28 "You cannot trade with that
target.", 29 "Select a target to trade with.", 32 "Unable to perform action in Mog House.". There is no /trade command
(not in the command table or the usage texts, d_msg 55687): trade starts from the menus. Untraced: when "Cancel trade
transaction?" (file 52 #44) shows; the help lines (0x101c87e0 indexes 7 / 11 are cursor animations); whether moneyctr
draws a digit cursor.

### 7.11 Auto-translate in the chat line (2026-10-21; goal 39; `src/ui/game_ui_autotranslate.cpp`)

**Tokens** (DATA + CODE; LSB `src/map/autotranslate.cpp` agrees). A token is 6 bytes, `FD t l a b FD`, and the game
refuses one with a zero byte (CODE 0x1018fa95..0x1018fac0 copies a token only when its 4 inner bytes are non-zero):

| t | l | a b | Meaning |
|---|---|---|---|
| 02 | 02 (English) | group, phrase | a phrase of d_msg 55665 (`02 02 gg 00` names the group) |
| 07 / 09 / 0A | 02 | item id hi, lo | an item; a zero high byte is sent as 09 02 FF lo, a zero low byte as 0A 02 hi FF |
| 13 / 15 / 16 | 02 | key item id | a key item, with the same escapes |

Phrases may name another table as "@<kind><hex>": @J job (55467), @A area (55465), @C spell (55702), @Y ability or
weapon skill (55701), e.g. "@C2B" Protect, "@A68" Jugner Forest (713 @Y, 311 @C, 237 @A, 22 @J in this install). 148
phrases are a single space (the Controller / Keyboard groups' button glyphs); they are left out of the lists.

**The input line** (CODE). The chat line object (0x101c63c0, built over the IME / auto-translate engine "menu fep",
global 0x1063027c, 0x2224 bytes, ctor 0x1018ed50) keeps its text at +0xc2c with the length at +0xc70; inserting a
token writes FD, the candidate's 4-byte id (entry +0x14) and FD (0x101ac9e9..0x101aca2a). Its length check counts the
tokens (0x101acd80: the line must fit `len + 6 * (tokens + 1)`). The client: 120 bytes, a token counted whole, the
cursor stepping over a token and Backspace / Delete removing it whole (`dat::chatedit`).

**The window "menu fep"** (CODE, 2026-10-22; class vtable 0x103368f0, object 0x1063027c, 0x2224 bytes). One candidate
window serves the Japanese IME's conversion candidates and the auto-translate completion (the engine at 0x10630220,
vtable 0x10337a98: op 0x101b18b0 calls ImmNotifyIME(NI_SELECTCANDIDATESTR) with an IME, else in auto-translate
candidate mode (+0x8c81, index +0x8c84, count +0xec28) turns the window's ops into its own keys: 1 / 2 Tab back /
forward, 5 Enter, 6 Esc (0x101b58c0)). Layout 96x134: element 1 at (3, 4) is a top row, elements 2..8 at (3, 20 + 16 k)
the 7 candidate rows.
- **Draw** (0x1018f460): the top row has a mark (Shift-JIS 81 A3) at the window's x + 9 and "index/count" ("%d/%d", 1-based;
  "0/0" when empty) at x + 39, y + 2, in 0x80808080. Then one page of 7 (`index / 7 * 7` .. + 6) at x + 4, y + 18 + 16 k;
  a candidate is "text" or "text 0x06 note", the note drawn after it in 0x80404040; one wider than the window - 8 is cut
  to fit and given "..." (0x1036c9b4). The cursor is the layout's element (index % 7) + 2 (0x10118ce0): that sets the
  element's state 3 (0x101198a0) and puts the window's cursor sub (type 6) on it (0x101180a0 -> 0x10119d60). In most
  menus that sub is the hand "anc anc_s"; "menu fep"'s is its own shape 2, a 92x16 bar (texture "menu gauge" at
  48,16, an orange gradient peaking mid-row; rgba 54545420, alpha blend), so the candidate row is highlighted, with no
  hand (issue #102). The client draws it at the element's (3, 20 + 16 k) under the text. INFERRED: stretched to the
  window's width - 4 when the window is wider than the layout's 96.
- **Size and place** (0x1018f2b0, every frame): the width is the longest candidate's, at least 90, + 12, at most 140 or
  200 by the client's language flags (200 here); the height is the layout's 134. It stands at the line's anchor plus the
  caret's x (0x1018f910) and is pushed left to keep 16 inside the screen (0x1018ef20).
- **Keys** (0x1018ef90, the window's events): the moves go to the engine's ops above (Tab / Shift+Tab, the arrows, a
  page step that walks to the next page's row), the confirm and cancel to Enter / Esc. **Sound:** only the cursor's
  se 0001 when the index changes (0x1018f43e -> 0x10037a60); no open / choose / close sounds.
- **The client** (`GameUi::atDraw` / `atKey`) draws and keys it as above. INFERRED: the window above the line (the
  anchor object at 0x10630278 + 0x14 was not followed); the candidate order (phrases in group and file order, then
  items and key items by name) and the categories with nothing typed (the dictionary's 42 groups as "{Greetings}", then
  a group's phrases, Right / Left in and out), which the engine's list builder (+0x18) was not followed for.

**The brackets** (CODE 0x1011c870 / 0x1011cdf0): a token shows between the gaiji characters EF 27 and EF 28, whose JIS
index (0x211d / 0x211e) falls in the "font usgaiji" range: shapes 8 and 9, a 4x16 quad from the textures "menu
markgree" and "menu markred", 8 wide. The client's text renderer draws any such code from "font usgaiji"
(`UiRenderer::gaijiShape`), so the brackets are the game's own green and red marks.

Not this: "menu fp_cat" / "fp_cat2" / "fp_info" / "fp_help" are the Alter Ego points windows
(title 130 "Categories", help 1097 / 1098 "...alter ego points"; ctors 0x1014b5a0 / 0x1014b990).

**Where tokens go.** Typed and macro chat lines keep them as FD codes for every chat mode (say, shout, party,
linkshell 1 / 2, yell, tell, emote); a command line (anything else starting with "/") reads them as their text, so
"/ma {Cure} <t>" works (`GameUi::submitLine`). Received (and our own echoed) lines keep a token as its text between EF 27
/ EF 28 (`AutoTranslate::expandMarked`; the session's printable() keeps those pairs), so the log draws the gaiji brackets
for real tokens only and a "{...}" a player typed stays plain. Plain-text uses (the chat filter, `/bugreport`) turn the
marks back into "{" / "}" (`AutoTranslate::plainMarks`); Ashita addons see the marks, as in the game. Macro lines are stored with the bytes (goal 35's mcr files).

## 8. Config (2026-10-18; RE notes config_re.md)

- **Store (CODE):** one table of 0xd0 config ids at 0x1062fbac: Set 0x10193740 / Get 0x10193760; Set clamps a value
  outside [min, max] to the id's reset value and calls the id's callbacks. Local ids are saved per character in
  `USER/<charid>/cnf.dat` (layout not resolved); a few are PlayOnline-shared. They are not the launcher's registry
  numbers. ffxi-linux keeps them as `cfg_<id>` in the `[ffxi-linux]` section of client.ini (SE / BGM volume as
  `sound_volume` / `music_volume`, window type as `window_skin`, keyboard as `keyboard`).
- **"menu configwi" rows** (element id -> page): 1 conf2win Gameplay, 2 cfilter Chat Filters, 3 conftxtc Font Colors,
  4 conf5m Windows (-> conf5win shared, conf5w1 / conf5w2 log windows), 10 conf11m Log, 5 conf3win Misc., 6 conf6win
  Misc. 2, 11 conf12wi Misc. 3, 12 conf13wi Misc. 4, 7 fxfilter Effects, 8 conf7 Mouse/Cam., 9 conf4 Global,
  13 Gamepad (starts FFXiPadConfig.exe; dimmed here).
- **Pages are grids of buttons**, one per choice, set at once (no left / right cyclers). The current choice gets
  windowps sprite 387 at (x + 0x25, y + 4) (412 for three-way groups); sliders draw sprite 384 stretched by
  (value - base) / (max - base), 0xa0a0a0a0 while being edited; select toggles editing, left / right step the value and
  set it at once (CODE 0x101966b0, conf2win 0x101971b2..). Element -> (id, value) bindings: `GameUi::cfgBindings`.
- **What each option does here** (CODE / DATA in the scratchpad notes config_gfx_re.md, config_log_re.md,
  config_misc_re.md, battle_mode_re.md):
  - Gameplay: auto-target (7) sends `/autotarget` (C2S 0x0DC flag 5; INFERRED that LSB needs it); SE / BGM volume
    (9 / 10); gamma (55..57): the game sets a fullscreen ramp (i / 255)^(1 / g), g = 1 + v / 100 (CODE 0x10016fd0 /
    0x100170e0; greyed without D3DCAPS2_FULLSCREENGAMMA, 0x10002be0); here a last full-screen pass over the finished
    frame (scene and interface) raises each channel to 1.5 / g, so the default 50 changes nothing (`gfx::GammaPass`).
    A D3D8 gamma ramp only reaches a full-screen swap chain, so a windowed game is never ramped: our default matches
    the windowed picture; a full-screen game at 50 would be brighter (exponent 0.67), not modelled; autosort (154) sorts
    the inventory's display order when its item count changes, and `/sort` sorts it, by type then id (202 = 0;
    INFERRED key, the game's [rec-8] is not traced) or by name (202 = 1), then C2S 0x03A (stack merge, 1 / s).
  - Misc.: character data public / hidden sends `/anon`; damage display (19): 1 / 2 start file 27's `damv` / `curv` /
    `misv` when a result's damage shows (messages by the byte table 0x1035caf0), their type 0x5C particles drawn with
    "menu dmgnum" digits (0-9, 10 "Miss!") in the result's log colour, 0.1 x scale per sprite unit; 2 also leaves those
    lines out of the log; shadows (58): 1 none; 2 "detailed" gives every actor within 40 of the camera the silhouette
    shadow (mode 1, CODE 0x10185bb0), which the default 0 gives only the local player (shadow_re.md §2a): the model is
    flattened along the zone light (kept at least ~45 degrees up, INFERRED) into a 128-pixel cell of a 1024 render
    texture (`SkinnedRenderer::drawSilhouettes`, after the characters, before the effects) and projected onto the
    ground triangles cut to its square (uncut, a large ground triangle smeared the cell across the floor),
    in place of `kage`, over a square of half-width max(size x 2.4, 1.1 x height) (INFERRED: the game
    fits a light-space box; where it renders its silhouette texture is not traced); weather particles (59 = 0): the
    generators under `weat` (gen+0xde bit 7, CODE 0x10047d3d) neither emit nor draw, so the cloud dome goes too (an
    event op forces them on, 0x100bad8f); character limit (60): the nearest 25..51 characters to the camera are drawn;
    other characters' armour static (165): no headgear, model 8 for body .. feet.
  - Misc. 2: clipping plane (62): fog distances x (v + 25) / 50, x 0.5 more past 500; animation rate (64): others
    re-pose on 4 / 3 / 2 / 1 of 4 frames, staggered by index; footstep effects (63 = 0): per step the zone's `fses` /
    `fser` generator ('w' walking | '0') + material + "00" and, on sand / snow / shallow water, file 0's `fmrk` (CODE
    0x100d524c), and while an actor stands on deep water (material 9) the set's `i900` wake runs on it (CODE 0x100d6400
    / 0x100d6480; kept alive 10 ticks at a time, so it ends when the actor leaves the water; the game's mounted
    exception, actor+0x878 == 3, is not applied); macro palette (157: Full = the 862-wide "mcr1long" /
    "mcr2long", forced compact under a 1024-wide UI; 158: shifted 130 right always (Static) or while a target is
    shown (Dynamic, INFERRED trigger)); aspect ratio (67: 0 4:3, 1 15:9, 2 15:10, 3 16:9, 4 16:10, 5 5:4, table
    0x1034f4a0, CODE 0x10002e60 -> 0x10002e80 -> 0x10015360): the projection takes that shape (x 0.75 / 0.791 as
    before) instead of the window's, so a ratio unlike the window's stretches the picture as on a monitor of another
    shape; until the option is first set the window's shape is used (`gfx::configuredAspect`).
  - Misc. 3: own equipment look (191) sends `/lockstyle on | off`, and each zone-in re-sends 0x053 Mode 1 / 0 (CODE
    0x101a7c80); status icon timers (195) = `/statustimer`; weapon effects (190): a weapon model's `!w01` / `!w11`
    run when its character engages, `!w02` / `!w12` when it disengages (CODE 0x100cf030; 190 = 1 skips the first pair);
    target effect (153 bit 7 = 0): a new target pulses brighter 3 x 40 ticks (CODE 0x100cb8a8; here by raising its
    material colour, the game moves the fog colour); party member status icons (194): the targeted member's effects
    (S2C 0x076) in rows of eight 14-unit cells above the log at x + 200, its name above (CODE 0x10227fa0). AoE range
    graphics (192 = 1, default 0; CODE 0x101582b5 -> 0x1009ad10 / 0x1008f290): while a sub-target cursor is up for an
    area spell, everyone within its radius of the cursor's actor gets the target pulse too (with 153 bit 7 = 0). The
    radius: file 81 "mgc_" record +0x46 indexes the game's table 0x10364498 {0, 1, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16,
    20, 25, 30, 255} (clamped to 40), +0x47 non-zero marks area spells (CODE: the cursor's fill callback 0x1022b440 reads
    +0x46 / +0x47 into the area record it hands 0x1022cdf0, which sets the cursor's +0x75.. fields; abilities with a
    +0x53 radius search the same table, 0x1022b83b; DATA: every -ga / -ra spell has 8 / 1 = 10 yalms).
    `/areaeffect` (`/aoe`) [on | off] sets 192 (else a toggle) with 55648 #297 / #298 (CODE 0x100792d0). Here only
    `/ma "Name" <st...>` lines raise a sub-target cursor (the Magic menu casts at once), and abilities are not covered.
    Furniture camera collision (153 bit 6 = 0, default on; CODE 0x1016bc33): the camera ray also stops at the Mog
    House furnishings, here full-height boxes over their footprints (`client::mog::furnitureCameraBox`). The game
    (0x1016bc43..0x1016bcbd, Mog House only, not while arranging) walks the actors (0x10085170 / 0x100851a0), takes each
    furniture actor's model (+0x5c4) mesh (+0x30) at its position +0x734 / rotation +0x744 (0x100c4af0) and tests the
    ray against it (0x1016c4e0): the piece's own shape, which our boxes approximate. Icon pattern (Misc., 153 bit 2): status icons from file 39557 (type one) or
    87 (type two).
  - Misc. 4 chat filter (206: 1 term filter, 2 player filter, 0 off; CODE 0x100da510, caller 0x100e385c): the game
    reads the player's `prohibited.txt` / `allowed.txt` (UTF-8, <= 0x23f0 bytes, one entry per line, <= 31 characters;
    leading flags `(n)` a player name, `(i)` / `(s)` not decoded; `.`, `[a-z]`, `[^..]`, `\` escapes, 0x101bd430) and
    checks shout, yell and assist lines (kinds 1, 26, 34, 35; table 0x100e3cac). A match in allowed.txt wins. Mode 1
    drops the line (return 1 -> 0x100e3c94); mode 2 keeps it with the text " ***This message has been deleted.***"
    (55648 #345, 0x100e3870). Both modes match alike (0x100da510 -> 0x101bca60(sender, normalised text) ->
    0x101bcb30, the sender up to 16 characters, the text 0x400): `(n)` entries test the sender (as a whole name,
    INFERRED), the others the text; only the result differs. Here the files sit beside client.ini; both modes stay dimmed while prohibited.txt has no
    entries (as 0x101bca80), and choosing any of the three re-reads the files ("Pressing this button will reflect your
    edits", 55651 #1088 / #1090). `client::ChatFilter`.
  - Global profanity filter (24 = 0 on, default; CODE 0x101ba010 -> 0x101b9f40): the game's word lists are
    PlayOnline's `vulgar2.dic` / `entryz.dic`, encrypted or compressed (7.99 bits / byte) and read through pol.exe, not
    decoded. Here the player's own `profanity.txt` beside client.ini (the same entry syntax) masks matches in incoming
    chat with `*` (the game's mask is SJIS 0x819B or `?`, INFERRED); without that file it does nothing.
  - Software keyboard (201): dimmed. It is PlayOnline's on-screen keyboard for typing with a gamepad; SDL's own text
    input (and its on-screen keyboard where the platform has one) stands in, and nothing here could draw the
    PlayOnline one. Auto-disconnect (65): logs out after 10..60 minutes without input
    (the game leaves it to PlayOnline, INFERRED).
  - Mouse/Cam.: inversion (139..142); edge-of-screen rotation (143 = 0, default on) turns the camera while the cursor
    is within a pixel of the window's edge; locked behind (145 = 1) swings the camera 2.5 % a tick toward behind the
    character after 4 ticks without camera input, when -0.5 < dot(view, facing) < 0.99 (CODE 0x1001f9d0); movement
    reference (144): dimmed, because this client has no mouse-driven movement for it to steer. Traced for when it
    does: on a mouse button press with no other held (0x10124860..0x10124a75) the reference point is the cursor (0)
    or the window centre (1, 0x101261d0); while the move actions are held (0x101239a0 actions 0x8b / 0x8c) the cursor's
    offset from it is a stick: x = dx / (width / 21), y = dy / (height / 21), clamped to +-1 (0x10126030 /
    0x10125f10), and atan2 of it picks one of 8 directions (0x101257f7).
  - Effects ("menu fxfilter", 18 rows, texts 55650 #153..170): bits 1..16 of cfg 146 hide a battle action's generators
    (scheduler op 2 with an action, CODE 0x100599f0 -> 0x100ade10) by source -> target: you (or your pet), a monster,
    a party member, anyone else (0x100adec0); bit 0 limits that to while engaged; bit 17 (screen shaking) has nothing
    to act on (no camera shake here).
  - Windows: window type (18), multiple log windows (180: window 2 above / right of window 1), timestamps (181:
    "[HH:MM] " / "[HH:MM:SS] " in the basic system message colour; `/timestamp` toggles 2), open / close effect (164,
    selectable and saved, with no effect: FFXiMain reads it only on its Config page (all 14 constant 0xa4 sites and the
    store slot +0x1c38 checked; registration kind 4 = local cnf.dat), so the effect, if any, is not in the game client). Log windows 1 / 2 ("menu conf5w1" / "conf5w2", ids 14..17, 156 / 182..184, 197, 198): auto-resize,
    max / min lines, column width (352 + (UI width - 512) x v / 100 within [352, UI width - 160]), interval; with
    auto-resize a window shrinks one line per interval toward its min lines once text stops (INFERRED pacing).
  - Font Colors ("menu conftxtc" -> "textcol1" list + "textcol2" sample -> "textcol3" R / G / B bars, +-4): 28 ids
    (27..49, 155, 176, 179, 203, 204) as 0x80RRGGBB; reset via "menu confyn". Chat lines take their channel's colour;
    battle lines the colour of their message and who acted on whom (seven per-message tables, CODE 0x1009e630; only
    "target is you", "actor is you" and the rest differ: `client::battleColour`).
  - Log Settings ("menu conf11m" -> "conf11l" -> "conf11s"): 64-bit masks per window (185 / 186, 187 / 188); a set bit
    shows that line kind in the window; lines without a bit go to window 1 (CODE 0x10129ae0).
- **Chat Filters** ("menu cfilter", here a list): 55 rows from table 0x10379a28 {server bit, log-only bit, label in
  file 55650}; server bits 0..29 = MessageFilter, 30..61 = MessageFilter2 (LSB filters1_t / filters2_t); log-only bits =
  cfg 52; the last three rows set the system message level (SAVE_CONF byte 1 bits 3..4). The state comes from S2C 0x0B4
  (SAVE_CONF @4); a change sends C2S 0x0DB Kind 0 with ConfigSys = that SAVE_CONF head, filters / level replaced (the
  game sends it when the page closes; here at once). LSB stores them (chars.chatfilters_1 / _2) and answers 0x0B4.
  The log-only bits hide other players' say / shout / tell / party / linkshell / emote / unity / assist lines in the log.
- **Key assignment** (Misc. 2 "Key Assignment": 8 Movement -> "menu k1assign", 9 Commands -> "menu k2assign",
  10 Default -> yes / no with 55650 #143 "Reset keyboard assignment to default?"; CODE 0x10196a3e..0x10196aa3,
  0x10196ad0): both lists are 14-row scrolling windows (366x248, rows `menu cfilter` at x 34, help 277 / 278, title
  69 "Keyboard"). A row draws "[  ]" at x + 4 with the key top centred at x + 0x13, and its label (55650) at x + 0x40
  (CODE 0x1019c180). There is no select step: pressing a key assigns it to the row under the cursor (CODE 0x1019c3c0;
  the keyboard hook 0x1019c3a0 stores the DIK, the next frame applies it). Details, rows and saving:
  input-format.md §5. Here: `GameUi::Kind::KeyAssign` (param 0 / 1), keys from `GameUi::assignKey()`.
- **Not done:** the Gamepad page, shadows "detailed" (58 = 2, a
  silhouette rendered along the light; drawn as simple), the deep-water wake, the dimmed options above.

### 8.1 Add-ons (this client's row, goal 47, 2026-10-22)

Not a retail page: Ashita addons (goal 31, ashita-compat.md) managed from the Config menu.
- **The row:** "menu configwi" gets an element 15 one row (16) under Gamepad (13); the window grows by 16 and the
  up / down wrap goes through it (`GameUi::layout`, cached as `stacked_["configwi+addons"]`). It is drawn as the other
  rows' button (the first two quads of windowps 359, texture "menu buttonto") with "Add-ons" spelled in the buttons'
  own letters (#84, `ui/button_label.h`). DATA: the game has no sheet for those small bold italic letters; each label
  sprite cuts them out of "font font" (its top-left block, ASCII in 13-texel rows), a shadow pass (+2, +2, colour
  7f7f7f20, blend [1 2 0 1]) then the letters (7f7f7f7f, blend [1 0 2 1]); windowps 359 is Global (quads 2-7 the
  shadow, 8-13 G l o b a l). Tall letters start at the label's top, short ones 2 lower, the hyphen 4 lower with one
  texel before it (windowps 109 "On-screen damage display"). `buttonLabel` copies the colours, blending and origin
  from 359 and rebuilds "Global" quad for quad (test_button_label.cpp). Confirm returns `(GameUi::kAddonsTag, 0)`
  with Config left open. The pages below use the game's list windows and moji font, like its own item lists.
- **Pages:** "menu inventor" list windows fed by `GameUi::openAppPage` (a page stays open when a row is chosen, cancel
  pops one window and reports -1, Left / Right on an adjustable row comes back through `takeAppAdjust`; the title
  window shows the page's name). apps/xi-client/addons_page.cpp:
  - **Add-ons:** "Ashita release" (Auto / Latest / HorizonXI; Left / Right or confirm saves `ashita_version` and
    restarts the addons with the same ones loaded), then each folder of `<ashita_dir>/addons` with a main file: right
    column On / Off, `*` = starts at login, Error after a failed load or a callback error; the help line has the
    header's description, version, author and "needs HorizonXI" (it requires bitmap / color / lin / typedefs).
  - **An add-on:** Load / Unload, Reload, Start at login, Settings (count of files), Run /<name> (its own command),
    Information (header to the log), then its last errors (`AddonManager::errors`, at most 20, kept after unload).
  - **Settings:** the settings lib's file for this character (`config/addons/<addon>/<Char>_<id>/*.lua`, else
    `defaults/`), File switches between several; fields sorted by key (addons rewrite the file in table order). Bools
    toggle (confirm / Left / Right), numbers step by 1 (0.1 if decimal), colours show as 0xAARRGGBB; numbers and strings
    are typed: confirm puts `/addons set ADDON FILE KEY ` on the chat line, and the page reopens after. A loaded addon
    is unloaded before its file is written and loaded again (it saves its settings on unload).
- **Start at login:** `~/.config/ffxi-linux/addons/<Character>.txt` (one name a line, `#` comments), loaded once the
  character is in a zone, after `ashita_load` / the boot script.
- **Commands:** `/addons` opens the page; `/addons set ADDON FILE KEY VALUE` writes a setting (hex 0x... for colours).

## 9. Battle UI pieces: target arrows, KO time left, nameless NPCs (2026-10-18, goal 16)

Tags: CODE (FFXiMain.unpacked.dll) / DATA / INFERRED.

**Target arrows (CODE 0x1014ffc0 update, 0x101500d0 draw; both target-window vtable 0x10334f18).**
- Frames: shared sheet 6 = "anc anc_l" (0x101509a0 copies its shape list to window+0x78, count +0xb9). DATA: six sprites,
  each one quad 20 x 32 at x -10..10, y -32..0 from texture "anc anc" (a rotated 32 x 20 cell), blend [1 0 1 1]; the
  bottom edge (the tip) sits on the anchor point.
- Animation: +0xbb counts down by the elapsed ticks; at < 0 it resets to 3 and the frame +0xba advances (wraps at the
  count): a new frame every 3 ticks (1/20 s), a 0.3 s cycle.
- Positions: target state [0x1057876c] holds two slots (+0 and +0x28); 0x101597b0(&x, &y, slot) projects the slot's
  entity to the screen (0x10159c70 -> entity vfunc 0x27c / 0x280). If [0x1057876c]+0xc4 != 0 (a command is waiting
  for its sub-target): slot 0 (the cursor) -> (+0xc0, +0xc2) with y - 10 and +0xb8 = 1, slot 1 (the main target) ->
  (+0xbc, +0xbe); else slot 0 -> (+0xbc, +0xbe) and +0xb8 = 0.
- Draw: (+0xbc, +0xbe) with colour 0xE0808080 (neutral: the texture's own gold) when x != 0; with +0xb8 set also
  (+0xc0, +0xc2) in 0x101504d0's colour: 0xE04040D0 blue, 0xE0D04040 red, 0xE0D0D040 yellow, chosen by the distance
  to the target (0x10027550, less both entities' +0x208 sizes) against the pending action's range (table by
  0x101cc0d0(actor+0x70)): INFERRED blue in range, red out of range, yellow at the edge.
- ffxi-linux (`actors_draw.cpp`): the white arrow over the target, the blue one over the sub-target cursor 10 units
  higher; the anchor is the screen point just above the name label (INFERRED: the game's point is vfunc 0x27c, not
  traced); always blue (no action ranges here). Test "target arrows: sheet 6 ...".

**KO window "Time Left:" (CODE 0x1012dff0 open, 0x1012ded0 update, 0x1012de20 draw; layout "menu dead").**
- Opened from 0x10096c77 with a deadline; the update keeps +0x20 = (deadline - now) x 60 ticks; the draw formats
  +0x20 / 60 seconds as "%d:%02d" (VA 0x1036f6e0) with 0x101c9330 at (left + 0x4B, top + 6), colour 0x80808080.
- Source: S2C 0x037 dead_counter1 @0x3C = 60 x the seconds until the forced home point **plus 6 minutes**
  (dead_counter2 @0x40 = Vana'diel timestamp + the same; LSB char_status.cpp: "the client uses 66 min as the maximum
  ... below 6 min the client forces the home point"); the caller subtracts 0x168 (360 s).
- ffxi-linux: `WorldSnapshot::deadCounter` / `deadCounterAt`; shown = counter / 60 - 360 - seconds since it arrived,
  as m:ss at (75, 6). Live: 59:53 then 59:50 three seconds later (BotD with HP 0).

**Nameless NPCs.** LSB's npc_list names NPCs whose retail name is empty 'blank' (polutils name '', 220 rows) and sends
that name in 0x00E. The game draws entity+0x7c as it is (target window 0x101500d0), so retail shows nothing. ffxi-linux
drops a 0x00E name of exactly "blank" (no label; an empty target name) and no longer shows "#id" for nameless
targets (INFERRED: what LSB should send is the empty polutils name).


### 9.1 Battle UI and controls, goal 23 (2026-10-18)

- **Enter with nothing targeted (issue #72, 2026-10-23).** The target object's key handler (CODE 0x10158520) asks
  0x101583a0 whether there is a current target. With one, Enter (target command 1) confirms on it (0x10158ce0(0),
  sound 0x0e). With none, it runs target command 1 in 0x10158720: 0x10083550 moves every actor that passes the filter
  0x10083360 (flags from target object +0x60 / +0x64) into our frame (matrix from our position and heading, 0x10028160)
  and keeps the nearest with local x > 0 (in front of us, 0x103295d8 = 0.0) under a squared distance of 2500.0
  (0x451c4000, 50 yalms); 0x10157a70 targets it. When nothing is picked, Enter opens "menu playermo" (0x1015e260 with
  0x1036f5c4; "menu mp_pmode", 0x1036f850, while 0x10097c80 holds: [0x104dfd98]+0x40db6 != 0 or [0x10485640] == 0x17,
  not modelled). Enter never opens the chat line; Space and "/" do. xi-client: the filter flags are not decoded (Tab's
  filter stands in, INFERRED), and the menu opens in mode 2 (6 engaged), INFERRED; with no target its rows are Chat,
  Magic, Abilities, Trust, Items, Trade, Check, as in the user's retail capture.

- **The player action menu (goal 37, 2026-10-21).** Enter on a player, ourselves included, opens playermo mode 2
  (candidates 4 8 7 25 9 19 16 12 22 24 27 10). The filter (CODE 0x1012ae60, jump tables 0x1012b1d8 / 0x1012b19c;
  returns 1 = keep) per code:
  - 4 chat (0x1012af03): no target keeps Chat; an NPC (id & 0xFF000000 == 0x01000000) drops it; any other target,
    ourselves too, rewrites it to 5 Tell (help 23; the same keytops3 1 "Chat ▶" label).
  - 10 examine (0x1012ae9f, help 29, label keytops3 19 "Check"): kept unless 0x1008e080 (actor +0x1ff == 1 with
    +0x1d0 bits 0 / 8 clear); on ourselves, with the call's second argument 0, 0x10177640 (0x105ff404 -> 0x101814e0)
    and zone != 211 (0x101cbed0) turn it into 13 Fish. 11 check (0x1012aee2) adds a drop for +0x1d0 bit 8.
  - 16 invite (0x1012af6d): dropped when 0x10201ab0 (party slots at 0x10630fb8, 0x7C apart: exactly one failing
    0x10201a60 and at least one passing); on ourselves (or no target) kept only for our own +0x2a0 actor with
    +0x170 != 1; another player kept by its +0x124 bit 20 or 21.
  - 19 trade (0x1012af27): dropped while 0x101cbf00(0, 0) (our +0x1d0 bit 10) or with an NPC targeted; kept for
    ourselves and other players.
  - 27 seek party (0x1012b178): kept when the target's +0x140 bit 3 is set. 7 8 9 25 always kept; 12 treasure needs
    a pool (0x1013ef20).
  So on ourselves (solo, no pool): Chat ▶, Magic ▶, Abilities, Trust, Items, Trade, Check, as retail shows it.
- **Right in the battle menu (CODE 0x1012b755).** Calls the row code's third table field (0x1036f250 + code * 12 + 8):
  codes 4 / 5 -> 0x1012aa10 opens "menu chatctrl", 8 -> 0x1012aa30 opens "menu magselec" (codes 2 / 3 -> 0x1012abf0, a
  flag toggle). The ▶ on Chat and Magic is part of their label sprites (keytops3 1 and 7 carry a keytop arrow).
- **"menu chatctrl" (confirm CODE 0x1012d1e0, rows dispatch 0x1012d34c).** Rows: Say, Tell, Party, Linkshell,
  Linkshell2, Unity, Shout (labels keytops3 28 31 34 37 518 520 43), each 0x101ad220(mode, name) on the input object
  0x10630284 with game modes 2, 4, 5, 6, 12, 11, 3; Linkshell / Linkshell2 only with a pearl (0x100e3660 /
  0x100e9660). Tell's name (+0x14: -1 = the current target, entity +0x7c or +0x214) is drawn after element 8's arrow;
  the window then closes (0x1015e820). ffxi-linux maps the rows to its /chatmode kinds and Tell to "/tell <name> ".
- **"menu magselec"** has the rows of "menu mgcmenu" (help 303..307, 637, 698) without Trust; ffxi-linux treats it
  as MagicMenu.

- **Arrow anchor (CODE 0x10159c70 -> actor vfunc 0x27c = 0x100d0620).** vfunc 0xac (0x100a49e0) returns the byte at
  actor+0x8a6 (an attachment point index), vfunc 0x1c4 (0x100d4490) gives that point's position, y += the float at
  actor+0x8c0, then vfunc 0x280 projects it. With a ground target (0x10159bb0) vfunc 0x280 is used on the point less
  0.5 instead. Actor vtable 0x1032d710. ffxi-linux still uses the model height + 0.25 (INFERRED).
- **Range colours (CODE 0x101504d0 -> 0x10150540).** Args: player entity, target entity, the player's pet
  (0x101cc0d0: entity table 0x10480af0[actor+0x1fa], the pet index), the range record at target state +0x6c
  (0x101576f0), colours in-range / out / edge. The record is two 4-byte entries {kind, range, flat?, sizes?}: kind 1
  measures player -> target, kind 2 pet -> target (player when no pet); byte 3 = 0 drops the two entities' +0x208
  sizes; byte 2 = 0 zeroes y (a flat distance). Out (red) when distance - sizes > range + 0.5 or distance >= 25; edge
  (yellow) when > range - 0.5; else in range (blue). Who fills +0x6c was not traced; file 86 (spells) and 85
  (abilities) hold no range byte (DATA: Cure's record is id, type, element, targets, skill, MP, times, levels, id,
  icon, names, descriptions, zeros). **Where the ranges live (CODE + DATA, 2026-10-19):** 0x101576b0(records, count)
  copies the records into the targeting object (0x1057876c) +0x6c when a command starts its sub-target cursor:
  spells (0x1022b3b0) {1, range, 0, 1} with range = table 0x103644b8[file 81 "mgc_" +0x45] (u8 pairs: 0 1 3 4 5 6 7 8
  10 12 14 16 20 25 30 255); weapon skills (0x102232c0) and job abilities (0x10222f40) the same from file 81 "comm"
  (type 0x53, 0x30-byte records by command id: weapon skills 0.., job abilities 512 + id, pet moves above; rotated as
  file 86; +0x10 range index, +0x0E sub-list: 6 Blood Pact: Rage, 10 Ward, 18 Ready); for a command whose +0x02 is 6
  or 10 the player range comes from 0x25b / 0x2ac and a kind-2 (pet) record holds the command's own. Typed commands
  without an action use the default {1, 30, 1, 1} (0x10157780, data 0x10335240). 0x10329a08 = 0.5, 0x1032cf58 = 25.0.
  Cure, Stone 20 yalms; Fast Blade 3; Provoke 16; Mighty Strikes 255 (no limit). The hitbox size is 0x00E +0x25 / 10
  (LSB Flags2.g); players 0. ffxi-linux: GameUi::subTargetRange + the arrow in actors_draw.cpp (XI_RANGE_TRACE=1
  logs each change); checked live with BotB (BLM) /ma "Stone" <st> on NPCs at 14.6 / 19.9 / 25.5 yalms: blue /
  yellow / red, and /check <st> (range 30, 3D): blue at 20, red at 25.5. Pet (kind 2) records are not modelled.
- **Shortcut keys open their list alone (CODE 0x101adcf0, 2026-10-19).** The shortcut object registers one handler
  per action id (0x101ada30(id, handler, arg, mode)); none opens the main menu: Items 0x39 -> 0x101ae3f0 (inventory
  0x1021c300(0, 1), as the main menu's Items), Equipment 0x36 -> 0x101ae3b0 (0x10133060), Magic 0x41 -> 0x101ae5d0
  (0x1022ad80(0, 1, 0x100): "menu magic" with every spell type but trust, the third argument a mask of excluded types,
  bit 8 trust), Abilities 0x7f / Weapon Skills 0x80 / Pet Commands 0x9f -> 0x101ae560 / 0x101ae580 / 0x101ae5b0
  (0x102226f0(type 1 / 3 / 2, 0, 1): "menu ability", the list the abimenu rows open; Monstrosity: type 0x14, no pet
  commands), Attack 0x3b "/attack", Heal 0x3d "/heal", Ranged Attack 0x81 "/shoot <t>", Call for Help 0x82 "/help",
  Check 0x3f 0x101346e0(0, 1, 0), the chat rows a prefix ("/say ", "/tell ", ...). "menu ability" is "menu magic" with
  title 68 Abilities. ffxi-linux: GameUi::openShortcut. The second argument of 0x1022ad80 (1 from the key, 0 from the
  menus) skips the job / level test in 0x1022bf00 and picks the other remembered cursor; not modelled.
- **Battle menu Magic / Abilities (CODE 0x1012b8ce / 0x1012b8e7).** Magic opens the spell list directly
  (0x1022ad80(0, 0, 0x100), every type but trust); Abilities opens "menu abiselec" (16,206, title 68: Job Abilities,
  Weapon Skills, Ranged Attack, Mount, Pet Commands; no Job Traits), whose rows open the lists as abimenu's do. The
  list (16,96, 182x200) covers abiselec, which is hidden under it.
- **German / French key tops (CODE 0x101af8c0, 2026-10-19).** The key-top table (0x10630120, DIK -> character) is
  built per client language ([0x10456a28]+8: 2 German, 3 French): the US letters, digits and punctuation for all, then
  German adds pseudo codes 0xf0..0xf3 (the umlaut keys, drawn as their own glyphs) and DIK 0x1a / 0x1b as '+' / '#';
  French adds pseudo codes 0xf8..0xfd ('\xf8', '$', '*', '2', ')', '!'). 0x101afab0 maps scan codes to these pseudo
  codes (pairs at 0x1037c3e8). The Compact defaults also move for those languages (0x1019cdde: Confirm on DIK 0x2c,
  rows 0x41..0x46 to 0xfc / 0x11 / 0xf9 / 0xfa ... for German, 0xf3 / 0x15 / 0x1a / 0x1b for French). ffxi-linux runs
  the English client only (English text files, no language setting), whose key tops are the US ones whatever the
  keyboard: bindings are physical keys, so a QWERTZ or AZERTY keyboard already works as with the English Windows
  client. Not implemented; it belongs with a German / French client mode.
- **KO time, right-aligned (CODE 0x1012de6e).** When [0x10456a28]+8 (the language: 2 German, 3 French) == 3 and
  +0x18 >= 0, the time is drawn right-aligned (0x101c9700) at (right - 4, top + 6); otherwise left at (left + 0x4B,
  top + 6). English clients use the latter, as ffxi-linux does.
- **Pet Commands.** "menu abimenu" element 5 (shape 163, help 505) opens the list of learned job abilities whose type
  (file 85 +0x02) is 2 (DATA: Fight, Heel, Leave, Sic, Stay, Dismiss, Assault, Retreat, Release, Blood Pact: Rage /
  Ward, Deploy, Deactivate, Retrieve, the Maneuvers); the Job Abilities list leaves them out. LSB only grants an
  avatar's commands while it is out (the list is empty before the summon). The Pet Commands key (k2assign row 133,
  Ctrl+B) opens the list directly. Element 6 (shape 551) is Mount.
- **Pet status.** S2C 0x068 PET_SYNC (LSB pet_sync.cpp): type 4 in the low 6 bits of +4, our id @8; with a pet (size
  0x2C): pet index @0xC, HP% @0xE, MP% @0xF, TP @0x10, its battle target @0x14, name @0x18.
- **Pet display (CODE, 2026-10-19).** Not a party row: "menu partywin" picks "menu ptw{members}" without the pet
  (0x10203b2f). The party object draws a line 0x17 above its window (0x10204102): our pet (0x10202510: own actor
  0x10485f7a -> entity +0x1fa, the pet index) with its name (entity +0x7c, or +0x214 when +0x210 is set) at the corner,
  the HP gauge (entity +0xEC, HP%) at +0x1A,+7 as the rows', and its TP right-aligned at +0x30,+0xA (0x102024b0: grey
  0x606060 under 1000, blue 0x2060D0 from 1000) only when config 174 bit 0 is set (`/partyinfo showtp [on | off]`,
  handler 0x10079240, sub-command table 0x103579fc; here too; models 0xB22..0xB31 never show it, 0x10098250); then, 0x10 higher, the same line for the entity at +0x2a0
  (0x102025f0; INFERRED a fellow). The object's +0x5e "top" moves up to each line, so the target window and the windows
  stacked on it sit above them. The party cursor (<stpt>) reaches the pet as row -1 (0x10202fe0, 0x10203120 /
  0x102031e0). ffxi-linux: the pet line (name, HP gauge, TP with config 174 bit 0), the target / sub windows above it;
  no MP (the game shows none); its x is the party window's (the game also keeps it clear of another window,
  0x10204064, not modelled).
- **Own Unity.** S2C 0x061 unity_info @0x58: Faction (the leader 1..11) in bits 0..4 (LSB 0x061_clistatus.h); the
  Unity search's row 0 searches it, dimmed without one.
- **Mute list Remove.** Mentors only: S2C 0x037 Flags3 @0x38 bit 4 MentorFlag (LSB char_status.cpp). Remove sends C2S
  0x0B7 ASSIST_CHANNEL {u8 Kind 0x27, u8 0, sName[15], Mes ' '} (LSB 0x0b7_assist_channel.h); LSB also requires mastery
  rank 6. Others get 55651 #1052 as before.

### 9.2 Target lock in battle and Switch Target (2026-10-22, issue #69)

**Target commands (CODE 0x10158520 -> 0x10158720; key table DATA 0x10371ef0, 16-byte records {context, action,
mode, command}).** The target object ([0x1057876c]) turns key actions into numbered target commands:

| Keys (action) | Command | What it does (0x10158720) |
|---|---|---|
| Enter / Num Enter (0x91) | 1 | confirm on the current target (0x10083550) |
| Tab, Num 0 without Shift (0x84) | 15 | next target (0x10158bd0(1)) |
| Shift+Tab, Shift+Num 0 (0x97) | 16 | previous target (0x10158bd0(0)) |
| F1..F6 (0x46..0x4b), F7 (0x4c) | 6..11 | party member n (0x10203050) |
| F8 (0x4d) | 28 | nearest monster (0x10083e10(0x10, 0)) |
| F9 (0x4e), F10 (0x52), F11 / F12 (0x53 / 0x54) | 21, 18, 13 / 14 | 0x10083df0, 0x10083e30(0x10, 0), alliance rows |
| Esc (0x92) | 17 | clear (0x10158f20) |

**The lock (target object +0x5c bits 0..1; setter 0x10157590(mode)).** While it is set and no sub-target cursor is up
(+0xc4 = 0, +0x59 = 0), every target command but 1 (and 5) returns at once (the test at the top of 0x10158720): Tab,
Shift+Tab, the F-keys and Esc leave the target alone. With a sub-target cursor up, the same commands move the cursor.
- Set by /lockon (command id 0x5c, handler 0x1007a630: toggles 0 <-> 1), by key action 0x30 (Num *): the target
  object's per-frame key check 0x10158110 tests action 0x30 in context 0x3e and calls the setter with
  `(+0x5c & 3) == 0`, a toggle. That is the only field use of action 0x30 (the one other test, context 0x3f at
  0x1024beb1, toggles a flag of the object at [0x106368dc], not a heal), so Num * is Lock Target, not Heal (issue #116;
  Heal stays on Ctrl/Alt+H). Also set by S2C 0x058 ASSIST when +0x51 is set (0x1009fe00; +0x51 = 1 when config 7, auto-target, is set to 0,
  0x101aa9ec). Turning it on snaps the target to the battle target (0x10081480 on the handle 0x1047d600, INFERRED to be
  ours) and tells the camera (0x10159350 callbacks).
- Cleared when the battle target dies or goes (0x10157620: none, or its status 2 / 3; per frame at 0x10157969) and when
  our status leaves engaged (0x1008b722: status not 1 / 3). A sub-target cursor saves the mode in bits 2..3 and
  restores it afterwards (0x10159370, 0x10158f70).
- The setter (0x10157590) turns it on only with a target that is there and alive (0x10157620), and tells the camera
  (0x10159350 mode 0 on / 1 off), so the one lock is the target lock, the camera lock (/lockon in dat-format §7.8b) and
  the facing together.
- The target window draws a lock shape while the mode is set (§5.2): this+0x5c, loaded at 0x101507a0 from sheet 1
  (menu windowps) +0x350 = **sprite 212**, at the window's top left.
- Not found: a lock set by engaging itself. ffxi-linux follows the user's retail observation instead (INFERRED):
  engaging a living monster locks on it (Tab, F1..F6 and Esc do nothing) until it dies or we disengage. Typed commands
  (/target, /ta, /assist), macros, addons and Switch Target still change the target.
- ffxi-linux (issue #116, `client/target_lock.h`): one lock for the target, the camera and the facing. Num * and
  /lockon toggle it (also while engaged: unlocking frees Tab / Esc, the camera and the character's turning; locking
  again needs a living target); engaging sets it once, on the engage edge; disengaging or the target dying / leaving
  clears it. Locked, the camera looks at the target (dat-format §7.8b /lockon) and the character faces it, stepping
  back / sideways relative to it; standing, it turns to face the target. Unlocked (INFERRED from retail play): the
  character runs where it is steered and does not turn by itself, also while engaged (LSB refuses melee rounds while
  we are not facing the target). No chat line on either toggle (none found).

**Switch Target (battle menu code 18, confirm CODE 0x1012b9b2; help 0x25).** No filter (0x1012ae60 keeps it in mode 6).
Confirm sets the default range record {30 yalms, 3D} (0x10157780) and starts the sub-target cursor (0x10159370) with
valid-target mask 0x20 (monsters; the mask table DATA 0x103720d0 maps the spell target bits 0x01 self, 0x02 players,
0x04 party, 0x08 alliance, 0x10 NPCs, 0x20 monsters, 0x40 / 0x80 to the cursor's filters) and the callback 0x1012bda0,
which runs "/attack %d" (DATA 0x1036f5f8) with the pick. Attack (code 1, 0x1012b7e1) runs the same for the current
target. LSB answers /attack on another monster while engaged with ChangeTarget (C2S 0x01A category 0x0F: the battle
target moves at once, in range or not; 0x01a_action.cpp, ai_container.cpp).
- ffxi-linux: `GameUi::beginSubTarget(line, token, targets, d)` starts the cursor for any command (the typed <st> tokens
  use it too; issue #68 builds on it); Switch Target is `beginSubTarget("/attack <stnpc>", "<stnpc>", 0x20, d)`. The
  cursor takes the arrows and Tab / Shift+Tab. `/attack [target]`: not engaged = engage; engaged, another monster =
  ChangeTarget (the target and the lock follow); the battle target = disengage.
- Live (BotA, WAR20, West Ronfaure): engaged on a Wild Rabbit, Tab, Esc and F1 kept the rabbit ("target: locked on
  the battle target"); Enter -> Switch Target -> Tab moved the cursor to a Tunnel Worm -> Enter: the target window
  showed the worm and the server answered "The Tunnel Worm is out of range." (the battle target had moved to it).

### 9.3 Spells, abilities and weapon skills from the menus pick their target (2026-10-22, issue #68)

The lists (main menu Magic / Abilities, the battle menu's Magic / Abilities, Ctrl+M / Ctrl+J / Ctrl+W, Pet Commands)
never act at once: a confirmed row raises the sub-target cursor on the action's valid targets, and the pick runs the
action as a typed command.

- **Spells (confirm CODE 0x1022c300).** A spell the player cannot cast (0x1022ca90) prints its reason (table
  0x10386900) and stops. Otherwise the cursor mask comes from 0x10102210(targets mgc +0x06, type +0x02, +0x44, our
  statuses): the spell's own valid-target bits, except that a song (type 5) under Pianissimo (status 409) or a
  geomancy spell (type 7, +0x44 bit 0x20 clear) under Entrust (584) whose bits are 0x01 (self only) gets 0x05 (self and
  party). 0x10159370(callback 0x1022cca0, mask) raises the cursor; when nobody qualifies it prints message table
  0x103763b0 entry 0x16 (55646 #90 "You must specify a valid target for that spell.") and the list stays. Otherwise
  0x10159410 sets the cancel callback 0x1022cd20, the range record is set (0x1022b3b0, §9.2 / subTargetRange) and the
  list, its sub window and the target window are hidden (0x1015e5f0). The pick (0x1022cca0) runs `/magic "%s" %d`
  (DATA 0x10386970) through the command line (0x10080cf0) and closes the list (0x1015e6a0); Esc (0x1022cd20) shows
  the hidden windows again, so the list comes back where it was.
- **Job abilities, weapon skills, pet commands (0x10223d50..).** The command record (0x10223cc0, file 81 "comm",
  0x30 bytes): +0x0E non-zero opens a sub-list instead (0x102226f0); else the cursor takes the mask at +0x0A
  (0x10223d9e; DATA: Fast Blade 0x20, Provoke 0x20, Mighty Strikes 0x01) with callback 0x10224a50, which runs
  `/weaponskill "%s" %d` (0x1038621c), `/monsterskill "%s" %d` (0x10386204) or `/jobability "%s" %d` (0x103861f0).
  Nobody to pick: entry 0x1b, 55646 #93 "You must specify a valid target for that ability.". The cancel callback is
  0x10224b80.
- **Where the cursor starts (0x10159100).** On the current target when it passes the mask, else (target command 1, or
  5 when the mask allows ourselves) the first candidate; ourselves included. Self-only actions (Berserk, Blink)
  still raise the cursor, on us: Enter confirms. No action skips the cursor.
- ffxi-linux: `GameUi::menuSubTarget(command, name, targets, d)` hides the menus (kept for Esc), raises
  `beginSubTarget("/magic \"Cure\" <st>", "<st>", mask, d)` and the pick runs the line with `<id:HEX>` as a typed
  command would. Masks: spells from file 86 (+ the Pianissimo / Entrust rule; the +0x44 flag is not read, INFERRED to
  be clear), abilities and weapon skills from `AbilityTable::commandTargets` (comm +0x0A; file 85's bits when it has
  none). The weapon skill rows need TP 1000 and being engaged, but no target: the cursor picks it. Typed commands with
  their own targets and macros are unchanged. Not done: the comm +0x0E sub-lists (Blood Pact rows open them in the
  game) and the /monsterskill rows.
- Live 2026-10-22: BotA (WAR20, West Ronfaure) Ctrl+J -> Berserk -> the cursor on BotA -> Enter: "BotA uses
  Berserk."; target a Wild Rabbit, Ctrl+J -> Provoke -> the cursor on the rabbit -> Enter: "BotA uses Provoke on the
  Wild Rabbit."; Ctrl+J -> Mighty Strikes -> Esc: the list back with its cursor on Mighty Strikes; engaged at TP 1229,
  Ctrl+W -> Fast Blade -> the cursor on the battle target -> Enter: "BotA uses Fast Blade. The Wild Rabbit takes 120
  points of damage.". BotC (WHM) in a party with TestA, Southern San d'Oria: Ctrl+M -> Cure (mask 0x3f) -> the
  cursor on BotC -> Right -> TestA -> Enter: "BotC starts casting Cure on TestA."

### 9.4 Status icons wearing off (2026-10-23, issue #82)

The HUD strip of our status icons ("menu buff" layout; the window object in 0x10634694 / its state at +0x288..+0x2f0+,
icons u16[32] at +0x290 and expiries u32[32] at +0x2f0, filled from S2C 0x063 type 9 by 0x10227750, 0x7FFFFFFF = no
timer).
- Draw of icon i (CODE 0x10227ad0): colour 0x80808080 unless cfg 195 (Config Misc. 3 "status icon timers", default
  on; 0x10193760(0xc3)) is set, the icon has a timer, and its time left `(expiry - now*60 + 59) / 60` (whole seconds,
  rounded up; now from 0x10103910) is **15 or less**: then the colour is `0x80vvvvvv`, `v = 0x40 - (int)(p * -64.0)`
  (0x1033c280 = -64.0), so R = G = B run 0x40..0x80 (half to full brightness) and **alpha stays 0x80**. The icon is
  drawn at (x + 1, y + 1) with scale 1.5 (0.75 for the other icon pattern) by 0x10026a00.
- The phase p (+0x288) is shared by every icon in the strip, updated once a frame in 0x10227750 (0x10227983..0x102279dd):
  the counter c (+0x28c) is turned into a triangle `x = c > 20 ? 40 - c : c` (20.0 at 0x10329ce0, 40.0 at 0x10331380),
  `p = x * 0.05` (0x1032a3e0), then c drops by the frame's 1/60 s ticks (0x10014cb0: [0x104568fc]+0x28, at least 1)
  and starts again at 40.0 (0x42200000) once below 0. One cycle is 40 ticks, 2/3 s: dim -> full -> dim.
- The timer text under the icon (0x10227be0) is gated by the same cfg 195 and shown above 5 s left.
- Party members' status icons (cfg 194, S2C 0x076) carry no timers, so they never pulse.
- xi-client: `src/ui/status_pulse.h` (`statusPulseCounter`, `statusPulseLevel`, `statusSecondsLeftCeil`, tests in
  `tests/test_status_pulse.cpp`), used by `GameUi::drawBuffs`. The counter is taken from the clock (`40 - ticks mod 40`)
  instead of the game's per-frame decrement with its 1-tick floor, so it keeps the 2/3 s cycle at any frame rate
  (INFERRED equivalence: at 60 fps they are the same). Before this, the client blinked the icon's alpha on and off
  every 15 frames for the last 30 s regardless of cfg 195 (not traced).

### 9.5 Selecting our status icons and cancelling one (2026-10-26, issue #122)

**Select Window from the field** (no menu up; the field input object's handler around 0x10157dc6): the key (group 0x3e
action 0x78; Num + / Compact F) bumps a counter at +0x68, wrapping at 3 (0x10157e49), and selects by its new value:
**1** the log window ("menu logwindo", 0x105eee1c), **2** our status icons ("menu buff", the window object
0x10634698, vtable 0x1033c218), **0** neither (0x1015e760(0)). Cancel (group 0x3e action 0x0F) sets the counter back to
0, deselects (0x1015e740(0)) and plays sound 0x0F (0x10157e9e). So Select Window twice from the field puts the cursor on
the status icons, a third press lets go.

**The icon window's input** (vtable +0x18 = 0x10227610): event 5 (Confirm), when it has icons (+0x40) and the cursor
(+0x30) is on one, runs 0x10227a10; event 6 (Cancel) is swallowed (the field object above deselects); every other
event goes to the list base 0x10221cb0 (cursor movement; its grid rules were not followed: xi-client steps one icon
with Left / Right and a row of nine with Up / Down, INFERRED). 0x10227a10: if the icon's "can cancel" byte (+0x2d0 + i,
filled per frame from the status record's +2, 0x10227b87 via 0x10226c50) is set, 0x100a04c0(status id) sends C2S
0x0F1 at once (protocol-map §7.4). **No Yes/No** is asked; an icon without the flag does nothing.

**While the icons have the cursor** (0x102278c1..0x1022797e, each frame the window is the focused one and its text is
not yet up): the title window (0x1057847c, 0x10150bf0) gets the text built by 0x10226fb0 for the status id (the
status's name, by language; INFERRED from its use, 0x102270f0 not followed) and the help line (0x105781f8, 0x1013a250)
the record's description string (0x10226c80: the string table at the record's +4).

**Status records** (file 87, 0x1800 bytes per status id): the first 0x1FC bytes are decoded by 0x1021e3d0 when loaded
(0x10226ca0): n = popcount(b[2]) - popcount(b[11]) + popcount(b[12]); |n % 5| = 0 rotate left 1, 1 right 1, 2 left 2,
3 right 2, 4 left 3; bytes 2, 11 and 12 (the key) are left as they are. Decoded: +0 u16 status id, +2 u8 can cancel
(the key byte itself, so not rotated), +3 u8 0xFF = no timer text (0x10226c60), +4 a string table (u32 count, u32
offsets from +4; a 0x1C-byte string header, then the text: the description at +0x2C in every record, INFERRED
layout). The icon (+0x280 size, +0x284 image) is outside the decoded part. Examples: Protect (40) and Hundred Fists
(46) can be cancelled, Food (251) and KO (0) not.

xi-client: `ui/status_select.h` (`nextFieldSelect`, `moveBuffCursor`), `dat::statusInfo` / `dat::decodeDataBlock`
(`src/dat/items.cpp`), `GameUi::selectWindow` / `drawBuffSelection` and the 0x0F1 send in `GameUi::draw`; tests in
`tests/test_status_select.cpp`. Not done: the game blanking the cancelled id in its own icon table until the server's
next 0x063 (ours waits for the 0x063); the mouse on the icons.

## 10. Window placement and the HUD's top row (2026-10-19, goal 20)

Checked against retail screenshots from the web (Steam Community captures of app 230330 at 800x600, menu resolution
1:1); ours at `--res 800x600 --menu-res 800x600`.

### 10.1 Menu registry and stacking (CODE)

- **Registry** (table at 0x103722d8, 0x2c bytes a record, ends at an empty name; lookup 0x1015db80, bind 0x1015e860):
  `char name[0x20]` ("menu    targetwi"), `+0x20` the window object's global, `+0x24 u32 flags` -> menu +0x34,
  `+0x28 u8` -> menu +0x30, `+0x29 u8` -> menu +0x60, `+0x2a u16` -> menu +0x38. Flag 0x100000 sets the scale byte
  (menu +0x99). 370 records.
- **Stack groups** (menu manager 0x105eed18 +0x8c, set at 0x1015d862): 0 "menu logwindo" (0x105eee1c; 4 = logwin2
  when the second log window is open, 0x1015f890), 1 the party window (0x10631888), 2 the target window (0x10578478),
  3 0x1057845c (query), 4 logwin2.
- **Placement** (0x1015e4e1 when a menu opens): flag 0x1000 above group 0; 0x4000 above group 2, else group 1; 0x2000
  above group 1; 0x10000000 its own group. 0x1015f6a0 moves the window to `top = base.top - (bottom - top) - 2`
  (or `base.bottom - height` when the base is of kind 1 / 2, 0x10118150 +0x44), at least 16; x unchanged.
- Above the log (0x1000): query, gmtell, playermo (with its layouts battlemo / attackmo), chatctrl, magselec, abiselec,
  myroom, rmpost, mogdoor, mogext, mogpost, roomlist, storage, storage2, jobchang, jobcsel, jobcselu, arealist, shopmain,
  guildsho, merityn, maplist, comyn, resyn, resynus, resynde, resynfr, magic, ability, emote, mount, rmlo1, rmlo2, rmlo4,
  plnt1-6, mp_pmode, mp_abisc. Above the party (0x2000): targetwi, ut_point, conq3pts, imperial, allied, colopoin.
  Above the target or party (0x4000): subwindo.
- The party window's layout: "menu ptw{n}" by members, **"menu ptw0" ("Solo") when not in a party** (retail captures).
- **Overlapping windows (issue #51, 2026-10-22).** The menu manager neither hides nor dims a window that another
  covers: there is no generic "inactive" state (the 0.58 part alpha, part +0x4c, is set by four specific menus only,
  §window alpha), and opening the spell list from "menu magselec" leaves magselec open (0x1013f5b0..0x1013f6f8 only
  call 0x1022ad80). Windows sit in one list ordered by layer (registry +0x29 -> menu +0x60; >= 5 stay last,
  0x1015da20) and by the registry's u16 mask (+0x2a -> menu +0x38): a raised window is moved before the first one it
  shares a mask bit with (0x1015fa57..0x1015fb28; magic 0xc, magselec 0x8, playermo / abiselec 0x4, chatctrl 0x8,
  evitem / conf2win 0x1c, statcom / mgcmenu 0x81). The log never shrinks for a menu: its height is the auto-resize
  line count (0x10162bd0 steps it), it only keeps its lines while a 0x1000 menu is open (0x1015ebd0(0x1000) in
  0x101625f0), and 0x10162e10 shows / hides both logs for the full-screen log only. So what keeps stacked windows
  readable in the game is the skin's opacity: window type 6's background quad is drawn without blending (blend
  `00 00 02 01`, vertex alpha 0x7f), type 1's blends with vertex alpha 0x20 (top) .. 0x60 (bottom). The loader's x1.5
  on quad alpha (0x1011e410) applies to the skin pieces as traced; ffxi-linux had dropped it for every quad after the
  bars' capture (§10.4, a type 6 capture, where the skin's alpha is unused), which left type 1 windows a third more
  see-through than the game's, so text under them showed through. Skins keep the x1.5 again
  (`XI_UI_SKIN_ALPHA_OLD=1` for the old look); other sprites stay at A + 1. `XI_UI_STACK_TRACE=1` logs the menu stack
  (layout, rect, anchor, hidden) whenever it changes.
  **Correction (2026-10-22, the user):** the game's menus and dialogue boxes are always solid, for every window type;
  the blended reading of skin 14's background quad above does not match what the game shows. So the background piece
  (0 / 9) is drawn without blending and at full alpha whenever the window is fully shown (part alpha 1.0); a fading
  window (the open animation, the 0.58 state) still blends. The border pieces keep their additive lines. How the game
  itself makes it solid is not traced (INFERRED from the user's report and the type 6 capture, which is opaque);
  `XI_UI_TRANSLUCENT_WINDOWS=1` brings back the blended background.

### 10.2 Log window (CODE 0x101621f0 and captures)

- Lines are 16 apart (y -= 0x10 per line from the bottom), text at window left + 4. Window width = text width + 14
  (0x10162ca0).
- Retail draws the window frame at all times. Element 1 of "menu logwindo" is the page marker ("anc kaipage"), shown
  only when scrolled back or an event waits.
- The window always follows the newest line (issue #104): its frame (0x101625f0) walks the start line (+0x2c, a log
  ring index) toward the newest every frame, one line per step and at once when 0x41 or more behind; +0x2c is
  otherwise only set by the constructor (0x10161de0) and the reset (0x10162c70), so no input scrolls the main window
  back (the full-screen log does). ffxi-linux keeps its mouse-wheel look-back over the log (not the game's), and any
  new line returns the window to the newest (`logScrollAfter`); before, a wheel notch left the newest rows hidden
  below the window, which hid a `!zone` area line.
- **While the chat line is open** (issue #105). "menu inline" (16,410 366x22, anchor 2) is bottom-aligned with "menu
  logwindo" (16,298 366x134): the authored layouts put the input line over the log's bottom 22. The game keeps the log
  window's height and moves its text out of the way: in the frame (0x101625f0), when the input line is open
  (0x1011b970 on its object 0x10630278, at 0x101628bc / 0x101629f1), the drawn line count +0x30 drops by one and the
  text rect's bottom (+0x46) and the text offset (+0x2e) move up 0x10. With text at window top + 4, 16 a line, the
  last row then ends exactly where the input line starts (`logTextRows`, `logTextBottom`, `logInputTop`). The same
  frame keeps a window of two lines or fewer open while typing (0x10162732: +0x34 <= 2 * +0x54 forces the "keep"
  flag). The second branch of both checks (byte +0x4c) and which log the vtable +0x4c call selects (window 1 only, as
  ffxi-linux does) are not traced (INFERRED). ffxi-linux used to grow the window by 22 instead, and since it also keeps
  a mouse-wheel look-back the game lacks, a looked-back log (the page marker showing) left its newest lines hidden
  while typing; opening the chat line now returns the log to the newest line as well.
- **The chat line's width** (issue #108). "menu inline" is the input object 0x10630278 itself (registry entry at
  0x103723b4, vtable 0x10337a40). Its open handler (slot 1, 0x101b7ab0) reads its own rect (0x1011a640) and resizes it
  (0x1011a4d0) to x, y, log window 1's +0x32 + 0xe, its own height: the same width the log window gives itself (its
  reset 0x10162ca0 resizes to +0x32 + 0xe, +0x32 = the column width from 0x10161a20, capped at +0x52). Both sit at x 16,
  so the chat line's left and right edges match the log's, and a widened log (Config column width, 156) widens the
  line (`logColumnWidth`, `logWindowWidth`). The same handler stores log +0x32 - 0x56 at +0x34 and passes it to the
  line's text draw (0x101b8210); its use there was not followed, so ffxi-linux keeps scrolling long input at the
  window's width - 24. ffxi-linux used the authored 366 for the line, so a widened log left it short. Under the full
  log the line keeps window 1's normal column width (INFERRED: the handler reads window 1's +0x32, which the full log
  was not checked to change).
- **The log objects** (issue #91; `src/ui/log_object.h`). The globals 0x105eee1c / 0x105eee20 hold log window 1's and
  2's objects; the draw-message-window code (0x1012df6d) sets each one's +0x59 byte when a line arrives. The frame
  (0x101625f0) works in pixels: +0x34 (i32) is the shown height, lines x the line height +0x54 (i16, 16). A new line
  adds one line's height (0x10162470, capped at max lines +0x4e x +0x54; its callers were not followed, so "per line"
  is INFERRED); while nothing keeps the log open the frame
  lowers +0x34 with the elapsed time toward min lines (+0x50), never below, and 0x101623c0 takes a line off and rounds
  up. Lines shown (+0x31) = ceil(+0x34 / +0x54), and the window's height eases toward 6 + 16 x lines (0x10162bd0: 8 px a
  frame when more than 16 away, the authored 134 at 8 lines) from its bottom (window +0x40). +0x40 / +0x42 are the text
  rect's left / top (0x10118ea0 then top + 3), so a shrunk-away log has its text top 19 above the bottom.
  Addons use these to hide the game's log: fancychat writes 0 to both +0x34 every frame (the log shrinks away until a
  new line, and it writes again before that shows) and reads window 1's +0x42 to tell when the game's log is opened.
  ffxi-linux publishes its log windows there each frame (`prelude_scan.lua`; native settings `log.window1.*` /
  `log.window2.*`): its own shown height, the line limits, and the text top as the screen height less
  `logTextTopFromBottom(lines)`. An addon's write of +0x34 since the last frame is the height the client draws
  (`LogHold`): kept while the addon keeps writing a value other than the client's own, or while the two agree, and let
  go 250 ms after the writes stop, or at once when the addon unloads. INFERRED: retail answers +0x42 in its menu
  resolution, which fancychat compares with the viewport height; ffxi-linux counts the distance from the bottom at
  one menu unit a pixel, so the comparison holds at any UI scale. The client's own pacing (a whole max-lines window on
  new text, a line off per interval) is unchanged; only what an addon writes is new. Moving or resizing the log through
  its window object (+0x08: +0x3a x, +0x40 bottom) is not wired (no addon in the two sets does it).
- The chat mode label sits on its top edge: "menu windowps" 276 Say, 277 Shout, 278 (reads "Scream"; for yell,
  INFERRED), 279 Tell, 280 Party, 281 Linkshell, 711 Linkshell2, 712 Unity. The sprites are authored for the 366-wide
  window (letters at x 299..); retail keeps the label at the right end: x + width - 366 (INFERRED from captures).
- Window labels and titles are built from per-letter quads of "menu menu2fon" (capitals at v 161, lower case at v 175;
  "Target" = T a r g e t). `xi-dat ui-labels` lists every sprite's letter quads.

**Full-screen log** (issue #92, 2026-10-24; `src/ui/full_log.cpp`, `GameUi::drawChat`). Select Window (Num +) selects
the log window ("menu logwindo", input 0x101619b0); Confirm (event 5) hides both logs (vtable +0x4c on 0x105781d4 /
0x105781d8) and opens "menu fulllog" (object 0x105781e0, ctor 0x10137560, vtable 0x10333b68, open 0x101375d0, input
0x10138160, frame 0x10137f50 drawing the lines through the log window's own 0x101621f0); event 6 closes it.

- **Channel tabs.** The full log shows one tab's lines; open starts on the first (+0x54 = 0 at 0x1013768a). Event 8
  (the Select Window key, 0x101383ea) steps to the next tab (0x10138840, wrapping) and goes back to the newest line.
  The tab list (pairs at 0x1036f950, filled by 0x10138630 / copied by 0x10138730 to +0x68) is filter / label; an
  English client lists them as 0/0 Window1, 8/1 Window2 (only with Config "multiple log windows", 0x10193760(0xb4)),
  3/2 Say, 4/3 Tell, 1/4 Party, 2/5 Linkshell, 7/6 Linkshell2, 10/8 AssistE, 9/7 AssistJ, 6/9 Unity, 5/10 Shout (the
  Japanese order swaps 10 and 9). The filter is the log object's (0x10578158) predicate 0x10129920 on the line's log
  mode: 0 window 1's routing mask (0x10129a70), 8 window 2's (0x10129ab0: cfg 187 / 188), 3 modes 1 / 9, 4 4 / 12,
  1 5 / 13 / 0xd2, 2 6 / 14, 7 0xd5 / 0xd6, 9 0xdb / 0xdc, 10 0xdd / 0xde, 6 0xd3 / 0xd4, 5 2 / 10 / 3 / 11. The labels
  were read from their sprites' letters, which ties modes 0xdb.. to Assist (J), 0xdd.. to Assist (E), 0xd3.. to Unity.
- **The tab row** (0x10137ab0): "menu fep" sprites (sheet 15) along the window's top, x from its left, y = top - 9;
  label i has an unselected and a selected sprite (table 0x10333b4c: 0x12 / 0x1b Window1, 0x13 / 0x1c Window2, 0x0a /
  0x14 Say, 0x0c / 0x16 Tell, 0x0d / 0x17 Party, 0x0e / 0x18 Linkshell, 0x10 / 0x19 Linkshell2, 0x1d / 0x1f AssistJ,
  0x1e / 0x20 AssistE, 0x11 / 0x1a Unity, 0x0b / 0x15 Shout). The shown tab is drawn selected and advances by its
  sprite's width, the others 30.
- **Cursor** (+0x1c, rows from the bottom, 1..rows): Up (event 1) moves it up and scrolls back one line at the top
  row; Down (2) the reverse; Left (3 / 23) pages back, and at the oldest line puts the cursor on the top row; Right
  (4 / 24) pages forward, and at the newest line on the bottom row. The highlighted line gets flag 0x80 (0x10137d7c);
  how that is drawn is not traced (ffxi-linux: a pale bar behind the row, INFERRED). Confirm on a line with a speaker
  opens "menu charlnk" ("/tell %s" / "/target %s", 0x10138438) — not done in ffxi-linux. Cancel closes the full log
  and selects the log window again (0x1013832a).
- **Wrapping** (ffxi-linux): the full log is 352 wide, narrower than a widened log window, so long words wrap there
  first. Until #92 a continuation row whose only spaces were its two-space indent was cut inside the indent and came
  back unchanged, looping forever (the client hung, then was killed): an addon's error line with a long file path did
  it. `wrapLogLine` now takes at least one character past the indent per row.

**Area line on zone-in** (2026-10-23, issue #81; `src/client/zone_banner.cpp`). The S2C 0x00A handler (CODE
0x100fac20) logs the zone you arrive in, switching on LoginState (+0x80; jump table 0x100fb570):

- State 2 (the game), 0 and > 5: the area line (0x100fb590). Login counts too, as it is a zone-in.
- State 1 (Mog House): MapNumber (+0x42, compared before the client's +0x1BC remap) 0x118 Mog Garden or 0x11D
  Feretory -> the area line. Otherwise, when 0x100f9580(floor, MyroomMapNumber +0xAA) holds (floor != 0, or model
  0x121..0x123), 0x100fb620 logs string table 9 (55648) by floor via the table at 0x10331820: #294 "=== Area: Mog
  House 1F ===", #295 "=== Area: Mog House 2F ===" (floor = (MyroomSubMapNumber +0xA8 >> 1) & 3; slots 2 and 3 are not
  string ids). A Mog House without a second floor logs nothing.
- States 3, 4 and 5 (exits to POL / job change): nothing.
- The area line: the zone name is string table 5 (file 0xd8a9 = 55465, loaded by 0x10153aa0) at ZoneNo (+0x30). With a
  name: string table 10 (file 50, XISTRING) #0x5E first when not empty (empty in English; formatted with table 0x14's
  zone name), then #0x62 "=== Area: %s ===". Without a name: the DLL's own "=== Area: %d ===" (DATA 0x10362354) with
  the zone id.
- Colour: log mode 0x29 (0x100d9a50's first argument). Mode table 0x1036e898 gives colour code 0x67 (lookup
  0x10129c00 / 0x10129c30), the same code Config Font Colors "Actions others evade" (cfg 46, default 0x707070) sets
  (0x10129eb0), and the Log Settings bit of that code is 0x2C (0x1012a050).
- Not traced: the handler's guard `arg1+0xC == 2` (taken as always true on a normal zone-in, INFERRED).

### 10.3 Top row: title, help line, network meter (CODE)

- **Title window** ("menu titlewin", 16,16 112x30, object 0x1057847c): the front menu's window title, 0x10118570 ->
  0x10150bf0: the window record's string (Meb str2) looked up in **file 55652** (window titles: 5 Commands, 20
  Equipment, 72 Log Window, 76 Magic List, 81 Status, 129 Chat). Menus without one (menuwind, statuswi) show none.
- **Help line** ("menu helpwind", 130,16): width = 252 + (uiWidth x (1 - f) - 512) (0x1013a180; f from 0x101601a0,
  taken as 0), so it runs to the network window. Its "Help" label is windowps 361 (at x + width - 78, INFERRED).
  Retail text at (+6, +8).
- **Network meter** ("menu netstat", 384,16 right-anchored; object 0x10630fac, vtable 0x10338368; mode +0x14: 0 the
  PlayOnline box 0x10200d30, 1 the meter 0x10200ef0): arrows "menu netmeter" frame base + phase at the origin (base
  by the link percentage: 1 green >= 60, 5 yellow >= 30, 9 red, as 1-based sprite numbers; phase 0..3 steps every
  11 - pct / 10 draws, 0x10200e80: the gradient strip under the arrows runs along them), "%3d" + a percent sign shape at
  (+20, +10), S (sent) at (+58, +4) and R (received) at (+58, +16) with values at +68, colour 0x60808080. S / R are
  bytes a second: every 3000 ms the byte counts x 1000 / 3000 (0x100fca10, 0x100fc950 / 0x100fc970). The percentage
  (+8) drops while nothing arrives for 10 s (0x100fc990). Retail shows the meter at all times and its window only with
  a menu up (captures).

### 10.4 Gauge window ("menu gaugewin", 16,48 128x44; object 0x105781ec, draw 0x101392d0)

- Fills "menu windowps" 97 (the party HP fill) at (+25, +8) HP x HPP / 100 colour 0x80808080, (+25, +18) MP x MPP / 100
  0x8080e080, (+25, +28) TP: < 1000 x TP / 1000 in 0x800060b0; 1000..2999 full, then (TP % 1001) / 1000 over it in
  0x8000b060 (< 2000) or 0x802060d0; 3000 full. Numbers "%4d" at +123, rows +7 / +17 / +27, 0xc0808080. Labels and
  tracks: sheet table entry 4 ("menu framesus") shape 8 at (+4, +8): "HP" / "MP" / "TP" font quads (colours
  0x6040407f / 0x6060407f / 0x4040607f) and three "menu gauge" 64x8 tracks with end caps at x 18..90.
- Bar brightness (2026-10-19, lossless retail capture of the Solo window, window type 6): the fills and the track
  under them came out too strong. The quad alpha loader (0x1011e410) scales A by 1.5 (constants 1.5 at 0x1032a3ac, 255
  at 0x10329a20), but the capture matches only without that factor: HP centre ours (255,172,194) with it, (228,130,152)
  without, retail (226,116,118) over the same (0,14,30) background. The UI now uses A + 1. Where the client undoes the
  1.5 was not traced (INFERRED from the capture). Blue still reads about 30 high.
- Sheet table 0x10630330 (0x101c8560): 0 font fontshp, 1 menu windowps, 2 menu keytops3, 3 menu pname, ...
- Retail shows it with the spell list (captures); what opens it was not traced.

### 10.5 Sub window and the spell list (CODE)

- **Sub window** ("menu subwindo", 384,208 112x42 bottom-right anchored; registry flag 0x4000: above the target window,
  else the party window; object 0x10578474, vtable 0x10334eb8, draw 0x1014f890 by mode +0x28 0..10). With the spell list
  (mode 1: 0x1014f9b0 + 0x1014fb50): "MP Cost" (file 55648 #230, string table 9 via 0x10152e20) at (+5, +8) with the
  value right-aligned at +0x69, "Next" (#231) at (+5, +0x1a) with the recast (m:ss) right-aligned at +0x69. Retail
  draws both upright (captures).
- **Gauge / cast placement**: "menu casttime" (16,94 128x24) is the casting bar under the gauge window; 0x1012c720 puts a
  window at x 16 below the casting bar, else below the gauge window (y 94), else at y 47.
- **Spell list rows** (list class 0x10221320, magic list vtable 0x1033c510, row draw 0x1022c9c0): the name at row x +
  0x12; the icon at the row origin from 0x102208a0(iconA, iconB): sheet table 0x10634368 entry 5 "menu magiconw" shape
  iconA when config option 2 is set (0x101937a0(2)) or iconB is 0xFFFF; else iconB: entry 6 "menu magico2w" shape
  iconB - 1000 from 1000, else "menu magiconw" shape iconB. iconA / iconB are u16 +0x40 / +0x42 of the spell's 0x64-byte
  record in table 0x10634f54: file **81** (ROM/118/114) chunk "mgc_" (type 0x49, 0x19000 bytes, 1024 records, each
  rotated as file 86's, decoder 0x1021e3d0; loaded by 0x101670e0). iconA equals file 86's record +0x28 (element icons:
  Cure 6, Fire 8, Aero 10, Stone 11; shapes 0..7 white, 8..15 black, 16..23 summon, 24..31 ninjutsu, 32.. songs);
  iconB is the spell's own (Cure 86, Stone 210, Poison 223, Blind 233, Fire 240, Aero 280). "menu magiconw" lives in file
  **39551** (ROM/280/15, per-spell textures "white cure", "black aero"..). Retail captures show the per-spell icons. Row colours (table 0x103860fc): 0x80808080 normal, 0x80404040 not
  usable, 0x80803040 (pink) the selected row. Retail's list shows no MP cost (it is in the sub window).

### 10.6 Status menu (CODE 0x10146270 .. 0x10148c30)

- Sheet table entries (0x101c87e0): 1 menu windowps, 2 menu keytops3, 4 menu framesus (language dependent). Sheet 4 shape
  8 is the gauge window's HP / MP / TP labels and frames (10.4).
- Objects (pointers 0x105783e0..0x105783fc): statcom (vtable 0x103348e0, input 0x10148c30), statuswi (ctor 0x10146270,
  draw 0x10147000), level (draw 0x101475e0), joblevel (0x10147f60), profile (init 0x10147900, draw 0x101479f0),
  btlskill (0x101483e0), mgcskill (0x101485e0), trdskill (init 0x10148730, draw 0x101487d0).
- "menu statcom" (384,48 right-anchored, elements 1..6, help 480..484 / 720) replaces the main menu; "menu statuswi"
  (16,16) and "menu level" (16,208) show beside it. No title window. Confirm on element i (i <= 5) closes the shown
  window (0x101489f0) and opens table 0x103716c8[i - 1]: profile, joblevel, btlskill, mgcskill, trdskill (all 130,48);
  the cursor stays in statcom and confirming the shown one again does nothing. Element 6 (label keytops3 506,
  "Currencies") calls 0x1022a180(1) on the object at 0x106346b4 ("menu evitem", §10.10), which exists once the
  quest / mission file 82 has loaded (0x10222020 -> 0x10228f90). Cancel
  closes the shown window first (0x10148c4b), then statcom.
- Text: fontshp throughout (0x101c9330 left, returns the end x; 0x101c9700 with argument 5 = 1 right-aligned;
  0x101c9900 width-fitted). Colours 0x80RRGGBB.
- statuswi: name (4,7), width w - 6. Nothing else until 0x10485824 bit 0 (CLISTATUS seen). "Lv%d" (55653 #89) at
  (4,0x13), the job name sprite windowps 212 + job at end x + 2 (0x802060a0 while level-synced); the support job the
  same at (4,0x1f) when set. "ILv" (4,0x2b); "%3d/" of 99 + ilvl (0x061 +0x55) or "---/" right at 0x46; "Su%2d" of
  su_lv (+0x52) at 0x4a. HP "%4d/" right at (0x46,0x3a), max "%d" at 0x48; MP the same at y 0x46; TP "%4d" right at
  (0x4b,0x52). STR..CHR base "%4d" at (0x32, 0x60 + 13 i), bonus after it: "+%d" 0x80408040, "%d" 0x80804040.
  A stat-down flag (0x100fd280 bit 0x17 HP, 0x18 MP, 0x10 + i stats) tints the row 0x80804040 and draws a marker
  sprite: not tracked here.
- level: "Level %d" (55653 #86) (8,10); "EXP" (#87) (15,22); EXP right at (90,34); "NEXTLevel" (#88) (15,46); needed
  right at (90,58); numbers with "," separators (0x101c8ff0); gauge 0x101cabf0 (windowps fill 97, frame 38) at
  (22,72), exp / next.
- profile: help line "Title: " (55651 #78) + the title (55704 by title id; 0x10157050) set once. Nation crest +
  name keytops3 74 + nation (nation < 3) at (0x37,0x12); home point area (55661[BindZoneNo +0x4a]) at (0x5e,0x34);
  rank "%d" right at (0x40,0x44); rank bar 0x101caa70 (framesus fill 103, frame 98) at (0x5c,0x54), rankbar (+0x48)
  x 1/4096 (0x10330250). Not drawn here: a region name at (0x32,0x24) (0x10107250(1, ...)), and the mastery /
  Monstrosity lines from 0x10087a50 on (0x10482f9c, 0x10482fa4, 0x104dfd98 +0x40dec / +0x40deb).
- joblevel: jobs 1..22 (23 skipped) whose bit is set in the unlocked mask, two to a row from y 0x13 (+14 per pair):
  name windowps 212 + job at 0x0a / 0x82, level "%d" right at 0x6e / 0xe6. Name colour main 0x80e09620, support
  0x804070a0, else white.
- btlskill: skills 1..31 (S2C 0x062 skill_base[i] @+0x80, copy at 0x104856a4), keytops3 208 + id at 0x0a / 0x82,
  value & 0x7fff right at 0x78 / 0xf0 (0x6e / 0xe6 for language 0), 0x802060a0 when bit 15 (capped). Bit 15 with
  level 0 is skipped (LSB sends 0x8000 for automaton skills 22..24). Two to a row as joblevel.
- mgcskill: skills 32..47, keytops3 240..255, values right at 0x6e / 0xe6, same rules.
- trdskill: skills 48..62, 0xFFFF skipped, one to a row from y 0x13: name keytops3 208 + id at 0x0a; rank
  (value & 0x1f, < 16) keytops3 193 + rank at 0x6e (0x5f language 0), 0x802060a0 when capped; "(%d)" of
  (value >> 5) & 0x3ff right at 0xf5. The window is resized (0x1011a4d0) to its rows; the height used here (0x16 +
  14 per row) is INFERRED, the argument reads as 14 x (rows / 2) + 0x16 and was not resolved.
- Retail capture (Status, Lv19 WAR) has no ILv / Su row; this client draws it as the code above does.

### 10.7 Equipment and Items screens (CODE and captures)

- Equipment (CODE 0x101330ad) opens "menu persona" (the statuswi object 0x105783e4, its +0x1c set to 2), "menu money"
  and "menu equip". Retail captures (1280x800, menu scale 1) show the status window at 16,48 under the title window
  (32 below its Status-menu place), the grid at 130,48, the hovered slot's item list ("menu inventor") 2 right of the
  grid at 314,48 while the cursor is still on the grid, "Current Gil" at 16,240 and the item info window at 130,240.
  The list rows show icon and name only (the level is in the info window). The list placement and the 32 offset are
  INFERRED from captures; the code that moves them was not traced.
- "menu money" (16,240 112x56, object 0x10578248, draw 0x101407f0): sprite framesus 11 holds "Current Gil" and the
  coin; the gil (inventory slot 0 count, 0x100eb3c0(0, 0) +4) right-aligned at (+0x67, +0x1f) via 0x101c8ff0, with
  thousands separators and " G" as in captures. Also opened by the item list (0x10139850), trade (0x10139cf0), delivery
  (0x1012ef52), post (0x1014cdb0) and gift (0x10151c3b) screens; closed by 0x10131d70 (persona and money).
- Items: "menu inventor" (16,48) with "menu money" under it, so the item info window goes to 130,240 as on the
  Equipment screen (INFERRED). The window record's strings: help 3 "Select an item." (55651), title 2 "Items" (55652),
  shown when the row has no help of its own.
- Items count (#107): the title window reads "Items" then held/capacity ("28/30") in plain white, the capacity from
  S2C 0x01C (ItemNum - 1; slot 0 is the gil), gil not counted. The game titles its counted lists with one string,
  "%s          %d/%d" (0x103869e4: title, ten spaces, count; the quest list 0x102334d0 -> title 0x101170e0, and
  0x10231b00 drawing it 0x80808080 at the title window + 0x3c, + 10). That the inventory uses the same layout, and that
  it drops the list's grey "position/total" in its place, is INFERRED from retail captures: its own draw was not found
  (none of the "%d/%d" users near the inventory object 0x1063430c draws it). ffxi-linux: ui/item_count.h.
- "menu statpup" (16,48 112x190; object 0x10631910, vtable 0x1033b200, draw 0x10207cd0) is the automaton's status
  window, not the Equipment screen's.

### 10.8 Name labels over characters (CODE 0x100d0440 -> 0x10086140)

- Not a Windows font: the GDI / D3DX font code in the DLL (CreateFontIndirectA 0x102e72fe, D3DXCreateFont 0x102daa70,
  DrawText 0x102f48c7) is statically linked D3DX with no game callers. "font high" (0x1011c500) is a texture the moji
  path builds at run time, not used here.
- Actor vtable (e.g. 0x1032d710): +0x284 = 0x100d0440, the per-frame name pass. It skips hidden / cutscene cases, gets
  the anchor, scale and colour from vfunc +0x278 (0x100d03f0: the attach point + actor +0x8c0 in y, then 0x10082ff0),
  the name from vfunc +0xcc ("%s", 0x1034f7e0) and draws it with 0x100d07b0 -> 0x10086140 (skipped when actor +0xb2 is
  set); a second line (vfunc +0xd0) goes through 0x100d07e0. Actor +0x88 bit 0x400: scale forced to 1.
- 0x10082ff0: the anchor projected (0x10082ee0) with x, y, z x 4096 (0x1032d3d8) as integers. z > 4100 (0x103581e8):
  hidden; z < 4020 (0x103581e4): scale 1; else scale (4100 - z) / 80. With the game's projection (near 0.1, far 65535,
  z = 1 - 0.1 / depth) that is min(1, 0.05 + 5.12 / view depth): 1 within ~5.4, 0.55 at 10, 0.3 at 20; never hidden.
  Colour = actor +0x78 (alpha byte cleared under a flag via vfunc +0x300); actor +0x88 bit 4 pulses the alpha
  (sin of a 16x-rate angle, x 32 + 96).
- 0x10086140(anchor, text, scale, colour, colour2): at most 0x24 bytes; bytes 0xC8..0xCD expand through the tables at
  0x1032d36c / 0x1032d374 (icon codes); 0xAC adds 0xAD. Glyphs are "font fontshp" (sheet object 0x104e1bf8; 0x10120440
  -> 0x1011f9b0): each w x h = quad x1 - x0 - 1 by y2 - y0 - 1, advance w; codes 0x8E..0xA8 at 0.8 size, 0xAA at 0.5.
  The line is centred (x from -total / 2); '\n' moves down 8. Screen size: x units x screen width x 0.00234375
  (0x1032ade0) x scale, y units x screen height x 0.00234375 x scale, so on a 4:3 screen glyphs are 4/3 wider than
  tall (the wide look). Quads at the projected point (0x10026f00), drawn by 0x1000cd00 (render states 0x18 / 0x1b set
  around it); texture by code range (0x101204c0: the second fontshp texture swaps in colour2).
- The outline and italics are in the fontshp texture (white glyphs with a dark edge, drawn at twice the quad's
  resolution), so no extra outline pass.
- Here (apps/xi-client/actors_draw.cpp): the same glyphs, sizes and depth scale; the anchor is the cap line's foot
  (INFERRED: where 0x10086140 puts the quads relative to the anchor in y was not resolved). Colours: the actor's +0x78
  writer was not traced; every player white, party members and our own name too (a lossless retail capture, 2026-10-19);
  NPCs / mobs / claimed mobs as before.
- **Icons before the name** (vfunc +0xcc = 0x10082e50 -> 0x10097770): at most one icon, then a space, then the name. The
  first byte is 0x92 (the linkshell pearl) when the entity is a PC with +0x124 bit 27; 0x10097430(0, that byte) then
  returns the first icon of this chain, else the byte unchanged: POL 0x8E, link dead 0x8F, Monstrosity name 0xAB,
  Ballista / Brenner teams (0x9E 0x9F 0xA0 0xA3 0xA4, 0xA5 with +0x29c, 0xA6 / 0xA7 for teams 0x20..0x27), away 0x90
  (Monstrosity ranks 0xAE..0xB0 for non-PCs), GM level 1..7 (table 0x1035b62c: 0x93 0x93 0x95 0x96 0x97 0x98 0x99)
  unless GmIconFlag, PvP 0x94, recruiting 0xAC (+ 0xAD), auto party 0x9D, seeking a party 0x91, +0x130 bit 23 0xA8,
  trial 0x93, mentor 0xA2 (0xB1 for non-PCs), new adventurer 0xA1, bazaar 0x9C. 0x10097660 / 0x100976d0 add icons
  only in Ballista / PvP areas. Gate 0x10097720: no icons on an invisible player (0x10097320), among bits not traced.
- **Flags** (the 0x00D handler copies bit by bit, CODE 0x10099a10 .. 0x1009a920; LSB names from char_update.cpp):
  Flags1 (+0x20) bit 11 LfgFlag -> +0x124 bit 20, 12 Anon -> bit 23, 14 Away -> bit 22, 16 POL -> bit 26, 17 LinkShell
  -> bit 27, 18 LinkDead -> bit 28, 24..26 GmLevel -> +0x128 bits 11..13, 29 Invis -> +0x128 bit 6, 31 Bazaar ->
  +0x128 bit 9; Flags2 (+0x24) bit 28 GmIconFlag -> +0x12c bit 18, 31 AutoPartyFlag -> +0x124 bit 21; Flags3 (+0x28)
  bit 1 LfgMasterFlag -> +0x140 bit 3, +0x29 BallistaTeam -> +0x1ff, bit 23 NewCharacterFlag -> +0x130 bit 13, bit 24
  MentorFlag -> +0x130 bit 12; +0x32 BallistaInfo -> +0x294; Flags4 (+0x33) bit 1 TrialFlag -> +0x130 bit 14, bit 6
  JobMasterFlag -> +0x13c bit 7. Our own (0x037, LSB char_status.cpp): Flags0 @0x28 (Lfg 4, Away 7, POL 24, LinkShell
  25, LinkDead 26, GmLevel 29..31), Flags1 @0x2C (Invis 15, Bazaar 29, GmIcon 31), Flags2 @0x34 (AutoParty 2), Flags3
  @0x38 (LfgMaster 0, Trial 1, NewCharacter 3, Mentor 4, BallistaTeam 8..15), Flags4 @0x58 (JobMaster 7).
- **Icon glyphs**: "font fontshp" shapes code - 0x20 (0x8E..0xB1, texture "menu ustatshd", 16 x 16 at y -2), drawn in
  colour2 (0x101204c0 swaps it in for that texture): the linkshell colour when the first byte is 0x92 (actor +0x7c =
  entity +0x1d4, 0x00D +0x24..0x26 / 0x037 +0x31..0x33 packed with the first byte lowest, i.e. blue: CODE 0x1009a42e /
  0x1009d353), else 0x80808080. The packed colour has no alpha: the name's is used (INFERRED). 0x10086140: an icon after
  the first glyph draws at 0.8 size; 0xAD (after 0xAC) half size at (-2 - w, -2) over the previous icon, no advance.
  The stars and recruit glyphs (blend [0 0 1 1], blending off) have black alpha-0 surrounds: the UI shader discards
  alpha 0 (alpha test, INFERRED as the game's state).
- **Second line** (vfunc +0xd0 = 0x10082e90 -> 0x100978e0 -> 0x100d07e0 -> 0x10086830): the byte 0xD2 for a PC with
  JobMasterFlag; 0x10086830 expands it to D3 D4 D5, each glyph 0xAD: half size (bottom-aligned: y + h / 2), full size,
  half size, centred, 16 units above the name's anchor (0x1032d448 = -16). The retail capture's stars look a little
  wider apart and higher than this reading gives (not resolved).
- **Target pulse**: the target code sets actor +0x88 bit 2 on the new target and clears it on the old (CODE 0x10157b36;
  0x101584cc on clearing); 0x100830b7 then scales the name's alpha by (96 + 32 sin(16 x tick degrees)) / 128 (0.5..1.0),
  tick = the counter at 0x104568fc +0x34 (60 a second INFERRED).

### 10.9 Map window tag ("menu mapframe") and the event timers (CODE)

- **"menu mapframe"** (106x14, no elements in the DATA) is the map object's own window (map +0x8). 0x101ef810(sheet)
  gives it an animated sprite element (type 2; 0x1011aa20 -> 0x10119e30: the sheet, from shape 0, looping, a shape
  every 3 ticks, 0x10119e50) and sets map +0xA3 (0 clears both); the draw (0x101eeab9) then moves that sprite to the
  map cursor (+0x18 + +0x1C, +0x1A + +0x1E; the cursor the Map screen moves, also copied to [0x104e1d4c] +0x38 / +0x3C)
  unless +0xA6 is set. Callers: Wide Scan (map mode +0x16 = 7, 0x101fb776: sheet 9 "anc anc_scan"), the campaign
  map ("menu cnqframe", 0x101e7931: sheet 10 "anc anc_regi"); the others clear it. Events never set it, so the event
  map has no tag. xi-client: no Wide Scan or campaign map yet, so nothing draws it.
- **Cast-time / event-time windows** ("menu casttime" / "menu eventtim": 16,94 128x24, framesus 58 / 138 with the
  framesus 59 track at 22,6): open sets total +0x14 = left +0x18 (0x1012c100 / 0x1012c430). Each frame
  (0x1012c020 / 0x1012c340): moved to x 16, y 94 when the gauge window is up (0x1011b970 on 0x105781ec), else 47; a
  window whose left is already <= 0 closes (0x1015e6a0); else left -= the frame time in ticks (0x10014cb0: the frame
  time at [0x104568fc]+0x28, at least 1) and is clamped at 0; +0x1C = (total - left) / total (100 when total is 0).
  Mode 4 (events) draws only the time, left / 60 truncated (constant 1/60 at 0x1032b160), "%d:%02d" right-aligned at
  x + 0x69, y + 9; modes 0..3 draw the gauge (0x101cabf0 with +0x1C at x + 8) and a "%d" percent (+0x1C x 100) at
  x + 0x6A, y + 7. Seen live (local test hook, Spot the Difference in Southern San d'Oria): 1:00 counting down, gone at 0.
- **Event choice menu ("menu query", 16,208 bottom-anchored, 366x88; windowps 11 behind; elements 1..3 at 28,28 /
  28,48 / 28,68, 132x16)**: op 0x24's window (0x1015e9b0; handler 0x1057845c, vtable 0x10335280, 0x54C bytes). Open
  (0x1014dd40): rect +0x540 = the window's own, cursor sound off (+0x85 = 0). Prompt (0x1014de80): clears the options,
  the text at +0x38 (0x80 bytes), width +0x34 = its pixel width. Option (0x1014dec0): a 0x108-byte row (text +4, value
  +0x104), +0x34 = max(+0x34, width + 0x4C). Cursor (0x1014e020): the row, scroll +0x32 so three rows show. Draw
  (0x1014e150): the prompt at x + 4, y + 7, then rows scroll.. scroll + 2 at their elements' x / y (no counter).
  0x1014e230 widens it to max(min(log window width, 512), +0x34 + 8) only on German / French clients ([0x10456a28]+8
  = 2 / 3). Registry flag 0x1000 stacks it above the log (§10.1), so it never meets the timers at the top left.

### 10.10 Currencies (CODE 0x10228f90 .. 0x1022a230; issue #89)

- Object 0x106346b4 (0x94 bytes, ctor 0x102291d0, vtable 0x1033c418) drives "menu evitem" (16,48 366x248, 8 rows at
  x 40, y 13 + 29 i) for three modes in +0x88: 0 key items, 1 Currencies, 2 Currencies 2. Created by 0x10228f90 from
  0x10222020 when file 82 (`Dat_LangFileId(0x59)`, the quest / mission tables) loads; freed by 0x10229030.
- statcom (§10.6) element index 5 (Currencies) calls 0x1022a180(1) and index 9 calls it with 2 (Currencies 2; this
  install's statcom has only 6 elements, so mode 2 is unreachable). 0x1022a1c0 pushes "menu evitem" by name
  (0x1015e260); modes 1 and 2 set the title window to 55651 #721 "Currencies" (0x10152e20(0xe, 0x2d1) ->
  0x10150bf0).
- Mode 1 opening (0x10229d80): clear the list, send **C2S 0x10F** (header only, 4 bytes; 0x100fd450) with the reply
  callback 0x10229a60, +0x8a = 0 (not ready), +0x8b = 30 (INFERRED: a re-request delay; not done here). Mode 2
  (0x10229dc0) sends **C2S 0x115** with 0x10229bf0 instead.
- **S2C 0x113** (0x10229a60): 99 rows (0x63), one per entry of table 0x103862c8 `{u32 key, u16 label, u16 help}`,
  in table order; each row's value is the u32 0x10229320(index, row, packet) picks by key (offsets include the
  4-byte header): keys 0..2 u32 at 4 + 4k (conquest points), 3..7 u16 at 10 + 2k, 8 / 9 / 10 u16 at 0x1a / 0x1c /
  0x1e, 0x0b..0x13 u32 at 4k - 0xc (guild points), 0x14 u32 0x44, 0x15..0x1c u8 at k + 0x33, 0x1d / 0x1e u32 0x50 /
  0x54, 0x1f..0x21 u16 at 0x1a + 2k, 0x22 u32 0x60, 0x23..0x25 u8 at k + 0x41, 0x26 u32 0x68, 0x27 / 0x28 u16 at
  0x1e + 2k, 0x29..0x2c u32 0x70..0x7c, 0x2d..0x31 u32 at 4k - 0x34, 0x32..0x36 u32 0x94..0xa4, 0x37..0x39 u32
  0xac..0xb4, 0x3a..0x3e u8 at k + 0x7e, 0x3f u8 0xbd, 0x40 u8 0xcc, 0x41 / 0x42 u8 0xbe / 0xbf, 0x43..0x45 u32
  0xc0..0xc8, 0x46 u16 0x5e, 0x47 u16 0xa8, 0x48..0x4c the five 9-bit fields from 0xd8, 0x4d u32 0xe4, 0x4e..0x57
  u8 at k + 0x80, 0x58 u16 0xe0, 0x59 u16 0xaa, 0x5a..0x62 u16 0xe8..0xf8. The layout matches LSB's
  GP_SERV_COMMAND_CURRENCIES_1. Every row is listed, zero or not. +0x8a = 1.
- Row draw (0x10229ae0): the label is string table 0xc #label (file 55653: #136 "Conquest Points (San d'Oria)" ..
  #484 "Deeds"); the value `"%d"` (0x10353000, no separators) right-aligned at the row rect's x + 0x136 (the rect is
  INFERRED to be the list's; drawn here at the window's right edge - 8 like the other list counts). The help line is
  file 55651 #help (#722..). Mode 2 (S2C 0x118, 97 rows from table 0x103865e0, 0x10229800 / 0x10229c70) is not done.
- Before the reply arrives the list is empty (no "please wait" text found).


### 10.11 Guild shops (CODE 0x100ead20 .. 0x100eb1a0, 0x101ec210 .. 0x101ed58a, 0x1020f0e0; issue #94)

- **Packets.** 0x100ead20 registers the lists: S2C 0x083 (buy list) 0x100eae10, 0x085 (sell list) 0x100eaf70,
  0x082 / 0x084 (results) 0x100eaef0 / 0x100eaf90, 0x086 0x100eadb0; 0x1020f0e0 registers the menu's own handlers
  (0x086 0x101ed3c0, 0x082 0x101ec210, 0x084 0x101ec300) next to the NPC shop's 0x03c..0x040.
- **Lists** (game object 0x104dfd98 +0x19de4 + n * 0x408, n 0 = buy, 1 = sell; 0x100eade0): `u8 state` (0 none, 3
  arriving, 1 complete), `u8 count`, rows of 8 bytes at +4 (GP_GUILD_ITEM `{u16 ItemNo, u8 Count, u8 Max, i32
  Price}`), `u32 time` of the last request at +0x404. A 0x083 / 0x085 packet (`List[30]` @4, `Count` @0xf4, `Stat`
  @0xf5; 0x100eae30): Stat bit 0x40 empties the list (state 3), each row is added unless its item is listed already
  (0x100eaea0), bit 0x80 completes it (state 1). Every 0x086 zeroes both counts. 0x082 / 0x084 (`ItemNo` @4, `Count`
  @6, `Trade` @7) set the listed row's stock to Count.
- **0x086** (`Stat` @4, `Time` @8; 0x101ed3c0):
  - Stat 0 (open): with a window of registry flag 0x20000 up, or the player busy (0x1057876c +0x59 / +0xc4), logs
    55646 #69 "You must close the currently open window to use that command." Otherwise it stores our position
    (actor 0x104dfd98 +0x40da8's x / z) and opens **"menu guildsho"** (Buy / Sell, labels 149 / 150 like "menu
    shopmain").
  - Stat 1 (closed) / 2 (holiday): ignored when no guild window is open and Time bit 31 is set (LSB's hour tick,
    "passive"). Closed: string table 10 #25 "The shop is closed.\nThis shop's hours are %d to %d." with the first set
    bit of Time's low 24 bits and the first clear bit after it; holiday: #26 "...%s is this shop's weekly holiday."
    with the weekday (file 55658 #0..7; nothing when Time > 7). Then the guild window closes (vfunc +0x18(6, 0)).
- **Walking away** (0x101ed280): the guild window closes once our x / z is more than 6 from where it opened (squared
  distance against 36.0 at 0x103344b4).
- **Buy / Sell** (OnEvent 0x101ed2e0): a list whose count is 0 is requested (0x100eafe0: C2S 0x0ab / 0x0ad, 4 bytes,
  unless the last request was less than 5 ticks ago, which plays the error sound instead), then the shop list window
  (0x106319e8, "menu shop") opens with the guild's rows (title 55652 #13 "Guild Shop"):
  - Buy rows (provider vtable 0x10339f70, 0x101ec4d0): item name, a number, the price; dimmed (0x80404040) when the
    price is above our gil or the stock is 0. Help 55651 #47 "Purchase merchandise." Confirm (0x101ec750) opens the
    number window (0x10578200 -> 0x1013a7a0) up to min(stack, gil / price, stock), then a yes / no window
    (0x106319f0), then C2S 0x0aa `{u16 ItemNo, u8 0, u8 ItemNum}` (0x100eb110).
  - Sell rows (0x10339f44, 0x101ec8c0): first 55648 #44 "Select from your inventory.", then the rows whose price has
    bit 31 clear: name, a number, the price, dimmed when we hold none or the stock is at its maximum. Help 55651 #51.
    A row (or the first one, for every item) opens the inventory list (0x10339f18, 0x101ecd00; help 55651 #48 "Sell
    merchandise."); confirm there opens the number window up to the stack's count, the yes / no window (0x106319f4),
    then C2S 0x0ac `{u16 ItemNo, u8 PropertyItemIndex, u8 ItemNum}`.
- **Results** (0x101ec210 buy / 0x101ec300 sell; table 7 = file 55646, item lines file 7031 with Para0 = item, Para1 =
  count):
  - ItemNo 0: buying, Trade -5 -> #163 "Transaction cancelled. You can only hold one item of that type.", -3 -> #154
    "Please wait longer before making another purchase.", else #155 "You were unable to carry out that transaction."
    (selling: always #155).
  - Trade 0: #129 "You cannot make that purchase." / #130 "The guild would not buy that from you."
  - Otherwise 7031 #165 / #166 "You buy ... from the guild." (#167 / #168 "You sell ... to the guild.") for |Trade|
    (> 1 / 1), then, when Trade < 0 (partial), #131 "Your order could not be filled completely." / #132 "The guild
    could not purchase the full amount."
- xi-client (client/guild_shop.h, GameUi Kind::GuildMain ..): the lists, results, closed / holiday lines and the
  walk-away close as above. INFERRED / not done: the number window and the yes / no window are this client's "menu
  shopbuy" / "menu shopsell" prompts with Left / Right for the quantity (help line "x N (total gil)"); the buy rows'
  second column (read from the item record +0xcc8 by the game) is shown as the guild's stock; the sell inventory list
  shows unequipped stacks of items in the sell list whose stock is not full (the game's filter 0x101ecc40 is not
  traced); the #69 refusal and the 5-tick request throttle are not done (any open window gives way to the guild menu,
  as with NPC shops). The 7031 codes `01 05 04` (the count, Para1) and `01 09 2a` (the item's log name, plural past
  one) are read that way from the texts (INFERRED; dat::BattleText::dialogName tables 0x04 / 0x12a), which also fixes
  "You obtained ..." (#169) and the synthesis lines (#160..#163). Verified live (BotD, Babubu): bought 3 Little
  Worms (stock 180 -> 177, "You buy 3 little worms from the guild."), sold 2 back ("You sell 2 little worms to the
  guild.").

### 10.12 Equipment and inventory rules (goal #45, 2026-10-26; `src/ui/equip_rules.h`)

- **Opening Equipment** (CODE 0x10133060): refused while synthesizing (status 44, 55646 #78 "You cannot use that command
  during synthesis."), fishing (#77) or before a load flag (0x104dfd98 +0x198cc bit 0, #168; not tracked here). There
  is no lock in combat or while casting: the client changes gear then, and LSB refuses only in an event or an abnormal
  status (0x050's validator) or for a slot encumbrance holds.
- **The slot's list** (row colour 0x10130470, rows 0x10130b30): every item fitting the slot in the inventory and the Mog
  Wardrobes (each row carries a bag icon: 0 inventory, 1..8 wardrobe 1..8; the icon's sheet was not traced, not drawn
  here). Colours: worn 0x80509050 (green), wearable 0x80808080, not wearable or in a wardrobe the account lacks
  0x80404040. Wearable (0x10131f80): race bit (own race from the look), main job bit, level <= main level, superior
  level <= su_lv, and no slot of the item blocked by encumbrance (S2C 0x01B +0x60, low 16 bits = LSB m_EquipBlock).
- **Equip / remove** (0x10132700, also used by /equip): choosing the worn item again removes it (0x10132b04; there is no
  "(Remove)" row in the game). A weapon that can go in the main hand put in the sub slot: no NIN / THF / DNC / BLU main
  or support job -> 55646 #139, no main weapon -> #152, a two-handed main (skills 1, 4, 6, 7, 8, 10, 12: 0x1015a9a0)
  -> #153. Shields and grips are not checked by the client (LSB swaps what it must). The same item already in the slot
  sends nothing. Then C2S 0x050 (0x100e5270).
- **/equip** (CODE 0x1007b2c0): `/equip SLOT` alone removes (55648 #68 "Equipment removed."), with an item equips
  (#67 "Equipment changed."); slot names from string tables 0x17 / 0x18 (0x101325a0, map at 0x1036f7b8). "Remove All"
  exists only for mannequins (55648 #227 at 0x10142f0b is the Monstrosity list's); none in the Equipment window.
- **Items list colours** (0x101f0290): worn gear and the worn linkshells green; enchanted gear worn and ready
  0x80407090 (not done here); items the menu can use yellow 0x80909040: a non-equipment item with flag 0x0400, crystals
  (type 8), types 0x16 / 0x17; the rest white.
- **Items screen**: the list opens with "menu itmsortw" at 384,48 (CODE 0x101ae3f0 sets mode 0x10630c34, whose enter
  handler 0x101f01b0 opens "menu itmsortw", or "menu itmsort2" with the Recycle Bin row when +0x90 is set): Auto-sort
  (help 269), Manual Sort (270; help 5 / 6: the same stack again splits one off), Switch (1067: the containers,
  jump table 0x101f0039.. switching the list's container, 0x1021c320). The focus moves with Select Window (Num +,
  Compact F; #117): the list's handler 0x101f0530 sends event 8 to the sort window (0x1015da90 on 0x10630c28, or
  0x10630c2c for "itmsort2"), event 5 opens "menu iuse", 6 closes the list; the sort window's handler 0x101f3600
  sends events 6 (cancel) and 8 back to the list (0x101f32f0). No arrow key moves between them. Select Window during
  Manual Sort ending it is INFERRED. Here Switch opens the container's own list (Storage: take out,
  "Put away..."), as the game's container view is not traced; LSB's 0x029 rules: wardrobes take only gear, nothing goes
  into the temporary items, the Mog Safes / Storage only in the Mog House (INFERRED from LSB's validContainers).
  Manual Sort here: same stack -> 0x029 ItemNum 1 to a free slot (split); a part stack of the same item -> 0x029 into
  it (LSB "unite"); otherwise the two swap places in the list (the display order is the client's).
- **Auto-sort asks first** (#118; `src/ui/yes_no.h`): the sort window's Confirm on element 1 (0x101f37a4) opens the
  yes / no dialog object (0x102222d0 on [0x10634384]) with "menu sortyn" (0x1038610c: 384,124 112x42, element 1 Yes,
  2 No, 3 the title tab), the question 55653 #61 "Sort items?" (string table 0xc via 0x10152e20) in the help line
  (0x1013a250), the title 55652 #2 "Items" (table 0xd) in the title window (0x10150bf0(title, 2)), and startOnYes 0, so
  the cursor opens on **No** (0x10118dc0(2)). The dialog's handler (0x10222410): Confirm on Yes answers yes, on No or
  Cancel no; the callback 0x101f3330 on yes sends C2S 0x03A {u32 container} for the list's container (0x100ebe70; the
  inventory here, the only list this client sorts), redoes the display order (0x1021c320) and focuses the list
  (0x101f32f0); on no it does nothing, so the sort window keeps the focus. The config's autosort (cfg 154) still sorts
  without asking (0x1021c0c6). Other yes / no dialogs can open on Yes (startOnYes 1); `yesNoCursor()` takes either.
- **Drop asks first, on No** (#119; `src/ui/yes_no.h`): the item options' Drop (the Items handler at 0x101f2f96) opens
  the same dialog (0x102222d0 on [0x10634384]) with 55653 #62 "Dispose of item?", title 55652 #2 "Items" and
  startOnYes 0, so the cursor opens on **No**. Its yes callback (0x101f2c70) drops at once (0x101f2ba0: C2S 0x028, the
  whole stack) unless the item is Rare / Ex (the item record's 0xc000 flags) or a linkshell (item 513): those ask again
  with the "menu comyn" dialog (0x10631a00, 0x102109e0 with startOnYes 0, so No again), 55649 #48 "Really dispose of
  the %s?" (string table 0xa) with the item's name. Not followed: two more checks on that path (0x101cb450 /
  0x101cb540) and the linkshell's own prompts (55648 #343 / #344 via 0x101f2bf0). The window our first prompt uses,
  "menu yesno", is INFERRED (the dialog object's window choice was not traced).
- **Which yes / no prompts open on No**: the fourth argument of the "menu comyn" dialog (0x102109e0; 0 -> cursor on
  element 2) is 0 for Log out (0x10160950) and Shut down (0x101608c3), the home point prompt (0x1012dfbc), and the macro
  book / set Paste / Delete prompts (0x101a5da0 #238, 0x101a6787 #239 / #240); the Config resets already opened on No.
  Not traced, left on Yes: the Raise and tractor offers (0x10096e70 passes 1 for a prompt not identified; 0x10096d19
  passes a register) and the auction confirmations ("menu aucyn").
- **Use** ("menu iuse" element 1): items the menu can use and enchanted gear; plain gear is not equipped from here.
  An item whose targets (record +0x0C) reach others picks its target first (help 4).
- **Item info window** (0x1013cdf0, layout table 0x1036fa38, English row at 0x1036fa4a): icon (8,12); name at (0x33,4)
  white, green when worn; marks from (0x13a,4) left to right (0x1013cb80; "menu windowps" from 0x1036fa2c): 293 exdata
  mark (not drawn), 294 Rare (flag 0x8000), 295 Ex (0x4000), 296 in the temporary items; second line (0x33,20) for gear:
  weapons "(Skill)" (55653 #24..42 by skill, 0x1015a9e0), armour "[Slot]" (#14..23, 0x1015ac00), then the races
  (#0..13, 0x1015ad10: exact mask first, else the first sharing a bit). The level / jobs line and description rows come
  from line codes (0x1013d560) not traced in full: here the description, then "Lv.N jobs".
- **Equipment Sets** (Macros -> "Edit Equip. Set", "menu mcres20" 5 pages of 20, options "menu mcresmn": help
  831..837, 921; editor "menu mcresed"): Edit, Name, Copy, Paste, Delete, Use Current, Equip (C2S 0x051), Style Lock
  (0x053 Mode 3). Where the game keeps sets and how its rows read were not traced (INFERRED): here the text file of
  `/equipset` and "N name" rows.

### 10.13 Compass and Vana'diel clock above the log (issue #120, 2026-10-26; `src/ui/compass.h`, `GameUi::drawCompass`)

One object holds both (0x10634364, 0x3C bytes, ctor 0x1021ea10, created by 0x1021e890). The field opens it (0x10160050 /
0x10160af1: game state 0x60, no window with registry flag 4 open) and every frame 0x1021e8f0 runs its fade (0x1021eda0)
and draws it (0x1021ff10) unless 0x105781cc +0x48 is set.

- **Place.** x 0x58 (ctor +0x28, passed through the HUD scaling 0x10160200), y = the top of log window 1 (the log
  manager stores it through 0x1021e980 at 0x1015fbe9) less 0x2A (+0x2C): the dial's centre, 88 and 42 above the log.
  ffxi-linux uses its own log window 1's top (`logTop_`), so the compass moves with the log's height.
- **Fade in** (0x1021eda0): state 1 waits for the zone (0x10485824 bit 0), then 24 ticks (0x10385ECC) take the alpha
  +0x20 0 -> 0x80 and the size +0x24 0 -> 1; the dial also swings in from half a turn ((1 - f) * pi). ffxi-linux restarts
  it on every zone-in (60 ticks a second).
- **The dial** (0x1021ff10): sheet "menu compass" (alias row 3 of 0x1038a8c0, loaded into 0x1063436c by 0x102207e0,
  sprites through 0x10220850(1, n)). Sprite 0 (a 256x256 disc of four mirrored 32x32 quarters) is drawn with the matrix
  scale (+0x30 x +0x24 x 0.4), turn (0x10027b50: 3pi/2 - the camera's yaw, 0x10220780) and a tip back about x by
  1.0996 rad (0x10027ab0, 0xBF8CBE4C), so it lies like an ellipse. The letters are sprites 4 N (red), 1 E, 3 S, 2 W
  ("menu news" 16x16), drawn upright at the dial's (0, 140), (-140, 0), (0, -140), (140, 0) through the same matrix, in
  0x808080 with the fade's alpha. ffxi-linux turns the dial from the camera's forward vector (north toward where the
  camera looks north; the game's own yaw convention was not followed, INFERRED the same).
- **Reach by job** (0x1021eac0 -> 0x1021e9d0): range +0x34 = 0.8 x 36 = 28.8 yalms and size +0x30 0.8; a Ninja (main, or
  support) 36 and 1.0. +0x38 = range / 2. +0x2F (monsters shown) for Thief, Beastmaster, Ranger, Ninja, Summoner, Blue
  Mage, main or active support job.
- **Dots** (the loop over the 0x900 actors at 0x10480af0): "menu compass" 5 (a 6x6 marker, additive), at the actor's
  offset x 2 x the fade through the dial's turn and tip. Shown within the range and 4 yalms of height; alpha full inside
  range / 2, fading to 0 at the range, and fading again past 2 yalms of height (gone at 4). Colour by spawn flags
  (+0x1D0): players 0x0040F0, party / alliance (bits 0x0C) 0x3060E0, flag 0x100 0x30D000, NPCs (bit 2) 0x00B040 unless
  their +0x170 is 0x31, monsters (only with +0x2F) 0x808080, dead (+0x170 2 / 3) 0x206060. ffxi-linux: players, party,
  NPCs and monsters as listed; the 0x100 kind, the 0x31 NPCs and the actor's +0x210 check on players were not mapped
  (INFERRED: not drawn / drawn); doors, lifts and ships and hidden actors are skipped (INFERRED for the visibility test
  0x10098140).
- **Not on ships** (0x100ad6f0: zones 1, 3, 0x2E, 0x2F, 0x3A..0x3C, 0xDC, 0xDD, 0xDF..0xE4): no dial or dots, the clock
  line still shows. ffxi-linux also leaves both out in the Mog House and under the map and the full-screen log
  (INFERRED from the flag-4 window test).
- **Clock line** (0x1021fd50, shown while +0x2E, which 0x1021eaa0 toggles): "%d:%02d  %s" (0x10385F5C) with the
  Vana'diel hour (0x10103a40) and minute (0x10103a60) and our map square from 0x101f71b0: "(%c-%d)" (0x10384FEC) on the
  zone's map, "(?-?)" (0x10384FE4) without one, refreshed every 30 ticks (0x10385ED0). White (0x80808080) in the menu font
  at the dial's x - 0x32, y + 0x1A. The day's icon is "menu frames" sprite by weekday (table 0x10385EE8: 106 fire, 109
  earth, 111 water, 108 wind, 107 ice, 110 thunder, 112 light, 113 dark) at x - 0x44, y + 0x1A; the weather's is "font
  usgaiji" glyph table[(weather - 4) / 2 + 8] (0 fire, 5 water, 3 earth, 2 wind, 1 ice, 4 thunder, 6 light, 7 dark) at
  x - 0x44, y - 0x2A, twice side by side for the strong (odd) weathers 4..19.
- Not done: the target / event marker the dial draws last (0x100f9400 / 0x1021eb90: an animated arrow from sheet 0xC
  toward a point, 0.8 x the range out, and a name through 0x1008a810).

### 10.14 Scrolled item lists stop at the ends (issue #124, 2026-10-26; `src/ui/list_cursor.h`)

The item lists share one class (ctor 0x101f6370, vtable slot 6 = key handler 0x101f6820; 25 subclasses: Items, the
Equipment slot lists, trade, storage, shops, deliveries, search results ...). Its events (jump table 0x101f6b7c /
0x101f6ba8): 1 Down (0x101f694b), 2 Up (0x101f6888), 3 / 4 a page down / up (0x101f6a0e / 0x101f68d7, by the rows on a
page +0x18), 22..25 the same without the cursor. The layout's own arrow handling (0x10118201) leaves the cursor where it
is at the first / last row (that row's nav byte points to itself), and the list then scrolls:
- **Up** on row 1 (window cursor +0x4c and the last cursor +0x24 both 1): the scroll +0x1e goes down by one, clamped at
  0; at the top nothing happens and no cursor sound plays.
- **Down** on the last row of the page: the scroll goes up by one, clamped at the last page (+0x22), again silently at
  the end. In a list shorter than the page, the cursor is put back on the last item (0x101f69a6, 0x10118ce0(+0x20)).
- **Page steps** add or take a page and clamp to 0 / the last page.

So nothing wraps. ffxi-linux: `listCursorStep` clamps Up / Down and the page steps for every scrolled list
(`GameUi::isList`); the event menu (an NPC's options) keeps wrapping, as its window was not traced to this class
(INFERRED).

**Which keys page (#121, 2026-10-27).** Events 1..4 are the four arrow directions: the layout code turns them into an
element's navigation byte (0x1011b150: event 1 reads +0x18, 2 +0x17, 3 +0x19, 4 +0x1a; see §1, element record), and
the "menu equip" grid fixes which byte is which way: element 1 at x 24 has +0x19 = 5 (the element at x 58, to its
right) and +0x1a = 1 (itself, nothing further left). So **event 3 = Right** and **event 4 = Left**, and in the list
class **Right pages down, Left pages up** (3 adds a page to the scroll, 4 takes one away). The class constructor sets
+0x47 = 1, which lets events 3 / 4 page; a subclass can still claim them through vtable +0x6c (not traced per list).
No arrow key moves to the Items sort window (that is Select Window, §10.12, #117). The user reported Left as page
down (#121); the trace says otherwise, so ffxi-linux keeps Right = page down, Left = page up (`listKeyStep`, the same
steps as Page Down / Page Up).
