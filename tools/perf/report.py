#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""report.py TAG... : a Markdown table per resolution from runs/TAG_NN[_RES].csv.

Steady state = zone-in + 8 s .. + 33 s (as measure.py). Pop-in = zone-in .. + 8 s, when the crowd's models load.
"""
import csv, glob, os, re, sys

S = os.environ.get("PERF_OUT", "/tmp/ffxi-perf")  # where measure.py wrote runs/


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))]


rows_out = []
for tag in sys.argv[1:]:
    for path in sorted(glob.glob(os.path.join(S, "runs", f"{tag}_*.csv"))):
        m = re.match(rf"{tag}_(\d+)(?:_(\d+x\d+|x))?\.csv$", os.path.basename(path))  # _x: crowdrun --also-args
        if not m:
            continue
        rows = list(csv.DictReader(open(path)))
        zin = next((float(r["t"]) for r in rows if int(r["actors_n"]) > 0), None)
        if zin is None:
            continue
        steady = [r for r in rows if zin + 8 <= float(r["t"]) < zin + 33]
        popin = [r for r in rows if zin <= float(r["t"]) < zin + 8]
        f = [float(r["frame"]) for r in steady]
        a = [float(r["actors"]) for r in steady]
        g = [float(r["gpu"]) for r in steady]
        pf = [float(r["frame"]) for r in popin]
        rows_out.append((("1600x900 + also-args" if m.group(2) == "x" else m.group(2)) or "1600x900", int(m.group(1)), tag, len(f) / 25.0, sum(int(r["actors_n"]) for r in steady) / max(len(steady), 1),
                         pct(f, .5), pct(f, .99), max(f), pct(a, .99), pct(g, .99), max(pf) if pf else 0, sum(1 for v in pf if v > 8.3),
                         sum(1 for v in f if v > 8.3)))

for res in sorted({r[0] for r in rows_out}, key=lambda s: (int(s.split("x")[0]), s)):
    print(f"\n{res}\n")
    print("| crowd | build | drawn | fps | frame p50 | frame p99 | frame max | actors p99 | GPU p99 | pop-in worst | pop-in > 8.3 ms | steady > 8.3 ms |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in sorted((r for r in rows_out if r[0] == res), key=lambda r: (r[1], r[2] != "before")):
        print(f"| {r[1]} | {r[2]} | {r[4]:.0f} | {r[3]:.0f} | {r[5]:.2f} | {r[6]:.2f} | {r[7]:.2f} | {r[8]:.2f} | {r[9]:.2f} | {r[10]:.1f} | {r[11]} | {r[12]} |")
