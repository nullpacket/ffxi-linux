#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""stacks.py [SINCE]: tally the map server's main-thread stacks (frames 2-6) at each watchdog kill (read-only)."""
import re, subprocess, sys
from collections import Counter

since = sys.argv[1] if len(sys.argv) > 1 else "72h"
out = subprocess.run(["docker", "logs", "--since", since, "server-map-1"], capture_output=True, text=True)
lines = (out.stdout + out.stderr).splitlines()
tally = Counter()
for i, l in enumerate(lines):
    if "Stack trace (main thread)" not in l:
        continue
    frames = []
    for s in lines[i + 1:i + 12]:
        m = re.match(r"\s+#(\d+)\s+\S+ in (\S+)(?: at /server/src/(\S+?):\d+)?", s)
        if m and int(m.group(1)) >= 2:
            frames.append(m.group(2) + (f" ({m.group(3)})" if m.group(3) else ""))
        if len(frames) == 4:
            break
    tally[" <- ".join(frames)] += 1
for k, v in tally.most_common(20):
    print(v, k)
