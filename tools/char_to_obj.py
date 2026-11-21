#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference decoder for FFXI character models (Sk2 type 41, Os2 type 42).

Usage:
  FFXI_ROOT="/path/to/FINAL FANTASY XI" char_to_obj.py RACE FACE HEAD BODY HANDS LEGS FEET MAIN SUB RANGED OUT.obj
         [--raw] [--no-mirror] [--stats] [--textures]

RACE is the client race index (1 Hume M, 2 Hume F, 3 Elvaan M, 4 Elvaan F, 5 Taru M, 6 Taru F, 7 Mithra, 8 Galka).
FACE is the face byte (low byte of GrapIDTbl[0]); the slot arguments are the
model ids of GrapIDTbl[1..8], either with the slot nibble (0x1000 + id ...) or bare ids.
Pass -1 to leave a slot empty.

  char_to_obj.py npc MODEL_ID OUT.obj [--raw] [--no-mirror] [--textures]
exports a monster/NPC model id (S2C 0x00E model_id) whose file holds its own
Sk2 + Os2 chunks (file id from npc_file_id()).

The model is assembled the way FFXiMain.dll does it (docs/dat-format.md section 10):
file id = per-race range table lookup (0x1035f388 / 0x1035f420), skeleton from the
race base file, every Os2 chunk in each slot file skinned in the bind pose.
Output is one OBJ (one group per mesh, usemtl = 16-char texture name, spaces -> '_')
plus a .mtl that references <texture name>.png; --textures also decodes the
textures found in the loaded files into PNGs next to the OBJ.

Game coordinates are Y-down left-handed (D3D).  By default the OBJ is rotated 180
degrees about X ((x,y,z) -> (x,-y,-z)) so it is Y-up, and the face winding is
reversed for the OBJ counter-clockwise convention.  --raw writes game coordinates.
"""
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zone_to_obj as zt  # noqa: E402  (dat_path, FTABLE/VTABLE resolver)

zt.ROOT = os.environ.get("FFXI_ROOT") or zt.ROOT

# FFXiMain.dll 0x1035f420: u16 base (skeleton + motions) file id per race index 0..36
RACE_BASE = [7072, 7072, 10248, 13424, 16600, 19776, 19776, 23176, 26352] + [7072] * 20 + \
            [30696, 29912, 30304, 55295, 55329, 55363, 55397, 55431]

# FFXiMain.dll 0x1035f388 -> per race, 9 slots x 6 (startFileId, count) ranges.
# Slot 0 face, 1 head, 2 body, 3 hands, 4 legs, 5 feet, 6 main, 7 sub, 8 ranged.
def _armor(face, h, b, g, l, f, m, s, r):
    return [face, h, b, g, l, f, m, s, r]


_R0 = _armor([(7080, 32)],
             [(7112, 256), (63323, 48), (63371, 16), (71247, 256), (98787, 32), (102961, 64)],
             [(7368, 256), (63387, 48), (63435, 16), (71503, 256), (98819, 32), (103025, 64)],
             [(7624, 256), (63451, 48), (63499, 16), (71759, 256), (98851, 32), (103089, 64)],
             [(7880, 256), (63515, 48), (63563, 16), (72015, 256), (98883, 32), (103153, 64)],
             [(8136, 256), (63579, 48), (63627, 16), (72271, 256), (98915, 32), (103217, 64)],
             [(8392, 512), (63643, 128), (72527, 256), (107301, 300), (0, 64)],
             [(41199, 512), (66459, 128), (81999, 256), (105201, 300), (0, 64)],
             [(9416, 256)])
SLOT_RANGES = {
    0: _R0, 1: _R0,
    2: _armor([(10256, 32)],
              [(10288, 256), (63771, 48), (63819, 16), (72783, 256), (98947, 32), (103281, 64)],
              [(10544, 256), (63835, 48), (63883, 16), (73039, 256), (98979, 32), (103345, 64)],
              [(10800, 256), (63899, 48), (63947, 16), (73295, 256), (99011, 32), (103409, 64)],
              [(11056, 256), (63963, 48), (64011, 16), (73551, 256), (99043, 32), (103473, 64)],
              [(11312, 256), (64027, 48), (64075, 16), (73807, 256), (99075, 32), (103537, 64)],
              [(11568, 512), (64091, 128), (74063, 256), (107601, 300), (0, 64)],
              [(42479, 512), (66587, 128), (82255, 256), (105501, 300), (0, 64)],
              [(12592, 256)]),
    3: _armor([(13432, 32)],
              [(13464, 256), (64219, 48), (64267, 16), (74319, 256), (99107, 32), (103601, 64)],
              [(13720, 256), (64283, 48), (64331, 16), (74575, 256), (99139, 32), (103665, 64)],
              [(13976, 256), (64347, 48), (64395, 16), (74831, 256), (99171, 32), (103729, 64)],
              [(14232, 256), (64411, 48), (64459, 16), (75087, 256), (99203, 32), (103793, 64)],
              [(14488, 256), (64475, 48), (64523, 16), (75343, 256), (99235, 32), (103857, 64)],
              [(14744, 512), (64539, 128), (75599, 256), (107901, 300), (0, 64)],
              [(43759, 512), (66715, 128), (82511, 256), (105801, 300), (0, 64)],
              [(15768, 256)]),
    4: _armor([(16608, 32)],
              [(16640, 256), (64667, 48), (64715, 16), (75855, 256), (99267, 32), (103921, 64)],
              [(16896, 256), (64731, 48), (64779, 16), (76111, 256), (99299, 32), (103985, 64)],
              [(17152, 256), (64795, 48), (64843, 16), (76367, 256), (99331, 32), (104049, 64)],
              [(17408, 256), (64859, 48), (64907, 16), (76623, 256), (99363, 32), (104113, 64)],
              [(17664, 256), (64923, 48), (64971, 16), (76879, 256), (99395, 32), (104177, 64)],
              [(17920, 512), (64987, 128), (77135, 256), (108201, 300), (0, 64)],
              [(45039, 512), (66843, 128), (82767, 256), (106101, 300), (0, 64)],
              [(18944, 256)]),
    5: _armor([(19784, 32)],
              [(19816, 256), (65115, 48), (65163, 16), (77391, 256), (99427, 32), (104241, 64)],
              [(20072, 256), (65179, 48), (65227, 16), (77647, 256), (99459, 32), (104305, 64)],
              [(20328, 256), (65243, 48), (65291, 16), (77903, 256), (99491, 32), (104369, 64)],
              [(20584, 256), (65307, 48), (65355, 16), (78159, 256), (99523, 32), (104433, 64)],
              [(20840, 256), (65371, 48), (65419, 16), (78415, 256), (99555, 32), (104497, 64)],
              [(21096, 512), (65435, 128), (78671, 256), (108501, 300), (0, 64)],
              [(46319, 512), (66971, 128), (83023, 256), (106401, 300), (0, 64)],
              [(22120, 256)]),
    6: _armor([(22952, 32)],
              [(19816, 256), (65115, 48), (65171, 16), (77391, 256), (99443, 32), (104241, 64)],
              [(20072, 256), (65179, 48), (65235, 16), (77647, 256), (99475, 32), (104305, 64)],
              [(20328, 256), (65243, 48), (65299, 16), (77903, 256), (99507, 32), (104369, 64)],
              [(20584, 256), (65307, 48), (65363, 16), (78159, 256), (99539, 32), (104433, 64)],
              [(20840, 256), (65371, 48), (65427, 16), (78415, 256), (99571, 32), (104497, 64)],
              [(21096, 512), (65435, 128), (78671, 256), (108501, 300), (0, 64)],
              [(46319, 512), (66971, 128), (83023, 256), (106401, 300), (0, 64)],
              [(22120, 256)]),
    7: _armor([(23184, 32)],
              [(23216, 256), (65563, 48), (65611, 16), (78927, 256), (99587, 32), (104561, 64)],
              [(23472, 256), (65627, 48), (65675, 16), (79183, 256), (99619, 32), (104625, 64)],
              [(23728, 256), (65691, 48), (65739, 16), (79439, 256), (99651, 32), (104689, 64)],
              [(23984, 256), (65755, 48), (65803, 16), (79695, 256), (99683, 32), (104753, 64)],
              [(24240, 256), (65819, 48), (65867, 16), (79951, 256), (99715, 32), (104817, 64)],
              [(24496, 512), (65883, 128), (80207, 256), (108801, 300), (0, 64)],
              [(47599, 512), (67099, 128), (83279, 256), (106701, 300), (0, 64)],
              [(25520, 256)]),
    8: _armor([(26360, 32)],
              [(26392, 256), (66011, 48), (66059, 16), (80463, 256), (99747, 32), (104881, 64)],
              [(26648, 256), (66075, 48), (66123, 16), (80719, 256), (99779, 32), (104945, 64)],
              [(26904, 256), (66139, 48), (66187, 16), (80975, 256), (99811, 32), (105009, 64)],
              [(27160, 256), (66203, 48), (66251, 16), (81231, 256), (99843, 32), (105073, 64)],
              [(27416, 256), (66267, 48), (66315, 16), (81487, 256), (99875, 32), (105137, 64)],
              [(27672, 512), (66331, 128), (81743, 256), (109101, 300), (0, 64)],
              [(48879, 512), (67227, 128), (83535, 256), (107001, 300), (0, 64)],
              [(28696, 256)]),
}

# FFXiMain.dll 0x103515f8: mirror sign vectors selected by bone-ref bits 14..15
MIRROR_SIGN = [(1.0, 1.0, 1.0), (-1.0, 1.0, 1.0), (1.0, -1.0, 1.0), (1.0, 1.0, -1.0)]


def slot_file_id(race, slot, look):
    """GrapIDTbl[slot] -> file id, as FUN_100d2e90 does it. Returns None if out of range."""
    idx = look & 0xFFF
    acc = 0
    for start, count in SLOT_RANGES[race][slot]:
        if idx < acc + count:
            return start - acc + idx if start else None
        acc += count
    return None


def npc_file_id(model):
    """S2C 0x00E monster/NPC model id -> file id (FUN_100c50f0 kind 0, 0x100c5139)."""
    if model >= 3500:
        return 101739 + model - 3500
    if model >= 3000:
        return 99907 + model - 3000
    if model >= 1500:
        return 51795 + model - 1500
    return 1300 + model


# ---------------------------------------------------------------- chunks
def chunks(fid):
    path = zt.dat_path(fid)
    if not path or not os.path.exists(path):
        return []
    d = open(path, "rb").read()
    out, pos = [], 0
    while pos + 16 <= len(d):
        info = struct.unpack_from("<I", d, pos + 4)[0]
        size = ((info >> 7) & 0x7FFFF) * 16
        if size == 0:
            break
        out.append((info & 0x7F, d[pos:pos + 4].decode("latin1").rstrip("\0 "), d[pos + 16:pos + size]))
        pos += size
    return out


# ---------------------------------------------------------------- Sk2
def _qmat(x, y, z, w):  # D3DXMatrixRotationQuaternion (row-vector convention)
    return [[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
            [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
            [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]]


def _mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def parse_sk2(p):
    """Returns (bones, world) where bones[i] = (parent, flag, quat xyzw, trans) and
    world[i] is the bind-pose bone->model matrix (4x4, row vectors)."""
    n = struct.unpack_from("<H", p, 2)[0]
    bones, world = [], []
    for i in range(n):
        o = 4 + i * 30
        q = struct.unpack_from("<4f", p, o + 2)
        t = struct.unpack_from("<3f", p, o + 18)
        bones.append((p[o], p[o + 1], q, t))
        r = _qmat(*q)
        local = [r[0] + [0.0], r[1] + [0.0], r[2] + [0.0], list(t) + [1.0]]
        world.append(local if i == 0 else _mul(local, world[p[o]]))
    return bones, world


def _xf(v, m, w=1.0):
    return [v[0] * m[0][j] + v[1] * m[1][j] + v[2] * m[2][j] + w * m[3][j] for j in range(3)]


# ---------------------------------------------------------------- Os2
def parse_os2(p):
    """Decode one Os2 (type 42) payload. Returns None for the non-skinned type 1."""
    if p[0] > 1 or (p[2] & 0x7F) != 0:
        return None
    sec = [(struct.unpack_from("<I", p, 6 + 6 * i)[0] * 2, struct.unpack_from("<H", p, 10 + 6 * i)[0] * 2)
           for i in range(6)]
    o, s = sec[1]
    bone_tbl = struct.unpack_from("<%dH" % (s // 2), p, o)
    n1, n2 = struct.unpack_from("<2H", p, sec[2][0])
    refs = struct.unpack_from("<%dH" % (2 * (n1 + n2)), p, sec[3][0])
    vo = sec[4][0]
    verts = []
    for i in range(n1):
        x, y, z, nx, ny, nz = struct.unpack_from("<6f", p, vo + 24 * i)
        verts.append(((x, y, z), None, (nx, ny, nz), None, 1.0, 0.0))
    vo += 24 * n1
    for i in range(n2):
        x1, x2, y1, y2, z1, z2, w1, w2, nx1, nx2, ny1, ny2, nz1, nz2 = struct.unpack_from("<14f", p, vo + 56 * i)
        verts.append(((x1, y1, z1), (x2, y2, z2), (nx1, ny1, nz1), (nx2, ny2, nz2), w1, w2))
    # polygon command stream (sec 0), u16 words; see FUN_1002cab0
    o = sec[0][0]
    tex, groups = None, []
    while True:
        op = struct.unpack_from("<H", p, o)[0]
        if op == 0xFFFF:
            break
        if op & 0x8000:
            if (op >> 4) & 0xF == 0:      # 0x8000: texture name
                tex = p[o + 2:o + 18].decode("latin1")
                o += 18
            else:                         # 0x8010: material, 22 words
                o += 46
            continue
        n = struct.unpack_from("<H", p, o + 2)[0]
        tris = []
        if op == 0x0054:                  # triangle list: n x (3 idx, 3 uv)
            for t in range(n):
                q = o + 4 + 30 * t
                i3 = struct.unpack_from("<3H", p, q)
                uv = struct.unpack_from("<6f", p, q + 6)
                tris.append((i3, (uv[0:2], uv[2:4], uv[4:6])))
            o += 4 + 30 * n
        elif op == 0x5453:                # strip: first tri (3 idx, 3 uv), then n-1 x (idx, uv)
            q = o + 4
            a, b, c = struct.unpack_from("<3H", p, q)
            uv = struct.unpack_from("<6f", p, q + 6)
            ua, ub, uc = uv[0:2], uv[2:4], uv[4:6]
            tris.append(((a, b, c), (ua, ub, uc)))
            q += 30
            for t in range(1, n):
                d = struct.unpack_from("<H", p, q)[0]
                ud = struct.unpack_from("<2f", p, q + 2)
                q += 10
                a, b, ua, ub = b, c, ub, uc
                c, uc = d, ud
                if t & 1:
                    tris.append(((b, a, c), (ub, ua, uc)))
                else:
                    tris.append(((a, b, c), (ua, ub, uc)))
            o += 24 + 10 * n
        elif op == 0x0043:                # untextured triangle list: n x (3 idx, u32 colour)
            for t in range(n):
                tris.append((struct.unpack_from("<3H", p, o + 4 + 10 * t), None))
            o += 4 + 10 * n
        elif op == 0x4353:                # untextured strip: 3 idx + (n-1) idx + u32 colour
            idx = struct.unpack_from("<%dH" % (n + 2), p, o + 4)
            for t in range(n):
                a, b, c = idx[t], idx[t + 1], idx[t + 2]
                tris.append((((b, a, c) if t & 1 else (a, b, c)), None))
            o += 12 + 2 * n
        else:
            raise ValueError("unknown Os2 op 0x%04x at %d" % (op, o))
        groups.append((tex, tris))
    return dict(version=p[0], flags2=p[2], flags3=p[3], flags4=p[4], bone_tbl=bone_tbl,
                n1=n1, n2=n2, refs=refs, verts=verts, groups=groups)


def skin(mesh, world, mirror):
    """Bind-pose skinning. mirror=False: bone = tbl[ref & 0x7f]; mirror=True:
    bone = tbl[(ref >> 7) & 0x7f] and the bone-local vertex is first multiplied
    by MIRROR_SIGN[ref >> 14] (FUN_1002d270)."""
    refs = mesh["refs"]
    # payload byte 2 bit 7 set: refs index the mesh bone table (sec 1);
    # clear: refs are skeleton bone numbers directly (DATA: 621/621 meshes)
    tbl = mesh["bone_tbl"] if mesh["flags2"] & 0x80 else range(len(world))
    out = []
    for i, (p1, p2, n1, n2, w1, w2) in enumerate(mesh["verts"]):
        r1, r2 = refs[2 * i], refs[2 * i + 1]
        def one(ref, pos, nrm, w=1.0):
            if mirror:
                s = MIRROR_SIGN[ref >> 14]
                pos = (pos[0] * s[0], pos[1] * s[1], pos[2] * s[2])
                nrm = (nrm[0] * s[0], nrm[1] * s[1], nrm[2] * s[2])
                m = world[tbl[(ref >> 7) & 0x7F]]
            else:
                m = world[tbl[ref & 0x7F]]
            return _xf(pos, m, w), _xf(nrm, m, 0.0)
        if p2 is None:
            out.append(one(r1, p1, n1))
        else:
            # 2-bone positions/normals are stored pre-weighted: only the bone translations take w1/w2.
            a, na = one(r1, p1, n1, w1)
            b, nb = one(r2, p2, n2, w2)
            out.append(([a[k] + b[k] for k in range(3)], [na[k] + nb[k] for k in range(3)]))
    return out


# ---------------------------------------------------------------- textures (sec. 4)
def _dxt_block_colors(c0, c1, dxt1):
    def rgb(c):
        return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
    a, b = rgb(c0), rgb(c1)
    if c0 > c1 or not dxt1:
        return [a + (255,), b + (255,), tuple((2 * a[k] + b[k]) // 3 for k in range(3)) + (255,),
                tuple((a[k] + 2 * b[k]) // 3 for k in range(3)) + (255,)]
    return [a + (255,), b + (255,), tuple((a[k] + b[k]) // 2 for k in range(3)) + (255,), (0, 0, 0, 0)]


def decode_texture(p):
    """Returns (name, w, h, rgba bytes) for a type 0x20/0x1B payload, or None."""
    flags = p[0]
    name = p[1:17].decode("latin1")
    w, h = struct.unpack_from("<ii", p, 0x11 + 4)
    bpp = struct.unpack_from("<H", p, 0x11 + 0xE)[0]
    clr = struct.unpack_from("<I", p, 0x11 + 0x24)[0]
    kind = (flags >> 4) & 7
    o = 0x39
    px = bytearray(w * h * 4)
    if kind == 2:
        fourcc = p[o:o + 4][::-1].decode("latin1")
        o += 12
        dxt1 = fourcc == "DXT1"
        bs = 8 if dxt1 else 16
        for by in range(0, h, 4):
            for bx in range(0, w, 4):
                blk = p[o:o + bs]
                o += bs
                alpha = None
                if not dxt1:
                    av = int.from_bytes(blk[0:8], "little")
                    alpha = [((av >> (4 * k)) & 15) * 17 for k in range(16)]
                    blk = blk[8:]
                c0, c1, bits = struct.unpack_from("<HHI", blk, 0)
                cols = _dxt_block_colors(c0, c1, dxt1)
                for k in range(16):
                    x, y = bx + (k & 3), by + (k >> 2)
                    if x < w and y < h:
                        c = cols[(bits >> (2 * k)) & 3]
                        a = alpha[k] if alpha else c[3]
                        px[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((c[0], c[1], c[2], a))
    elif kind in (0, 1, 3) and bpp in (4, 8):
        if kind == 3:
            o += 4
        esz = max(clr // 8, 3) if clr else 4
        ncol = 16 if bpp == 4 else 256
        pal = [p[o + esz * k:o + esz * k + esz] for k in range(ncol)]
        o += ncol * esz
        for i in range(w * h):
            v = (p[o + i // 2] >> (4 * (i & 1))) & 15 if bpp == 4 else p[o + i]
            e = pal[v]
            px[i * 4:i * 4 + 4] = bytes((e[2], e[1], e[0], e[3] if esz == 4 else 255))
    else:
        return None
    # PS2 alpha convention: 0x80 = opaque (sec. 4)
    for i in range(3, len(px), 4):
        px[i] = min(255, px[i] * 2)
    return name, w, h, bytes(px)


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------- assemble
def build(race, face, looks, mirror=True, log=print, npc=None):
    if npc is not None:
        base = npc_file_id(npc)
        files = [("npc", base)]
    else:
        base = RACE_BASE[race]
        files = [("base", base)]
        files.append(("face", slot_file_id(race, 0, face)))
        for s, look in enumerate(looks, start=1):
            if look is None or look < 0:
                continue
            files.append(("slot%d" % s, slot_file_id(race, s, look)))
    skel = None
    for t, name, p in chunks(base):
        if t == 41:
            skel = parse_sk2(p)
            break
    if skel is None:
        raise SystemExit("no Sk2 in base file %d" % base)
    bones, world = skel
    meshes, textures = [], {}
    for label, fid in files:
        if fid is None:
            log("%-6s: out of range" % label)
            continue
        cs = chunks(fid)
        names = []
        for t, name, p in cs:
            if t in (0x20, 0x1B):
                textures.setdefault(p[1:17].decode("latin1"), p)
            if t != 42:
                continue
            m = parse_os2(p)
            if m is None:
                names.append(name + "(type1)")
                continue
            names.append(name)
            copies = [False] + ([True] if (mirror and (m["flags4"] & 1)) else [])
            for mir in copies:
                meshes.append(("%s_%d_%s%s" % (label, fid, name, "_m" if mir else ""), m, skin(m, world, mir), mir))
        log("%-6s: file %d %s  Os2 %s" % (label, fid, zt.dat_path(fid), names))
    return bones, world, meshes, textures


def write_obj(path, meshes, raw=False, with_textures=None):
    base = os.path.splitext(path)[0]
    mtl = base + ".mtl"
    used = set()
    lines = ["mtllib %s" % os.path.basename(mtl)]
    vi = vt = vn = 0
    stats = dict(tris=0, verts=0, front_ok=0, front_bad=0)
    lo, hi = [1e9] * 3, [-1e9] * 3
    for gname, m, sk, mir in meshes:
        lines.append("g %s" % gname)
        for (pos, nrm) in sk:
            x, y, z = pos
            nx, ny, nz = nrm
            for k in range(3):
                lo[k] = min(lo[k], pos[k])
                hi[k] = max(hi[k], pos[k])
            if not raw:
                y, z, ny, nz = -y, -z, -ny, -nz
            lines.append("v %.5f %.5f %.5f" % (x, y, z))
            ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            lines.append("vn %.4f %.4f %.4f" % (nx / ln, ny / ln, nz / ln))
        stats["verts"] += len(sk)
        for tex, tris in m["groups"]:
            mname = (tex or "untextured").replace(" ", "_")
            used.add((mname, tex))
            lines.append("usemtl %s" % mname)
            for (a, b, c), uvs in tris:
                if a == b or b == c or a == c:
                    continue
                if mir:      # reflection reverses the winding
                    a, b = b, a
                    if uvs:
                        uvs = (uvs[1], uvs[0], uvs[2])
                # winding check: game space front faces are clockwise -> (B-A)x(C-A) opposite to the normal
                A, B, C = sk[a][0], sk[b][0], sk[c][0]
                e1 = [B[k] - A[k] for k in range(3)]
                e2 = [C[k] - A[k] for k in range(3)]
                cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                nsum = [sk[a][1][k] + sk[b][1][k] + sk[c][1][k] for k in range(3)]
                if sum(cr[k] * nsum[k] for k in range(3)) < 0:
                    stats["front_ok"] += 1
                else:
                    stats["front_bad"] += 1
                stats["tris"] += 1
                if uvs:
                    for u, v in uvs:
                        lines.append("vt %.5f %.5f" % (u, 1.0 - v))
                    t0 = vt
                    vt += 3
                    # OBJ wants CCW front faces: emit (a, c, b)
                    lines.append("f %d/%d/%d %d/%d/%d %d/%d/%d" % (vi + a + 1, t0 + 1, vn + a + 1,
                                                                   vi + c + 1, t0 + 3, vn + c + 1,
                                                                   vi + b + 1, t0 + 2, vn + b + 1))
                else:
                    lines.append("f %d//%d %d//%d %d//%d" % (vi + a + 1, vn + a + 1, vi + c + 1, vn + c + 1,
                                                             vi + b + 1, vn + b + 1))
        vi += len(sk)
        vn += len(sk)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(mtl, "w") as f:
        for mname, tex in sorted(used, key=lambda x: x[0]):
            f.write("newmtl %s\nKd 1 1 1\n" % mname)
            if tex:
                f.write("map_Kd %s.png\n" % mname)
    stats["bbox_game"] = (lo, hi)
    return stats, used


def main(argv):
    flags = {a for a in argv if a.startswith("--")}
    args = [a for a in argv if not a.startswith("--")]
    if len(args) == 3 and args[0] == "npc" and zt.ROOT:
        out = args[2]
        bones, world, meshes, textures = build(0, 0, [], mirror="--no-mirror" not in flags, npc=int(args[1], 0))
    elif len(args) == 11 and zt.ROOT:
        race, face = int(args[0], 0), int(args[1], 0)
        looks = [int(a, 0) for a in args[2:10]]
        out = args[10]
        bones, world, meshes, textures = build(race, face, looks, mirror="--no-mirror" not in flags)
    else:
        print(__doc__)
        return 2
    stats, used = write_obj(out, meshes, raw="--raw" in flags)
    if "--textures" in flags:
        d = os.path.dirname(os.path.abspath(out))
        for mname, tex in used:
            if tex and tex in textures:
                r = decode_texture(textures[tex])
                if r:
                    write_png(os.path.join(d, mname + ".png"), r[1], r[2], r[3])
    lo, hi = stats["bbox_game"]
    print("bones %d, meshes %d, verts %d, tris %d, winding cw-in-game %d / ccw %d" %
          (len(bones), len(meshes), stats["verts"], stats["tris"], stats["front_ok"], stats["front_bad"]))
    print("bbox (game, Y down): x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f  height %.3f" %
          (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], hi[1] - lo[1]))
    missing = sorted(t for _, t in used if t and t not in textures)
    if missing and "--stats" in flags:
        print("textures not found in the loaded files:", missing)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
