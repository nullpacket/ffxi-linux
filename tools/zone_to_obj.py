#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference decoder for FFXI zone geometry (Mzb type 28 + Mmb type 46).

Usage:  FFXI_ROOT="/path/to/FINAL FANTASY XI"  zone_to_obj.py ZONE_ID OUT.obj [--stats]

Reads the zone's model DAT (file id = zone+100, or zone+83191 for 600..699),
decodes every Mmb (mesh) and the Mzb (placement table), places each instance in
world space and writes one Wavefront OBJ (positions, uvs, normals, faces, one
group per instance, `usemtl` = 16-char texture name with spaces replaced by '_').
Game coordinates are Y-down (left-handed D3D); the OBJ negates Y so it is
right-handed Y-up with counter-clockwise front faces.

Format notes and evidence: docs/dat-format.md section 5. Key tables are copied
from FFXiMain.dll (0x10376ac0 and 0x10376de8, 256 bytes each).
"""
import math
import os
import struct
import sys

ROOT = os.environ.get("FFXI_ROOT") or os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "ffxi-linux", "game")  # xi-setup import

# FFXiMain.dll 0x10376ac0 (identical copy at 0x10376ef0, used by the Mzb decoder)
KEY_TABLE = bytes.fromhex(
    "e2e506a9ed26f44215f4817fde9aded01a982091394948a40a9f4069ecbd8181"
    "8dad10b8c188150511b1aaf00f1e34e681aacdac0284330a19389ee6734a115d"
    "bf857708cdd9960d7978cc35068ef9fe66b9210320291e27ca8682e64507dda9"
    "b6d5a203ecad62452dce79bd8f2d1018e60a6faa6f4684329f292cc2f0eb186f"
    "f23adcea7b0c812dcceba151772cfb49e890f790ce5c01f35cf441ab04e716cc"
    "3a055455dceda4d6bf3f9e0893b5633890f75af0a25f56c80870cb2416ddd274"
    "953a1a2a74c49debaf69aa51396594a24b1f1a605239e823ee5839063d226a2d"
    "d29125a52e7162a50bc1e56e43497c5846199f4549c64009a2995b7b987fa0d0")
# FFXiMain.dll 0x10376de8
SWAP_TABLE = bytes.fromhex(
    "b8c5f784e45a237bc8901df65d0951c10724ef5b1d739008a5701c225f6bebb0"
    "06c72a3ad26681db4162f29717fe05efa3dc22b345703e182db4ba0a651d87c3"
    "12ce8f9df70d50243af3ca706b679cb2c24d6a0ca8fa81a679ebbefe89b7ac7f"
    "6543ec565b35da813cab6d28602c5f31ebdf8e0f4ffaa3da127ef1a5d222a00c"
    "868c0a0c06c76518cef2a368fe359695a6fa58634159eadd7fd31ba84844ab91"
    "fd13b16801ac3a11783033d84e6a89057b068eb086fd9fd7485404aef3061736"
    "533fa81153caa195c2cde61f57b47faaf36bf9a027d009eff6687360dc502a25"
    "0f77b9b0040be1cc353184e622f9c2ab959161d92bb9724e107631660a0b2e83")

# ---------------------------------------------------------------- file access
_tables = None


def _load_tables():
    global _tables
    if _tables is None:
        vt = bytearray(open(os.path.join(ROOT, "VTABLE.DAT"), "rb").read())
        raw = open(os.path.join(ROOT, "FTABLE.DAT"), "rb").read()
        ft = list(struct.unpack("<%dH" % (len(raw) // 2), raw))
        n = 2
        while os.path.exists(os.path.join(ROOT, "ROM%d" % n, "VTABLE%d.DAT" % n)):
            v = open(os.path.join(ROOT, "ROM%d" % n, "VTABLE%d.DAT" % n), "rb").read()
            f = open(os.path.join(ROOT, "ROM%d" % n, "FTABLE%d.DAT" % n), "rb").read()
            for i, b in enumerate(v):
                if b == n and i < len(vt):
                    vt[i] = n
                    ft[i] = struct.unpack_from("<H", f, i * 2)[0]
            n += 1
        _tables = (vt, ft)
    return _tables


def dat_path(fid):
    vt, ft = _load_tables()
    v, f = vt[fid], ft[fid]
    if v == 0:
        return None
    if v == 1:
        return os.path.join(ROOT, "ROM", str(f >> 7), "%d.DAT" % (f & 0x7F))
    return os.path.join(ROOT, "ROM%d" % v, str(f >> 7), "%d.DAT" % (f & 0x7F))


def zone_model_fid(zone):
    return zone + 83191 if 600 <= zone < 700 else zone + 100


def walk_chunks(data):
    """Yield (offset, name4, type, size_incl_header)."""
    p = 0
    while p + 16 <= len(data):
        info = struct.unpack_from("<I", data, p + 4)[0]
        size = ((info >> 7) & 0x7FFFF) * 16
        if size == 0:
            break
        yield p, data[p:p + 4].decode("latin1"), info & 0x7F, size
        p += size

# ---------------------------------------------------------------- decoders


def decode_mmb(payload):
    """FFXiMain 0x1016fbe0 (XOR stage) + 0x10170d90 (8-byte block swap)."""
    p = bytearray(payload)
    length = struct.unpack_from("<I", p, 0)[0] & 0xFFFFFF
    if p[3] >= 5:
        key = KEY_TABLE[p[5] ^ 0xF0]
        ctr = 0
        for i in range(8, length):
            x = key & 0xFF
            w = (x << 8) | x
            ctr += 1
            key += ctr
            p[i] ^= (w >> (key & 7)) & 0xFF
            ctr += 1
            key += ctr
    if p[6] == 0xFF and p[7] == 0xFF:
        k3 = p[5] ^ 0xF0
        k2 = SWAP_TABLE[k3]
        half = ((length - 8) & ~0xF) // 2
        for pos in range(0, half, 8):
            k3 = (k3 + 9) & 0xFF
            bit = k2 & 1
            k2 = (k2 + k3) & 0xFF
            if bit:
                a, b = 8 + pos, 8 + pos + half
                p[a:a + 8], p[b:b + 8] = p[b:b + 8], p[a:a + 8]
    return p


def decode_mzb(payload):
    """FFXiMain 0x10179340 prologue: NOT selected 16..23-byte runs."""
    p = bytearray(payload)
    length = (struct.unpack_from("<I", p, 0)[0] & 0xFFFFFF) - 8
    if p[3] >= 0x1B:
        key = KEY_TABLE[p[7] ^ 0xFF]
        ctr = 0
        pos = 0
        while pos < length:
            k = key & 0xFF
            ctr += 1
            key += ctr
            n = ((k >> 4) & 7) + 16
            if (k & 1) and pos + n < length:
                for j in range(8 + pos, 8 + pos + n):
                    p[j] ^= 0xFF
            pos += n
    return p

# ---------------------------------------------------------------- parsers


class Piece:
    __slots__ = ("texture", "flags", "verts", "indices", "strip")


def parse_mmb(p):
    """Return (name16, [Piece]). Vertex = (x,y,z, nx,ny,nz, rgba_u32, u,v)."""
    version = p[3]
    vtype = p[4]
    name = bytes(p[0x10:0x20])
    count = struct.unpack_from("<I", p, 0x20)[0]
    if version <= 1:
        count = 9
    stride = 0x30 if vtype == 2 else 0x24
    strip = vtype in (1, 3)
    pieces = []
    for i in range(count):
        off = struct.unpack_from("<I", p, 0x3C + 4 * i)[0]
        if off == 0:
            continue
        npieces = struct.unpack_from("<I", p, off)[0]
        q = off + 0x20
        for _ in range(npieces):
            pc = Piece()
            pc.texture = bytes(p[q:q + 16])
            w = struct.unpack_from("<I", p, q + 16)[0]
            nv = w & 0x0FFFFFFF
            pc.flags = w >> 28
            vb = q + 20
            pc.verts = []
            for k in range(nv):
                o = vb + k * stride
                x, y, z = struct.unpack_from("<3f", p, o)
                n_off = o + (0x18 if vtype == 2 else 0x0C)
                nx, ny, nz, col, u, v = struct.unpack_from("<3fI2f", p, n_off)
                pc.verts.append((x, y, z, nx, ny, nz, col, u, v))
            ib = vb + nv * stride
            ni = struct.unpack_from("<I", p, ib)[0]
            pc.indices = struct.unpack_from("<%dH" % ni, p, ib + 4)
            pc.strip = strip
            q = ib + 4 + ((ni + 1) & ~1) * 2
            pieces.append(pc)
    return name, pieces


def parse_mzb(p):
    """Return list of placements (name16, pos3, rot3, scale3, raw_record)."""
    version = p[3]
    n = struct.unpack_from("<I", p, 4)[0] & 0xFFFFFF
    out = []
    for i in range(n):
        r = bytearray(p[0x20 + i * 100:0x20 + (i + 1) * 100])
        if version > 0x1A:
            for j in range(16):
                r[j] ^= 0x55
        f = struct.unpack_from("<9f", r, 16)
        out.append((bytes(r[:16]), f[0:3], f[3:6], f[6:9], bytes(r)))
    return out


def triangles(piece):
    idx = piece.indices
    if piece.strip:
        for i in range(len(idx) - 2):
            a, b, c = idx[i], idx[i + 1], idx[i + 2]
            if a == b or b == c or a == c:
                continue
            yield (a, b, c) if i % 2 == 0 else (a, c, b)
    else:
        for i in range(0, len(idx) - 2, 3):
            yield idx[i], idx[i + 1], idx[i + 2]


def world_matrix(pos, rot, scale):
    """Row-vector matrix W = S * Rx * Ry * Rz * T (D3DX conventions)."""
    def mul(a, b):
        return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
    cx, sx = math.cos(rot[0]), math.sin(rot[0])
    cy, sy = math.cos(rot[1]), math.sin(rot[1])
    cz, sz = math.cos(rot[2]), math.sin(rot[2])
    S = [[scale[0], 0, 0, 0], [0, scale[1], 0, 0], [0, 0, scale[2], 0], [0, 0, 0, 1]]
    RX = [[1, 0, 0, 0], [0, cx, sx, 0], [0, -sx, cx, 0], [0, 0, 0, 1]]
    RY = [[cy, 0, -sy, 0], [0, 1, 0, 0], [sy, 0, cy, 0], [0, 0, 0, 1]]
    RZ = [[cz, sz, 0, 0], [-sz, cz, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    T = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [pos[0], pos[1], pos[2], 1]]
    return mul(mul(mul(mul(S, RX), RY), RZ), T)


def load_zone(zone):
    path = dat_path(zone_model_fid(zone))
    if path is None:
        raise FileNotFoundError("zone %d: no model DAT" % zone)
    data = open(path, "rb").read()
    mmbs, mzb = {}, []
    for off, cname, t, size in walk_chunks(data):
        if t == 46:
            dec = decode_mmb(data[off + 16:off + size])
            name, pieces = parse_mmb(dec)
            mmbs.setdefault(name, pieces)
        elif t == 28 and cname != "fune":  # CMoMzb_Init 0x10176ec0 skips 'fune'
            mzb += parse_mzb(decode_mzb(data[off + 16:off + size]))
    return mmbs, mzb


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    zone = int(sys.argv[1])
    out = sys.argv[2]
    mmbs, placements = load_zone(zone)
    nv = nt = missing = 0
    lo = [1e30] * 3
    hi = [-1e30] * 3
    with open(out, "w") as f:
        f.write("# FFXI zone %d, %d meshes, %d placements\n" % (zone, len(mmbs), len(placements)))
        base = 1
        for i, (name, pos, rot, scale, _raw) in enumerate(placements):
            pieces = mmbs.get(name)
            if pieces is None:
                missing += 1
                continue
            W = world_matrix(pos, rot, scale)
            mirror = scale[0] * scale[1] * scale[2] < 0
            f.write("g i%d_%s\n" % (i, name.decode("latin1").strip()))
            for pc in pieces:
                f.write("usemtl %s\n" % pc.texture.decode("latin1").replace(" ", "_"))
                for (x, y, z, nx, ny, nz, _c, u, v) in pc.verts:
                    wx = x * W[0][0] + y * W[1][0] + z * W[2][0] + W[3][0]
                    wy = x * W[0][1] + y * W[1][1] + z * W[2][1] + W[3][1]
                    wz = x * W[0][2] + y * W[1][2] + z * W[2][2] + W[3][2]
                    for k, val in enumerate((wx, wy, wz)):
                        lo[k] = min(lo[k], val)
                        hi[k] = max(hi[k], val)
                    # normal: rotation/scale part only (inverse-transpose ignored; fine for OBJ preview)
                    wnx = nx * W[0][0] + ny * W[1][0] + nz * W[2][0]
                    wny = nx * W[0][1] + ny * W[1][1] + nz * W[2][1]
                    wnz = nx * W[0][2] + ny * W[1][2] + nz * W[2][2]
                    # OBJ is right-handed Y-up: rotate 180 degrees about X, (x, -y, -z). Negating Y alone would mirror east-west.
                    f.write("v %.4f %.4f %.4f\nvt %.5f %.5f\nvn %.4f %.4f %.4f\n"
                            % (wx, -wy, -wz, u, 1.0 - v, wnx, -wny, -wnz))
                for a, b, c in triangles(pc):
                    # (x, -y, -z) is a rotation, so the data's clockwise fronts stay clockwise: swap to the
                    # counter-clockwise order OBJ viewers expect, except on mirrored instances.
                    if not mirror:
                        b, c = c, b
                    a, b, c = a + base, b + base, c + base
                    f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n" % (a, a, a, b, b, b, c, c, c))
                    nt += 1
                base += len(pc.verts)
                nv += len(pc.verts)
    print("zone %d: %d unique meshes, %d placements (%d unresolved), %d vertices, %d triangles"
          % (zone, len(mmbs), len(placements), missing, nv, nt))
    print("world bbox (game coords, Y down): x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f" % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))


if __name__ == "__main__":
    main()
