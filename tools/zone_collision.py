#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference decoder for the FFXI zone collision mesh (the collision block inside the Mzb chunk).

Usage:
  FFXI_ROOT="/path/to/FINAL FANTASY XI"  zone_collision.py ZONE [options]

Options:
  --obj OUT.obj        write the collision mesh in world space (OBJ is Y-up: game Y is negated;
                       one group per placement, `usemtl matN` / `usemtl barrier`)
  --height X Z [Y]     list every collision surface crossed by the vertical line at (X, Z) and,
                       if Y is given, the first surface at or below Y (game space is Y-down, so
                       "below" means larger y)
  --stats              print header, counts, wall/floor statistics and self-checks

Format notes and evidence: docs/dat-format.md section 7. Mzb de-obfuscation and DAT lookup are
shared with tools/zone_to_obj.py (section 5).
"""
import math
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zone_to_obj as zt  # noqa: E402  (dat_path, zone_model_fid, walk_chunks, decode_mzb)

CELL = 4.0  # world units per grid cell (FFXiMain multiplies by 0.25 at 0x1016931c)


class Mesh:
    """One collision mesh: local-space vertices, normals and 8-byte triangles."""
    __slots__ = ("off", "verts", "normals", "tris", "has_barrier")


class Placement:
    """One 0xC0-byte instance record: object->world matrix and attributes."""
    __slots__ = ("off", "m", "inv", "nrm", "flags", "data_off", "bytes4", "f_b0", "ymin", "ymax", "id_bc")

    def to_world(self, v):
        m, (x, y, z) = self.m, v
        return (x * m[0] + y * m[4] + z * m[8] + m[12],
                x * m[1] + y * m[5] + z * m[9] + m[13],
                x * m[2] + y * m[6] + z * m[10] + m[14])

    def normal_to_world(self, n):
        r, (x, y, z) = self.nrm, n
        return (x * r[0] + y * r[3] + z * r[6],
                x * r[1] + y * r[4] + z * r[7],
                x * r[2] + y * r[5] + z * r[8])

    @property
    def roofed(self):
        return (self.flags >> 2) & 1

    @property
    def map_id(self):
        return (((self.flags >> 26) & 3) << 3) | ((self.flags >> 3) & 7)


class Collision:
    pass


def material(t):
    """4-bit surface material from the top bit of each triangle word (client 0x101698e4)."""
    return (t[0] >> 15) | ((t[1] >> 15) << 1) | ((t[2] >> 15) << 2) | ((t[3] >> 15) << 3)


def parse_collision(p):
    """p = de-obfuscated Mzb payload. Returns Collision or None when header+0x08 is 0."""
    coff = struct.unpack_from("<I", p, 0x08)[0]
    if coff == 0:
        return None
    if p[3] < 0x1A:
        # Only one zone (0) uses Mzb version 0x17; its placement records are laid out differently
        # (0x70 bytes apart in the data) and are not decoded here.
        raise NotImplementedError("Mzb version 0x%02x collision is not supported" % p[3])
    c = Collision()
    c.version = p[3]
    c.dims = tuple(p[0x0C:0x10])                      # A, B, C, D
    a, b, cc, d = c.dims
    c.grid_w = a * cc // 4                             # columns (x)
    c.grid_h = b * d // 4                              # rows (z)
    c.half_x = a * cc * 0.5
    c.half_z = b * d * 0.5
    c.has_grid = p[0x1C] == 0                          # client: zone-record[0] = p[0x1C] + 1, grid iff == 1
    c.query_flags = p[0x1D]
    (c.mesh_count, mesh_off, c.list_count, list_off, grid_off,
     place_off, c.place_count) = struct.unpack_from("<7I", p, coff)
    c.hdr = (coff, mesh_off, list_off, grid_off, place_off)
    stride = 0xC0  # client: 0x80 below version 0x1A, 0xC0 otherwise (0x10167e60)

    # meshes: contiguous, each {u32 vOff, u32 nOff, u32 tOff, i16 nTri, u16 flag} + arrays
    c.meshes = {}
    q = mesh_off
    for _ in range(c.mesh_count):
        vo, no, to, nt, flag = struct.unpack_from("<3IhH", p, q)
        m = Mesh()
        m.off = q
        nv, nn = (no - vo) // 12, (to - no) // 12
        m.verts = [struct.unpack_from("<3f", p, vo + 12 * i) for i in range(nv)]
        m.normals = [struct.unpack_from("<3f", p, no + 12 * i) for i in range(nn)]
        m.tris = [struct.unpack_from("<4H", p, to + 8 * k) for k in range(nt)]
        m.has_barrier = flag != 0
        c.meshes[q] = m
        q = to + 8 * nt
    c.mesh_end = q

    # placements
    c.places = {}
    for i in range(c.place_count):
        o = place_off + i * stride
        f = struct.unpack_from("<48f", p, o)
        pl = Placement()
        pl.off = o
        pl.m, pl.inv, pl.nrm = f[0:16], f[16:32], f[32:41]
        pl.flags, pl.data_off = struct.unpack_from("<2I", p, o + 0xA4)
        pl.bytes4 = tuple(p[o + 0xAC:o + 0xB0])
        pl.f_b0 = struct.unpack_from("<I", p, o + 0xB0)[0]
        pl.ymin, pl.ymax = f[45], f[46]
        pl.id_bc = struct.unpack_from("<I", p, o + 0xBC)[0]
        c.places[o] = pl

    # cell entry lists: {u32 hdr, (u32 placementOff, u32 meshOff) * (hdr & 0x7FF), u32 next}
    c.lists = {}
    q = list_off
    for _ in range(c.list_count):
        h = struct.unpack_from("<I", p, q)[0]
        n = h & 0x7FF
        e = struct.unpack_from("<%dI" % (2 * n), p, q + 4)
        c.lists[q] = (h, [(e[2 * k], e[2 * k + 1]) for k in range(n)])
        q += 8 + 8 * n
    c.list_end = q

    # grid: grid_h rows of grid_w u32 offsets (0 = empty cell)
    c.cells = struct.unpack_from("<%dI" % (c.grid_w * c.grid_h), p, grid_off) if c.has_grid else ()
    return c


def load_collision(zone):
    """Return (Collision, [names of other Mzb chunks that also carry a collision block])."""
    path = zt.dat_path(zt.zone_model_fid(zone))
    if path is None:
        raise FileNotFoundError("zone %d: no model DAT" % zone)
    data = open(path, "rb").read()
    first, others = None, []
    for off, name, t, size in zt.walk_chunks(data):
        if t != 28 or name == "fune":  # CMoMzb_Init 0x10176ec0 skips 'fune'
            continue
        p = zt.decode_mzb(data[off + 16:off + size])
        if struct.unpack_from("<I", p, 0x08)[0] == 0:
            continue
        if first is None:
            first = parse_collision(p)
        else:
            others.append(name)
    return first, others


def cell_of(c, x, z):
    return int(math.floor((x + c.half_x) / CELL)), int(math.floor((z + c.half_z) / CELL))


def pairs_near(c, x, z, radius=1):
    """Unique (placement, mesh) pairs in the (2r+1)^2 cells around (x, z), like the client queries."""
    cx, cz = cell_of(c, x, z)
    seen = []
    for rz in range(max(0, cz - radius), min(c.grid_h - 1, cz + radius) + 1):
        for rx in range(max(0, cx - radius), min(c.grid_w - 1, cx + radius) + 1):
            lo = c.cells[rz * c.grid_w + rx]
            if lo:
                for e in c.lists[lo][1]:
                    if e not in seen:
                        seen.append(e)
    return seen


def world_tri(pl, m, t):
    return (pl.to_world(m.verts[t[0] & 0x7FFF]),
            pl.to_world(m.verts[t[1] & 0x3FFF]),
            pl.to_world(m.verts[t[2] & 0x3FFF]))


def heights(c, x, z):
    """Every surface crossed by the vertical line at (x, z): [(y, material, barrier, world normal, placement)]."""
    out = []
    for po, mo in pairs_near(c, x, z):
        pl, m = c.places[po], c.meshes[mo]
        for t in m.tris:
            a, b, d = world_tri(pl, m, t)
            den = (b[2] - d[2]) * (a[0] - d[0]) + (d[0] - b[0]) * (a[2] - d[2])
            if abs(den) < 1e-12:
                continue  # vertical in xz: a wall, not crossed by a vertical line
            l1 = ((b[2] - d[2]) * (x - d[0]) + (d[0] - b[0]) * (z - d[2])) / den
            l2 = ((d[2] - a[2]) * (x - d[0]) + (a[0] - d[0]) * (z - d[2])) / den
            l3 = 1.0 - l1 - l2
            if min(l1, l2, l3) < -1e-6:
                continue
            y = l1 * a[1] + l2 * b[1] + l3 * d[1]
            n = pl.normal_to_world(m.normals[t[3] & 0x7FFF])
            out.append((y, material(t), (t[2] >> 14) & 1, n, po))
    out.sort(key=lambda h: h[0])
    dedup = []
    for h in out:
        if not dedup or abs(dedup[-1][0] - h[0]) > 1e-4 or dedup[-1][1:3] != h[1:3]:
            dedup.append(h)
    return dedup


def ground_height(c, x, z, y_from):
    """First surface at or below y_from (Y-down: the smallest y >= y_from - 0.01)."""
    for h in heights(c, x, z):
        if h[0] >= y_from - 0.01:
            return h
    return None


def all_pairs(c):
    seen = set()
    for lo, (h, ents) in c.lists.items():
        for e in ents:
            seen.add(e)
    return sorted(seen)


def write_obj(c, out):
    nv = nt = 0
    with open(out, "w") as f:
        f.write("# FFXI collision mesh, game coords rotated 180 degrees about X: (x, -y, -z), Y-up, not mirrored\n")
        base = 1
        cur = None
        for po, mo in all_pairs(c):
            pl, m = c.places[po], c.meshes[mo]
            if po != cur:
                f.write("g p%x\n" % po)
                cur = po
            wv = [pl.to_world(v) for v in m.verts]
            for (x, y, z) in wv:
                f.write("v %.4f %.4f %.4f\n" % (x, -y, -z))
            last = None
            for t in m.tris:
                mat = "barrier" if (t[2] >> 14) & 1 else "mat%d" % material(t)
                if mat != last:
                    f.write("usemtl %s\n" % mat)
                    last = mat
                i0, i1, i2 = t[0] & 0x7FFF, t[1] & 0x3FFF, t[2] & 0x3FFF
                f.write("f %d %d %d\n" % (i0 + base, i2 + base, i1 + base))  # clockwise data -> CCW for OBJ
                nt += 1
            base += len(wv)
            nv += len(wv)
    return nv, nt


def stats(c):
    coff, mo, lo, go, po = c.hdr
    print("version 0x%02x dims %s grid %dx%d (cell %.0f, x/z range +-%.0f/+-%.0f) has_grid %s flags 0x%02x"
          % (c.version, c.dims, c.grid_w, c.grid_h, CELL, c.half_x, c.half_z, c.has_grid, c.query_flags))
    print("block @0x%x: meshes %d @0x%x (end 0x%x), lists %d @0x%x (end 0x%x), grid @0x%x, placements %d @0x%x"
          % (coff, c.mesh_count, mo, c.mesh_end, c.list_count, lo, c.list_end, go, c.place_count, po))
    pairs = all_pairs(c)
    print("used (placement, mesh) pairs %d, placements used %d, meshes used %d, non-empty cells %d"
          % (len(pairs), len({a for a, _ in pairs}), len({b for _, b in pairs}), sum(1 for x in c.cells if x)))
    walls = floors = ceil = barrier = degenerate = wind_agree = wind_total = 0
    mats = Counter()
    for po_, mo_ in pairs:
        pl, m = c.places[po_], c.meshes[mo_]
        for t in m.tris:
            a, b, d = world_tri(pl, m, t)
            n = pl.normal_to_world(m.normals[t[3] & 0x7FFF])
            ln = math.sqrt(sum(k * k for k in n)) or 1.0
            ny = n[1] / ln
            if abs(ny) < 0.2:
                walls += 1
            elif ny < 0:
                floors += 1     # normal points to -y = up in Y-down space
            else:
                ceil += 1
            barrier += (t[2] >> 14) & 1
            mats[material(t)] += 1
            e1 = [b[i] - a[i] for i in range(3)]
            e2 = [d[i] - a[i] for i in range(3)]
            cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            dot = sum(cr[i] * n[i] for i in range(3))
            if abs(dot) < 1e-9:
                degenerate += 1
            else:
                wind_total += 1
                wind_agree += dot > 0
    tot = walls + floors + ceil
    print("world triangles %d: walls (|n.y|<0.2) %d, floors (n.y<=-0.2) %d, ceilings %d, barrier-bit %d, degenerate %d"
          % (tot, walls, floors, ceil, barrier, degenerate))
    print("winding: (B-A)x(C-A) same direction as stored normal in %d of %d" % (wind_agree, wind_total))
    print("materials:", dict(sorted(mats.items())))
    print("placement flags:", dict(Counter(pl.flags for pl in c.places.values()).most_common(8)))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    zone = int(args.pop(0))
    c, others = load_collision(zone)
    if c is None:
        print("zone %d: no collision block" % zone)
        sys.exit(2)
    if others:
        print("note: other Mzb chunks with collision not merged:", others)
    while args:
        a = args.pop(0)
        if a == "--obj":
            out = args.pop(0)
            nv, nt = write_obj(c, out)
            print("wrote %s: %d vertices, %d triangles" % (out, nv, nt))
        elif a == "--height":
            x, z = float(args.pop(0)), float(args.pop(0))
            y0 = None
            if args and not args[0].startswith("--"):
                y0 = float(args.pop(0))
            print("cell", cell_of(c, x, z))
            for y, mat, bar, n, po in heights(c, x, z):
                print("  y %.3f  material %d  barrier %d  normal (%.2f %.2f %.2f)  placement 0x%x"
                      % (y, mat, bar, n[0], n[1], n[2], po))
            if y0 is not None:
                h = ground_height(c, x, z, y0)
                print("ground under y=%.3f: %s" % (y0, "none" if h is None else "%.3f" % h[0]))
        elif a == "--stats":
            stats(c)
        else:
            print("unknown option", a)
            sys.exit(1)


if __name__ == "__main__":
    main()
