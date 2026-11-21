#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""spikes.py RUN [THRESHOLD_MS]: frames over the threshold, with their stage split, and a count over time."""
import csv, os, sys
S = os.environ.get("PERF_OUT", "/tmp/ffxi-perf")  # where measure.py wrote runs/
rows = list(csv.DictReader(open(os.path.join(S, "runs", sys.argv[1] + ".csv"))))
th = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
zin = next(float(r["t"]) for r in rows if int(r["actors_n"]) > 0)
over = [r for r in rows if float(r["frame"]) > th and float(r["t"]) > zin + 1]
print(f"{len(over)} frames over {th} ms after zone-in (+1 s), of {sum(1 for r in rows if float(r['t']) > zin + 1)}")
for r in over[:40]:
    print(f"t {float(r['t']) - zin:7.2f} " + " ".join(f"{k} {float(r[k]):5.2f}" for k in ("frame", "game", "ui", "actors", "effects", "wait", "record", "submit", "gpu")) + f" n {r['actors_n']}")
