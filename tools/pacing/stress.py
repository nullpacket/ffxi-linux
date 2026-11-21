#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""stress.py MODE MINUTES [N]: N headless crowd bots (testthirty.., crowdpass) cycle login -> wander STAY s -> leave,
for MINUTES, then counts map-server watchdog kills in that window (docker logs, read-only).
MODE old:    leave by SIGKILL (no logout, as test runs ended by `timeout` do) and log in again at once.
MODE rapid:  clean logout (0x0E7), then log in again at once.
MODE retail: clean logout, then wait until the server has dropped the session (accounts_sessions) plus GAP s.
Env: STAY (default 20), GAP (default 5), EXE (default build/xi-headless);
XI_LSB_DB_CONTAINER, XI_LSB_DB_USER, XI_LSB_DB_PASS, XI_LSB_DB_NAME (LSB stock defaults)."""
import os, random, subprocess, sys, threading, time, datetime

W = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # the repo root
EXE = os.environ.get("EXE", os.path.join(W, "build", "xi-headless"))
# LSB's database, from the environment; the defaults are LSB's stock docker-compose values.
_E = os.environ.get
DB = ["docker", "exec", _E("XI_LSB_DB_CONTAINER", "server-database-1"), "mariadb", "-u" + _E("XI_LSB_DB_USER", "xiadmin"),
      "-p" + _E("XI_LSB_DB_PASS", "password"), "-h127.0.0.1", _E("XI_LSB_DB_NAME", "xidb"), "-N", "-e"]
NAMES = ["thirty", "thirtyone", "thirtytwo", "thirtythree", "thirtyfour", "thirtyfive", "thirtysix", "thirtyseven",
         "thirtyeight", "thirtynine", "forty", "fortyone"]
mode, minutes = sys.argv[1], float(sys.argv[2])
n = int(sys.argv[3]) if len(sys.argv) > 3 else 6
n = min(n, len([b for b in os.environ.get("BOTS", "").split(",") if b]) or n)
stay, gap = float(os.environ.get("STAY", 20)), float(os.environ.get("GAP", 5))
S = os.environ.get("OUT", "/tmp/xi-stress")  # logs go to OUT/stress/
os.makedirs(os.path.join(S, "stress"), exist_ok=True)
start = datetime.datetime.now(datetime.timezone.utc)
deadline = time.time() + minutes * 60
stats = {"logins": 0, "fails": 0}
lock = threading.Lock()

def session_open(char):
    q = "select count(*) from accounts_sessions s join chars c on c.charid=s.charid where c.charname='%s'" % char
    return subprocess.run(DB + [q], capture_output=True, text=True).stdout.strip() != "0"

BOTS = [b.split(":") for b in os.environ.get("BOTS", "").split(",") if b]  # user:pass:Char, overrides the crowd


def bot(i):
    user, pw, char = BOTS[i] if BOTS else ("test" + NAMES[i], "crowdpass", "Test" + NAMES[i])
    log = open(os.path.join(S, "stress", "%s_%s.log" % (mode, user)), "w")
    time.sleep(i * 1.5)
    while time.time() < deadline:
        if mode == "retail":
            for _ in range(90):
                if not session_open(char):
                    break
                time.sleep(1)
            time.sleep(gap)
        args = [EXE, "--host", "127.0.0.1", "--user", user, "--pass", pw, "--char", char, "--quiet",
                "--seconds", str(int(stay + random.uniform(0, 5)))] + ([] if BOTS else ["--wander", "8"])
        p = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        if mode == "old":
            time.sleep(stay + random.uniform(0, 5))
            p.kill()
            rc = p.wait()
        else:
            try:
                rc = p.wait(timeout=stay + 90)
            except subprocess.TimeoutExpired:
                p.kill(); rc = p.wait()
        with lock:
            stats["logins"] += 1
            if rc not in (0, -9):
                stats["fails"] += 1

threads = [threading.Thread(target=bot, args=(i,)) for i in range(n)]
for t in threads:
    t.start()
for t in threads:
    t.join()
since = start.strftime("%Y-%m-%dT%H:%M:%SZ")
logs = subprocess.run(["docker", "logs", "--since", since, "server-map-1"], capture_output=True, text=True)
kills = (logs.stdout + logs.stderr).count("INACTIVITY WATCHDOG HAS TRIGGERED")
print(f"mode {mode}: {minutes} min, {n} bots, {stats['logins']} logins ({stats['fails']} failed), watchdog kills: {kills}")
