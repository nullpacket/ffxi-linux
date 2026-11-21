#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""kills.py [SINCE]: for each map-server watchdog kill in `docker logs`, the last meaningful log lines before it
(read-only). Prints a tally of the triggering event kinds."""
import re, subprocess, sys
from collections import Counter

since = sys.argv[1] if len(sys.argv) > 1 else "72h"
out = subprocess.run(["docker", "logs", "--since", since, "server-map-1"], capture_output=True, text=True)
lines = (out.stdout + out.stderr).splitlines()
noise = re.compile(r"Garbage Collected|Current State Top|Prepared statements|playtime saving|tick\.\.\.|application\.cpp|^\s*$|=====")
kinds = Counter()
for i, l in enumerate(lines):
    if "[critical] !!! INACTIVITY WATCHDOG HAS TRIGGERED" not in l:
        continue
    ts = l[:22]
    ctx = []
    for j in range(i - 1, max(0, i - 400), -1):
        s = lines[j]
        if "[critical]" in s or noise.search(s):
            continue
        if re.match(r"^\[\d", s):
            ctx.append(s[:22] + " " + re.sub(r"^\[[^]]*\]\[map\]\[\w+\] ", "", s[22:])[:120])
        if len(ctx) == 4:
            break
    last = ctx[0] if ctx else ""
    kind = ("leave zone" if "DecreaseZoneCounter" in last else "logout" if "Closing pending" in last or "key counter" in last
            else "login" if "logging in" in last or "IncreaseZoneCounter" in last else "other")
    kinds[kind] += 1
    print(ts, "|", " || ".join(reversed(ctx)))
print(dict(kinds))
