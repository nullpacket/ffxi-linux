#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Goal 13 (docs/performance.md): one frame-time measurement with Botone (charid 2; MEASURE=user,char,charid) at the test spot.

measure.py LABEL [--res WxH] [--after S] [--window S] [--spot zone,x,y,z,rot] [-- extra client args]
Writes LABEL.csv / LABEL.log / LABEL.png in runs/ and appends a row to results.tsv.
Env: XI_LSB_DB_CONTAINER, XI_LSB_DB_USER, XI_LSB_DB_PASS, XI_LSB_DB_NAME (LSB stock defaults).
"""
import csv, os, pathlib, subprocess, sys, time

S = os.environ.get("PERF_OUT", "/tmp/ffxi-perf")  # runs/, crowdlogs/, xdg/ and results.tsv go here
WT = str(pathlib.Path(__file__).resolve().parents[2])  # the repo (its build/xi-client, build/xi-headless)
# LSB's database, from the environment; the defaults are LSB's stock docker-compose values.
_E = os.environ.get
DB = ["docker", "exec", "-i", _E("XI_LSB_DB_CONTAINER", "server-database-1"), "mariadb", "-u" + _E("XI_LSB_DB_USER", "xiadmin"),
      "-p" + _E("XI_LSB_DB_PASS", "password"), "-h127.0.0.1", _E("XI_LSB_DB_NAME", "xidb"), "-N"]
SPOT = os.environ.get("SPOT", "230,17.5,2,-10.486,64")  # zone,x,y,z,rot: Southern San d'Oria plaza, facing the camera
# The measuring character: MEASURE=user,char,charid (default Botone).
USER, CHAR, CHARID = os.environ.get("MEASURE", "bot1,Botone,2").split(",")


def sql(q):
    return subprocess.run(DB, input=q, capture_output=True, text=True).stdout


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))]


def main():
    label = sys.argv[1]
    rest = sys.argv[2:]
    extra = []
    if "--" in rest:
        i = rest.index("--")
        rest, extra = rest[:i], rest[i + 1:]
    opts = dict(zip(rest[::2], rest[1::2]))
    res = opts.get("--res", "1600x900")
    after = float(opts.get("--after", "45"))
    window = float(opts.get("--window", "30"))
    spot = opts.get("--spot", SPOT).split(",")
    runs = os.path.join(S, "runs")
    os.makedirs(runs, exist_ok=True)
    for d in ("c", "d"):
        os.makedirs(os.path.join(S, "xdg", d), exist_ok=True)

    t0 = time.time()
    while time.time() - t0 < 180:
        busy = sql(f"select 1 from accounts_sessions where charid={CHARID}").strip()
        procs = [p for p in subprocess.run(["pgrep", "-x", "xi-client"], capture_output=True, text=True).stdout.split() + 
                 []
                 if os.path.realpath(f"/proc/{p}/exe").startswith((WT, S))]
        if not busy and not procs:
            break
        time.sleep(2)
    else:
        sys.exit(f"{CHAR} still logged in")
    # Goal 43: after the server drops a session, leave a retail-like gap before logging in again (RELOG_GAP seconds).
    time.sleep(float(os.environ.get("RELOG_GAP", 10)))
    z, x, y, zz, rot = spot
    if not os.environ.get("PERF_NO_PLACE"):  # PERF_NO_PLACE=1: the character is already at the spot (no DB write)
        sql(f"update chars set pos_zone={z}, pos_prevzone={z}, pos_x={x}, pos_y={y}, pos_z={zz}, pos_rot={rot} where charid={CHARID}")

    csvp, logp, png = (os.path.join(runs, label + e) for e in (".csv", ".log", ".png"))
    for old in (csvp, png):
        if os.path.exists(old):
            os.remove(old)
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(S, "xdg", "c"), XDG_DATA_HOME=os.path.join(S, "xdg", "d"), XI_NO_FOCUS="1",
               SDL_AUDIO_DRIVER="dummy", XI_DISPLAY=os.environ.get("XI_DISPLAY", "secondary"), XI_FRAME_TRACE="10", XI_PRESENT=os.environ.get("XI_PRESENT", "immediate"), XI_FRAME_TRACE_CSV=csvp)
    binary = os.path.abspath(opts.get("--bin", os.path.join(WT, "build/xi-client")))
    # The game data: FFXI_ROOT, else our imported copy (xi-setup import). Named explicitly because XDG_DATA_HOME is
    # pointed at the scratch dir above, where the client would otherwise look for it.
    env["FFXI_ROOT"] = os.environ.get("FFXI_ROOT") or os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "ffxi-linux", "game")
    cmd = [binary, "--compress-dat", os.path.expanduser("~/git/server/res/compress.dat"), "--host", "127.0.0.1", "--pass", "botpass",
           "--user", USER, "--char", CHAR, "--autoconnect", "--res", res,
           "--after", str(after), "--screenshot", png] + extra
    with open(logp, "w") as f:
        rc = subprocess.run(["timeout", str(int(after + 90)), *cmd], cwd=WT, env=env, stdout=f, stderr=subprocess.STDOUT).returncode

    rows = list(csv.DictReader(open(csvp)))
    zin = next((float(r["t"]) for r in rows if int(r["actors_n"]) > 0), None)
    if zin is None:
        sys.exit(f"never in zone (rc {rc})")
    sel = [r for r in rows if zin + 8 <= float(r["t"]) < zin + 8 + window]
    cols = ["frame", "gpu", "game", "ui", "actors", "effects", "wait", "record", "submit"]
    n = len(sel)
    fps = n / window
    actors = sum(int(r["actors_n"]) for r in sel) / max(n, 1)
    line = [label, res, f"{fps:.0f}", f"{actors:.1f}"]
    print(f"{label} {res} rc {rc}: {n} frames, {fps:.0f} fps, {actors:.1f} actors")
    for c in cols:
        v = [float(r[c]) for r in sel]
        p50, p95, p99, mx = pct(v, .5), pct(v, .95), pct(v, .99), max(v)
        print(f"  {c:8s} p50 {p50:6.2f}  p95 {p95:6.2f}  p99 {p99:6.2f}  max {mx:6.2f}")
        line += [f"{p50:.2f}", f"{p95:.2f}", f"{p99:.2f}"]
    with open(os.path.join(S, "results.tsv"), "a") as f:
        f.write("\t".join(line) + "\n")


if __name__ == "__main__":
    main()
