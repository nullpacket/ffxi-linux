#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render N watermarked 4K wallpapers for every non-instanced zone.

Camera spots come from LSB's zone data (mob/NPC positions and zone lines are known-walkable). Each zone gets a few
dozen low-res candidate shots rendered with --nosky, so sky and out-of-map void both clear to magenta; magenta below
the horizon is void. Candidates are scored on void, openness, brightness and detail, the best N (from different spots)
are re-rendered at 4K with the sky on, then cropped to 16:9, debanded and stamped with the logo.

  tools/zone_wallpapers.py [--per-zone 5] [--out DIR] [--server ~/git/server] [--only ZONE ...]
"""
import argparse
import math
import os
import random
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from PIL import Image, ImageChops, ImageFilter, ImageStat

ROOT = Path(__file__).resolve().parent.parent
VIEWER = ROOT / "build" / "xi-viewer"
LOGO = ROOT / "docs" / "branding" / "logo.svg"

# Untyped zones in LSB data are battlefield arenas, ships and GM areas; these few are open zones anyway.
UNTYPED_OPEN = {"reisenjima", "mount_kamihr", "leafallia", "silver_knife"}
TIMES = ["06:30", "08:00", "10:30", "13:00", "15:30", "17:00", "17:45"]
CAND_W, CAND_H = 480, 270
NEAR = {"city": 15, "outdoors": 16, "dungeon": 8, "dynamis": 15}  # yalms: geometry closer than this "blocks" the shot
FAR = {"city": 40, "outdoors": 70, "dungeon": 22, "dynamis": 40}   # yalms: a good shot has real depth past this
TAN_HALF_FOV = math.tan(math.radians(30))
AT_RE = re.compile(r"^\s*(?:at|from):\s*\[\s*(-?[\d.]+),\s*(-?[\d.]+),\s*(-?[\d.]+)")


def zone_ids(server):
    ids = {}
    for line in (server / "data/enums/zone.yaml").read_text().splitlines():
        m = re.match(r"^\s+([a-z0-9_]+):\s+(\d+)\s*$", line)
        if m:
            ids[m.group(1)] = int(m.group(2))
    return ids


def zone_kind(zdir):
    m = re.search(r"^type:\s*\[([^\]]*)\]", (zdir / "zone.yaml").read_text(), re.M)
    return m.group(1).strip() if m else ""


def zone_points(zdir):
    pts = set()
    for f in ("mobs.yaml", "npcs.yaml", "zone.yaml"):
        p = zdir / f
        if not p.exists():
            continue
        for line in p.read_text().splitlines():
            m = AT_RE.match(line)
            if m:
                x, y, z = (float(v) for v in m.groups())
                if max(abs(x), abs(y), abs(z)) > 1.5:  # 1,1,1 / 0,0,0 are "not placed" markers
                    pts.add((round(x, 1), round(y, 1), round(z, 1)))
    return list(pts)


def farthest(points, n, rnd):
    if len(points) <= n:
        return points[:]
    chosen = [rnd.choice(points)]
    d = [math.dist((p[0], p[2]), (chosen[0][0], chosen[0][2])) for p in points]
    while len(chosen) < n:
        i = max(range(len(points)), key=d.__getitem__)
        chosen.append(points[i])
        d = [min(d[j], math.dist((points[j][0], points[j][2]), (points[i][0], points[i][2]))) for j in range(len(points))]
    return chosen


def candidates(points, kind, rnd, spots=20):
    cx = sum(p[0] for p in points) / len(points)
    cz = sum(p[2] for p in points) / len(points)
    elev, pitch = {
        "city":     ((5, 14), (-12, 2)),
        "outdoors": ((8, 30), (-10, 4)),
        "dungeon":  ((1.8, 3.5), (-6, 6)),
        "dynamis":  ((6, 18), (-10, 3)),
    }.get(kind, ((5, 16), (-8, 3)))
    out = []
    for si, (x, y, z) in enumerate(farthest(points, spots, rnd)):
        toward = math.degrees(math.atan2(cx - x, cz - z))
        for yaw in (toward + rnd.uniform(-35, 35), rnd.uniform(0, 360), rnd.uniform(0, 360), toward + 180 + rnd.uniform(-40, 40)):
            e = rnd.uniform(*elev)
            p = rnd.uniform(*pitch)
            out.append({"spot": si, "cam": f"{x:.2f},{y - e:.2f},{z:.2f},{yaw % 360:.1f},{p:.1f}", "pitch": p,
                        "time": rnd.choice(TIMES)})
    return out


def run_viewer(zone_id, shots, size, nosky, env, extra=()):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        for s in shots:
            f.write(f"{s['cam']} {s['time']} {s['out']}\n")
        listfile = f.name
    args = [str(VIEWER), "--size", size, "--shots", listfile] + (["--nosky"] if nosky else []) + list(extra) + ["zone", str(zone_id)]
    try:
        subprocess.run(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    except subprocess.TimeoutExpired:
        pass
    os.unlink(listfile)


def mask(im, rgb):
    bands = []
    for band, want in zip(im.split(), rgb):
        bands.append(band.point(lambda v: 255 if v > 235 else 0) if want else band.point(lambda v: 255 if v < 25 else 0))
    return ImageChops.multiply(ImageChops.multiply(bands[0], bands[1]), bands[2])


def probe_fractions(path):
    """(geometry closer than the --nearprobe distance, geometry beyond it) as shares of the frame: beyond renders pure
    green, sky/void magenta."""
    try:
        im = Image.open(path).convert("RGB")
    except Exception:
        return 1.0, 0.0
    beyond = ImageStat.Stat(mask(im, (0, 1, 0))).mean[0] / 255
    sky = ImageStat.Stat(mask(im, (1, 0, 1))).mean[0] / 255
    return 1 - beyond - sky, beyond


def score(path, pitch, kind, near, far):
    try:
        im = Image.open(path).convert("RGB")
    except Exception:
        return None
    mag = mask(im, (1, 0, 1))
    W, H = im.size
    hz = int(min(max(0.5 + math.tan(math.radians(pitch)) / TAN_HALF_FOV * 0.5, 0.0), 1.0) * H)
    below = ImageStat.Stat(mag.crop((0, min(hz + 6, H - 1), W, H))).mean[0] / 255 if hz + 6 < H else 0.0
    above = ImageStat.Stat(mag.crop((0, 0, W, max(hz, 1)))).mean[0] / 255 if hz > 0 else 0.0
    total = ImageStat.Stat(mag).mean[0] / 255
    geo = ImageChops.invert(mag)
    if ImageStat.Stat(geo).mean[0] < 10:
        return None
    lum = im.convert("L")
    bright = ImageStat.Stat(lum, geo).mean[0]
    detail = ImageStat.Stat(lum.filter(ImageFilter.FIND_EDGES), geo).mean[0]
    contrast = ImageStat.Stat(lum, geo).stddev[0]
    if kind == "dungeon":
        if total > 0.12 or bright < 28 or near > 0.45 or detail < 9 or far < 0.12:
            return None
        return detail * 2 + contrast * 0.5 + bright * 0.15 - total * 150 - near * 30 + far * 40
    if below > 0.025 or bright < 30 or above < 0.04 or near > 0.25 or far < 0.2:
        return None  # void, too dark, sky blocked, something right in front of the lens, or no depth
    openness = 1 - abs(above - 0.4) * 1.5
    return detail * 2 + contrast * 0.5 + bright * 0.1 + openness * 12 - below * 400 - near * 40 + far * 30


def make_mark(tmp):
    png = Path(tmp) / "logo.png"
    subprocess.run(["rsvg-convert", "-w", "960", str(LOGO), "-o", str(png)], check=True)
    logo = Image.open(png).convert("RGBA")
    pad = 40
    mark = Image.new("RGBA", (logo.width + 2 * pad, logo.height + 2 * pad), (0, 0, 0, 0))
    shadow = Image.new("RGBA", mark.size, (0, 0, 0, 0))
    a = logo.split()[3].point(lambda v: int(v * 0.75))
    shadow.paste((0, 0, 0, 255), (pad, pad + 4), a)
    shadow = shadow.filter(ImageFilter.GaussianBlur(14))
    mark = Image.alpha_composite(shadow, mark)
    mark.alpha_composite(logo, (pad, pad))
    out = Path(tmp) / "mark.png"
    mark.save(out)
    return str(out)


def finish(raw, out, mark_path):
    im = Image.open(raw).convert("RGB")
    w, h = im.size
    cw = min(w, h * 16 // 9)
    ch = cw * 9 // 16
    im = im.crop(((w - cw) // 2, (h - ch) // 2, (w - cw) // 2 + cw, (h - ch) // 2 + ch)).resize((3840, 2160), Image.LANCZOS)
    noise = Image.effect_noise((3840, 2160), 3).convert("RGB")
    im = ImageChops.add(im, noise, 1.0, -128)
    mark = Image.open(mark_path)
    im.paste(mark, (3840 - mark.width - 50, 2160 - mark.height - 40), mark)
    im.save(out, quality=92, subsampling=0, optimize=True)
    os.unlink(raw)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-zone", type=int, default=5)
    ap.add_argument("--out", default=str(Path.home() / "Pictures/Wallpapers/ffxi-linux"))
    ap.add_argument("--server", default=str(Path.home() / "git/server"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()

    server = Path(a.server)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.setdefault("FFXI_ROOT", str(Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") / "ffxi-linux/game"))  # xi-setup import
    env.setdefault("XI_DISPLAY", "secondary")
    env["XI_NO_FOCUS"] = "1"

    ids = zone_ids(server)
    zones = []
    for zdir in sorted((server / "data/zones").iterdir()):
        if not (zdir / "zone.yaml").exists() or zdir.name not in ids:
            continue
        kind = zone_kind(zdir)
        if kind == "instanced" or (kind == "" and zdir.name not in UNTYPED_OPEN):
            continue
        if a.only and zdir.name not in a.only:
            continue
        zones.append((zdir, kind))
    print(f"{len(zones)} zones", flush=True)

    tmp = tempfile.mkdtemp(prefix="xiwall-")
    mark = make_mark(tmp)
    pool = ProcessPoolExecutor(max_workers=max(2, (os.cpu_count() or 4) - 4))
    pending = []
    skipped = []
    for n, (zdir, kind) in enumerate(zones, 1):
        name, zid = zdir.name, ids[zdir.name]
        if all((out / f"{name}-{i}.jpg").exists() for i in range(1, a.per_zone + 1)):
            continue
        rnd = random.Random(f"{a.seed}-{name}")
        pts = zone_points(zdir)
        if len(pts) < 2:
            skipped.append((name, "no positions"))
            continue
        cands = candidates(pts, kind, rnd)
        for i, c in enumerate(cands):
            c["out"] = f"{tmp}/{name}-c{i}.png"
        run_viewer(zid, cands, f"{CAND_W}x{CAND_H}", True, env)
        nearp = [dict(c, out=c["out"].replace(".png", "-near.png")) for c in cands]
        farp = [dict(c, out=c["out"].replace(".png", "-far.png")) for c in cands]
        run_viewer(zid, nearp, f"{CAND_W}x{CAND_H}", True, env, ["--nearprobe", str(NEAR.get(kind, 10))])
        run_viewer(zid, farp, f"{CAND_W}x{CAND_H}", True, env, ["--nearprobe", str(FAR.get(kind, 40))])
        scored = []
        for c, n_, f_ in zip(cands, nearp, farp):
            s = score(c["out"], c["pitch"], kind, probe_fractions(n_["out"])[0], probe_fractions(f_["out"])[1])
            for f in (c["out"], n_["out"], f_["out"]):
                if os.path.exists(f):
                    os.unlink(f)
            if s is not None:
                scored.append((s, c))
        scored.sort(key=lambda t: -t[0])
        picks, spots = [], set()
        for s, c in scored:  # one shot per spot first, then fill from the rest
            if c["spot"] not in spots:
                picks.append(c)
                spots.add(c["spot"])
            if len(picks) == a.per_zone:
                break
        for s, c in scored:
            if len(picks) == a.per_zone:
                break
            if c not in picks:
                picks.append(c)
        if not picks:
            skipped.append((name, f"0/{len(cands)} candidates usable"))
            print(f"[{n}/{len(zones)}] {name}: skipped", flush=True)
            continue
        for i, c in enumerate(picks, 1):
            c["out"] = f"{tmp}/{name}-{i}.png"
        run_viewer(zid, picks, "3840x2160", False, env)
        for i, c in enumerate(picks, 1):
            if os.path.exists(c["out"]):
                pending.append(pool.submit(finish, c["out"], str(out / f"{name}-{i}.jpg"), mark))
        print(f"[{n}/{len(zones)}] {name} ({kind or 'open'}): {len(picks)} shots from {len(scored)}/{len(cands)} usable", flush=True)

    for f in pending:
        f.result()
    pool.shutdown()
    print(f"done: {len(pending)} images in {out}")
    for name, why in skipped:
        print(f"skipped {name}: {why}")


if __name__ == "__main__":
    sys.exit(main())
