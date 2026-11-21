#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference parser for FFXI zone environment (Weather chunks, type 47): light, fog and sky.

Usage:
  FFXI_ROOT="/path/to/FINAL FANTASY XI"  zone_weather.py ZONE WEATHER TIME [options]
  FFXI_ROOT=...                          zone_weather.py ZONE --list

  ZONE     zone id (file = zone_model_fid(zone), see docs/dat-format.md section 3)
  WEATHER  server weather id 0..19 (LSB xi::Weather / S2C 0x00A WeatherNumber / 0x057 WeatherNumber)
           or a 4-char group name such as suny, clod, fine, mist
  TIME     Vana'diel time of day: "HH:MM", "HH:MM:SS" or decimal hours ("13.5")

Options:
  --alt              use the alternate fourcc table (fin1, sun1, ...), selected by event scripts
  --block NAME       top-level environment block to search (default "weat"; events use ev01, ev02, ...)
  --from ID --blend T  also blend from weather ID (old) to WEATHER (new) with factor T in 0..1,
                       like a server weather change in progress (S2C 0x057)
  --raw              dump the interpolated record field by field
  --list             list the environment blocks, groups and keyframes in the zone file

Format notes and evidence: docs/dat-format.md section 9. All offsets below are payload offsets
(chunk start + 16). Colours are stored R, G, B, A bytes (A is always 0x80).
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zone_to_obj as zt  # noqa: E402  (dat_path, zone_model_fid)

# FFXiMain 0x10377bb8 / 0x10377c08 (g_WeatherFourcc / g_WeatherFourccAlt), indexed by weather id.
FOURCC = ["fine", "suny", "clod", "mist", "dryw", "heat", "rain", "squl", "dust", "sand",
          "wind", "stom", "snow", "bliz", "thdr", "bolt", "aura", "ligt", "fogd", "dark"]
FOURCC_ALT = [n[:3] + "1" for n in FOURCC]          # fin1 sun1 clo1 mis1 ...
TICKS_PER_MIN = 3600                                  # 1 tick = 1/60 Vana'diel second
TICKS_PER_DAY = 24 * 60 * TICKS_PER_MIN               # 0x4F1A00

# ------------------------------------------------------------------ chunk walking


def walk(data):
    """Yield (offset, name4, type, size, block_path). Type 1 opens a block, type 0 closes it."""
    p, stack = 0, []
    while p + 16 <= len(data):
        info = struct.unpack_from("<I", data, p + 4)[0]
        t, size = info & 0x7F, ((info >> 7) & 0x7FFFF) * 16
        name = data[p:p + 4].decode("latin1")
        if t == 1:
            stack.append(name)
        elif t == 0:
            if not stack:
                return
            stack.pop()
            if not stack:
                return
        yield p, name, t, size, tuple(stack)
        p += size if size else 16


def key_ticks(name):
    """0x100720b0: chunk name 'HHMM' -> ticks. hh = c0*10+c1, mm = c2*10+c3 (ASCII digits)."""
    b = name.encode("latin1")
    hh = (b[0] - 48) * 10 + (b[1] - 48)
    mm = (b[2] - 48) * 10 + (b[3] - 48)
    return (hh * 60 + mm) * TICKS_PER_MIN


def load_weather(zone):
    fid = zt.zone_model_fid(zone)
    path = zt.dat_path(fid)
    data = open(path, "rb").read()
    recs = []  # (block_path, name, payload)
    for p, name, t, size, st in walk(data):
        if t == 47:
            recs.append((st, name, data[p + 16:p + size]))
    return fid, recs

# ------------------------------------------------------------------ record decode


class Rec:
    """The 0xAC-byte environment record (payload is 0xB0 with 4 bytes of padding)."""

    def __init__(self, b):
        self.b = bytearray(b[:0xAC])

    def u8(self, o):
        return self.b[o]

    def rgb(self, o):
        return tuple(self.b[o:o + 3])

    def f(self, o):
        return struct.unpack_from("<f", self.b, o)[0]

    def dir8(self, o):
        return tuple(struct.unpack_from("<3b", self.b, o))

    @property
    def explicit_dirs(self):
        return self.b[0] & 1


COLOUR_OFFS = [0x0C, 0x10, 0x14, 0x18, 0x2C, 0x30, 0x34, 0x38, 0x4C] + [0x6C + 4 * i for i in range(8)]
FLOAT_OFFS = [0x1C, 0x20, 0x24, 0x3C, 0x40, 0x44, 0x50, 0x54, 0x58, 0x68] + [0x8C + 4 * i for i in range(8)]
BYTE_OFFS = [0x5C, 0x5D, 0x5E]


def lerp_rec(a_new, b_old, t):
    """WeatherRec_Lerp 0x101893a0: out = old + (new - old) * t.
    Colour bytes: old + trunc((new - old) * t) per channel (0x10189660, unsigned, __ftol truncates).
    Floats: (new - old) * t + old (0x10189720). Not touched: +0x04, +0x08, +0x28, +0x48, +0x60, +0x64.
    Flag bit0 is OR-ed; with it set, the int8 directions at +0x10/+0x30 are rotated between the two
    keys (0x10033290) and renormalised. Here: normalised lerp, scaled back to 127."""
    out = bytearray(b_old.b)
    out[0] = (b_old.b[0] & ~1) | ((a_new.b[0] | b_old.b[0]) & 1)
    for o in COLOUR_OFFS:
        n = 4
        if out[0] & 1 and o in (0x10, 0x30):
            continue
        for i in range(n):
            na, ob = a_new.b[o + i], b_old.b[o + i]
            out[o + i] = (ob + int((na - ob) * t)) & 0xFF
    for o in BYTE_OFFS:
        na, ob = a_new.b[o], b_old.b[o]
        out[o] = (ob + int((na - ob) * t)) & 0xFF
    for o in FLOAT_OFFS:
        struct.pack_into("<f", out, o, (a_new.f(o) - b_old.f(o)) * t + b_old.f(o))
    if out[0] & 1:
        for o in (0x10, 0x30):
            va, vb = norm(a_new.dir8(o)), norm(b_old.dir8(o))
            v = norm(tuple(vb[i] + (va[i] - vb[i]) * t for i in range(3)))
            struct.pack_into("<3b", out, o, *(max(-127, min(127, int(c * 127))) for c in v))
    return Rec(out)


def norm(v):
    m = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / m for c in v)

# ------------------------------------------------------------------ selection + time


def select_group(recs, fourcc, block="weat"):
    """WeatherChannel_ctor 0x1018a6b0: find block 'weat', then its child block named fourcc;
    if missing, fall back to 'suny' (the client logs an error). Returns [(name, Rec)]."""
    for want in (fourcc, "suny"):
        keys = [(n, Rec(pl)) for st, n, pl in recs if block in st and want in st[st.index(block) + 1:]]
        if keys:
            return want, keys
    return None, []


def pick_keys(keys, now):
    """FindPrevKey 0x10189d40 / FindNextKey 0x1018a000.
    prev = key with the smallest (now - key) >= 0 (first in file order on ties);
    next = key with the smallest (key - now) > 0, else the earliest key (wraps past midnight)."""
    prev = nxt = None
    best_p = best_n = None
    earliest = None
    for n, r in keys:
        k = key_ticks(n)
        if k <= now and (best_p is None or now - k < best_p):
            best_p, prev = now - k, (n, r)
        if k > now and (best_n is None or k - now < best_n):
            best_n, nxt = k - now, (n, r)
        if earliest is None or k < key_ticks(earliest[0]):
            earliest = (n, r)
    if prev is None:   # client falls back to the last key iterated
        prev = keys[-1]
    if nxt is None:
        nxt = earliest
    return prev, nxt


def interpolate(keys, now):
    """WeatherChannel_UpdateKeys 0x1018a250: t = (now - prev) / (next - prev), day wrap."""
    prev, nxt = pick_keys(keys, now)
    if prev[0] == nxt[0] and prev[1] is nxt[1]:
        return prev, nxt, 0.0, prev[1]
    span = (key_ticks(nxt[0]) - key_ticks(prev[0])) % TICKS_PER_DAY
    el = (now - key_ticks(prev[0])) % TICKS_PER_DAY
    t = el / span if span else 0.0
    return prev, nxt, t, lerp_rec(nxt[1], prev[1], t)


def sun_moon(now):
    """Env_UpdateSunMoon 0x10187630 (flag bit0 clear): a = dayfrac * 2*pi.
    moon dir = (sin a, cos a, 0); sun dir = (sin(a+pi), cos(a+pi), 0). Game space is Y-down, so
    sun.y > 0 means the light points down. Day when sun.y >= 0 (06:00..18:00)."""
    a = now / TICKS_PER_DAY * 6.283
    moon = (math.sin(a), math.cos(a), 0.0)
    b = a + 3.1415
    if b > 3.1415:
        b -= 6.283
    sun = (math.sin(b), math.cos(b), 0.0)
    return sun, moon


def parse_time(s):
    if ":" in s:
        parts = [int(x) for x in s.split(":")]
        while len(parts) < 3:
            parts.append(0)
        h, m, sec = parts
        return (h * 3600 + m * 60 + sec) * 60 % TICKS_PER_DAY
    return int(round(float(s) * 60 * TICKS_PER_MIN)) % TICKS_PER_DAY

# ------------------------------------------------------------------ output


def lights(rec, now):
    """Light/ambient/fog for set 0 (zone meshes, +0x2C) and set 1 (actors, +0x0C)."""
    sun, moon = sun_moon(now)
    day = sun[1] >= 0.0
    out = {}
    for which, base in ((0, 0x2C), (1, 0x0C)):
        if rec.explicit_dirs:
            d = norm(rec.dir8(base + 4))
            col = rec.rgb(base)
            src = "explicit"
        elif day:
            d, col, src = sun, rec.rgb(base), "sun"
        else:
            d, col, src = moon, rec.rgb(base + 4), "moon"
        mult = rec.f(base + 0x18)
        end, start = rec.f(base + 0x10), rec.f(base + 0x14)
        if end <= start:          # Env_GetFog 0x10189030
            end = start + 1.0
        out[which] = dict(
            source=src, direction=d,
            diffuse=tuple(round(c / 255 * mult, 4) for c in col) + (1.0,),
            ambient_rgb=rec.rgb(base + 8),
            fog_rgb=rec.rgb(0x38),          # both sets use the +0x38 fog colour (CODE)
            fog_rgb_alt=rec.rgb(base + 0xC),
            fog_start=start, fog_end=end, mult=mult)
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    zone = int(argv[0])
    fid, recs = load_weather(zone)
    if "--list" in argv:
        print(f"zone {zone}: file {fid}, {len(recs)} Weather chunks")
        groups = {}
        for st, n, pl in recs:
            groups.setdefault("/".join(st), []).append(n)
        for g, ns in groups.items():
            print(f"  {g:28s} {' '.join(ns)}")
        return 0
    if len(argv) < 3:
        print(__doc__)
        return 1
    alt = "--alt" in argv
    block = argv[argv.index("--block") + 1] if "--block" in argv else "weat"
    w = argv[1]
    table = FOURCC_ALT if alt else FOURCC
    fourcc = table[int(w)] if w.isdigit() else w
    now = parse_time(argv[2])
    if not recs:
        print(f"zone {zone}: file {fid} has no Weather chunks. The client keeps its default record "
              "(fog grey 0x80, fog end 128, clear colour 0x808080; see section 9.6).")
        return 0
    used, keys = select_group(recs, fourcc, block)
    if not keys:
        print(f"no '{block}/{fourcc}' or '{block}/suny' group in zone {zone}")
        return 1
    if used != fourcc:
        print(f"warning: group {fourcc!r} missing, client falls back to {used!r}")
    prev, nxt, t, rec = interpolate(keys, now)
    hh, mm = divmod(now // TICKS_PER_MIN, 60)
    print(f"zone {zone} (file {fid})  weather {w} -> {block}/{used}  time {hh:02d}:{mm:02d}")
    print(f"  keys: prev {prev[0]}  next {nxt[0]}  t = {t:.4f}   (group has {len(keys)} keys:"
          f" {' '.join(sorted(n for n, _ in keys))})")
    if "--from" in argv:
        wf = argv[argv.index("--from") + 1]
        bt = float(argv[argv.index("--blend") + 1]) if "--blend" in argv else 0.5
        ff = table[int(wf)] if wf.isdigit() else wf
        used_f, keys_f = select_group(recs, ff, block)
        old = interpolate(keys_f, now)[3]
        rec = lerp_rec(rec, old, bt)
        print(f"  blended from {used_f} with weight {bt:.3f} on the new weather")
    print(f"  flags 0x{struct.unpack_from('<I', rec.b, 0)[0]:x} (bit0 explicit light directions = {rec.explicit_dirs})")
    L = lights(rec, now)
    for which, label in ((0, "zone meshes (set +0x2C, light index 0)"), (1, "actors (set +0x0C, light index 1)")):
        l = L[which]
        d = ", ".join(f"{c:+.3f}" for c in l["direction"])
        print(f"  {label}:")
        print(f"    light ({l['source']}) dir ({d})  diffuse {l['diffuse']}  (colour x {l['mult']:.2f})")
        print(f"    AMBIENT rgb {l['ambient_rgb']}")
        print(f"    fog colour rgb {l['fog_rgb']}  start {l['fog_start']:.1f}  end {l['fog_end']:.1f}"
              f"   (+{0x18 if which else 0x38:02x} copy: {l['fog_rgb_alt']})")
    print(f"  clear/horizon colour (+0x4C) rgb {rec.rgb(0x4C)}")
    grad = [(round(rec.f(0x8C + 4 * i), 3), rec.rgb(0x6C + 4 * i)) for i in range(8)]
    print("  sky gradient (+0x6C colours, +0x8C positions):")
    for pos, col in grad:
        print(f"    {pos:5.3f}  rgb {col}")
    print(f"  +0x50 {rec.f(0x50):.2f}  +0x54 {rec.f(0x54):.2f}  entity draw distance +0x58 {rec.f(0x58):.1f}"
          f"  +0x5C..5E {rec.u8(0x5C)} {rec.u8(0x5D)} {rec.u8(0x5E)}  +0x68 {rec.f(0x68):.1f}")
    if "--raw" in argv:
        for o in range(0, 0xAC, 16):
            seg = rec.b[o:o + 16]
            print(f"    +{o:02x}: {seg.hex(' ')}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
