#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where the client stands LSB's NPCs: feeds each zone's npcs.yaml to `xi-dat npcground` and summarises.

    python3 tools/npc_ground_check.py [--server ~/git/server] [--xi-dat build/xi-dat] [--show N] [--list KINDS] [ZONE_DIR ...]

For every NPC that is shown (not hidden / cutscene-only) and has a model, `xi-dat npcground` runs the game's actor
ground probe (client::actorGround: the highest floor from 2 above to 40 below the server height): "ground" (stood on
that floor), "keep" (GroundFlag, `moving` bit 15: hovering moogles, markers, wells keep the server height, as in the
game) or "miss" (no floor in reach). Prints per zone counts and the actors that moved the most. Read-only: nothing is
written to the server checkout.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

import yaml


def npcs(path):
    with open(path, encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    for npc_id, n in (data.get("npcs") or {}).items():
        if not isinstance(n, dict) or "at" not in n:
            continue
        if n.get("status", "normal") in ("disappear", "invisible", "cutscene_only"):
            continue
        render = n.get("render") or {}
        look = render.get("look") or {}
        if look.get("type") in ("door", "elevator", "ship"):
            continue  # look kinds 2..4 keep their own height in the client
        if look.get("type") == "standard" and look.get("model") in (None, 0, 50):
            continue  # model 50: the invisible marker actors (warp01, DIRECTOR)
        x, y, z = (float(v) for v in n["at"][:3])
        keep = 1 if int(render.get("moving", 0)) & 0x8000 else 0
        yield int(npc_id), x, y, z, keep, n.get("display_name", "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default=os.path.expanduser("~/git/server"))
    ap.add_argument("--xi-dat", default="build/xi-dat")
    ap.add_argument("--show", type=int, default=20, help="actors listed in each of the 'moved most' lists")
    ap.add_argument("--list", default="", help="also print every actor whose result is one of these (comma list: keep, ground, miss)")
    ap.add_argument("zones", nargs="*", help="zone folders under data/zones (default: all)")
    a = ap.parse_args()
    root = Path(a.server) / "data" / "zones"
    dirs = [root / z for z in a.zones] if a.zones else sorted(p for p in root.iterdir() if (p / "npcs.yaml").is_file())
    totals = {}
    moved = []
    for d in dirs:
        rows = list(npcs(d / "npcs.yaml"))
        if not rows:
            continue
        zone = (rows[0][0] >> 12) & 0xFFF
        text = "".join(f"{i} {x} {y} {z} {k} {name}\n" for i, x, y, z, k, name in rows)
        r = subprocess.run([a.xi_dat, "npcground", str(zone)], input=text, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"{d.name}: xi-dat failed: {r.stderr.strip()[:200]}", file=sys.stderr)
            continue
        for line in r.stdout.splitlines():
            p = line.split()
            if len(p) < 6 or p[2] != "y":
                if line.startswith("zone "):
                    print(f"{d.name}: {line}")
                continue
            what, y, drawn = p[1], float(p[3]), float(p[5]) if p[4] == "drawn" else float(p[3])
            totals[what] = totals.get(what, 0) + 1
            if what in a.list.split(","):
                print(f"  {d.name}: {line}")
            if what == "ground":
                moved.append((drawn - y, d.name, line))  # game y down: > 0 lowered, < 0 raised
    print("all zones:", ", ".join(f"{k} {v}" for k, v in sorted(totals.items())))
    moved.sort()
    print(f"raised the most (sunk into the floor at the server height):")
    for dy, zone, line in moved[: a.show]:
        print(f"  {-dy:7.3f} {zone}: {line}")
    print(f"lowered the most (floating at the server height):")
    for dy, zone, line in reversed(moved[-a.show :]):
        print(f"  {dy:7.3f} {zone}: {line}")
    big = sum(1 for dy, _, _ in moved if abs(dy) > 0.3)
    print(f"{big} of {len(moved)} grounded actors moved more than 0.3")


if __name__ == "__main__":
    main()
