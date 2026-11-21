#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""analyze.py PACECAP.csv: per segment between MARK lines, outgoing datagrams/s, 0x015/s, sub-packets per datagram,
datagram interval (median, p10, p90) and the other ids' rates."""
import csv, statistics, sys
from collections import Counter

rows = list(csv.DictReader(open(sys.argv[1])))
segs, cur, name = [], [], "start"
for r in rows:
    if r["dir"] == "MARK":
        segs.append((name, cur)); cur, name = [], r["id"]
    elif r["dir"] == "OUT":
        cur.append(r)
segs.append((name, cur))
for name, rs in segs:
    if len(rs) < 2:
        continue
    t = [float(r["ms"]) for r in rs]
    span = (t[-1] - t[0]) / 1000.0 or 1.0
    starts = [float(r["ms"]) for r in rs if r["chunk_first"] == "1"]
    gaps = sorted(b - a for a, b in zip(starts, starts[1:]))
    ids = Counter(r["id"] for r in rs)
    q = lambda p: gaps[min(len(gaps) - 1, int(p * len(gaps)))] if gaps else 0
    print(f"{name:10s} {span:6.1f}s  datagrams {len(starts)/span:5.2f}/s  0x015 {ids.get('0x015',0)/span:5.2f}/s  "
          f"per datagram {len(rs)/max(len(starts),1):4.2f}  gap ms median {q(.5):6.1f} p10 {q(.1):6.1f} p90 {q(.9):6.1f}  "
          + " ".join(f"{k}:{v}" for k, v in sorted(ids.items()) if k != "0x015"))
