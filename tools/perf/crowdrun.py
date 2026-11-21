#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Goal 13 (docs/performance.md): grow the crowd in steps and measure Botone at each size.

crowdrun.py PREFIX STEP1,STEP2,... [--res-also WxH] [--wander R]
e.g. crowdrun.py cmp 0,20,40,60 --bins before=old/xi-client,after=build/xi-client
Every crowd client runs until a common end time, then logs out by itself.
"""
import os, pathlib, subprocess, sys, time

S = os.environ.get("PERF_OUT", "/tmp/ffxi-perf")  # runs/, crowdlogs/, xdg/ and results.tsv go here
WT = str(pathlib.Path(__file__).resolve().parents[2])  # the repo (its build/xi-client, build/xi-headless)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import crowd  # noqa: E402

prefix = sys.argv[1]
steps = [int(v) for v in sys.argv[2].split(",")]
opts = dict(zip(sys.argv[3::2], sys.argv[4::2]))
also = opts.get("--res-also")
# --also-args "CLIENT ARGS": a second measurement per binary with extra client arguments (label suffix _x), e.g.
# "--bg-res 3840x2160" for a 4K scene in a normal window.
also_args = opts.get("--also-args")
wander = opts.get("--wander", "8")
# Binaries to compare at each crowd size: PREFIX=path pairs in --bins (default: this build as PREFIX).
BINS = [(t, os.path.abspath(b)) for t, b in (v.split("=", 1) for v in opts["--bins"].split(","))] if "--bins" in opts else [(prefix, os.path.join(WT, "build/xi-client"))]
per_measure = (110 + (110 if also else 0) + (110 if also_args else 0)) * len(BINS)
end = time.time() + 60 + len(steps) * (per_measure + 60)
members = sorted(crowd.crowd(), key=lambda r: r[2])
logs = os.path.join(S, "crowdlogs")
os.makedirs(logs, exist_ok=True)
procs = []


def stats():
    out = subprocess.run(["docker", "stats", "--no-stream", "--format", "{{.Name}} {{.CPUPerc}} {{.MemUsage}}"], capture_output=True, text=True).stdout
    return " | ".join(l for l in out.splitlines() if "map" in l or "world" in l or "database" in l or "connect" in l)


def online():
    return int(crowd.sql("select count(*) from accounts_sessions s join chars c using(charid) join accounts a on a.id=c.accid where a.login like 'test_%'"))


# Start clean: no crowd or Botone session left from an earlier run, every crowd character at the spot.
t0 = time.time()
while (online() or crowd.sql(f"select 1 from accounts_sessions where charid={os.environ.get('MEASURE', 'bot1,Botone,2').split(',')[2]}").strip()) and time.time() - t0 < 240:
    time.sleep(5)
time.sleep(float(os.environ.get("RELOG_GAP", 10)))  # goal 43: a retail-like gap after the server drops the sessions
if not os.environ.get("PERF_NO_PLACE"):  # PERF_NO_PLACE=1: the crowd already stands at the spot (no DB write)
    crowd.place(230, 17.5, 2.0, -2.5, 6.0)

for size in steps:
    while len(procs) < size:
        accid, login, charid = members[len(procs)]
        secs = int(end - time.time())
        f = open(os.path.join(logs, f"{login}.log"), "w")
        procs.append(subprocess.Popen([os.path.join(WT, "build/xi-headless"), "--user", login, "--pass", crowd.PASSWORD, "--char", login.capitalize(),
                                       "--seconds", str(secs), "--wander", wander, "--quiet"], stdout=f, stderr=subprocess.STDOUT))
        time.sleep(1.5)
    t0 = time.time()
    while online() < size and time.time() - t0 < 90:
        time.sleep(3)
    time.sleep(10)
    print(f"crowd {size}: {online()} online; {stats()}", flush=True)
    for tag, binary in BINS:
        subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "measure.py"), f"{tag}_{size:02d}", "--bin", binary, "--after", "40", "--window", "25"])
        if also:
            subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "measure.py"), f"{tag}_{size:02d}_{also}", "--bin", binary, "--res", also, "--after", "40", "--window", "25"])
        if also_args:
            subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "measure.py"), f"{tag}_{size:02d}_x", "--bin", binary, "--after", "40", "--window", "25", "--", *also_args.split()])
    print(f"after crowd {size}: {stats()}", flush=True)

print("measurements done; logging the crowd out", flush=True)
for p in procs:
    p.terminate()  # xi-headless logs out cleanly on SIGTERM
    time.sleep(0.2)
for p in procs:
    p.wait()
t0 = time.time()
while online() and time.time() - t0 < 120:
    time.sleep(3)
print(f"crowd online after logout: {online()}; {stats()}", flush=True)
