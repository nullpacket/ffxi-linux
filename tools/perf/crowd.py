#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Goal 13 speed-test crowd (docs/performance.md): accounts testone..testN (password crowdpass), characters Testone..TestN.

crowd.py create N       accounts + characters cloned from Bottwo (charid 3), varied race / face / size / gear looks
crowd.py place ZONE X Y Z [R]   put every crowd character at the spot (scattered within R yalms), logged out
crowd.py delete         remove every test<number> account, its characters and every charid-keyed row
crowd.py list
Env: XI_LSB_DB_CONTAINER, XI_LSB_DB_USER, XI_LSB_DB_PASS, XI_LSB_DB_NAME (LSB stock defaults).
"""
import math, os, random, subprocess, sys

# LSB's database, from the environment; the defaults are LSB's stock docker-compose values.
_E = os.environ.get
DB = ["docker", "exec", "-i", _E("XI_LSB_DB_CONTAINER", "server-database-1"), "mariadb", "-u" + _E("XI_LSB_DB_USER", "xiadmin"),
      "-p" + _E("XI_LSB_DB_PASS", "password"), "-h127.0.0.1", _E("XI_LSB_DB_NAME", "xidb"), "-N"]
TEMPLATE = 3  # Bottwo
PASSWORD = "crowdpass"
ONES = ["", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
        "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"]
TENS = ["", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"]
# Rows cloned from the template; the rest (sessions, equipment, inventory, effects, recasts, mail, fishing) start empty.
CLONE = ["chars", "char_chocobos", "char_exp", "char_flags", "char_history", "char_jobs", "char_job_points", "char_look", "char_merit",
         "char_monstrosity", "char_pet", "char_points", "char_profile", "char_skills", "char_spells", "char_stats", "char_storage",
         "char_style", "char_unlocks", "char_vars"]


def word(n):
    return ONES[n] if n < 20 else TENS[n // 10] + ONES[n % 10]


def sql(q):
    r = subprocess.run(DB, input=q, capture_output=True, text=True, errors="replace")
    if r.returncode:
        sys.exit(r.stderr)
    return r.stdout


def columns(table):
    return sql(f"select column_name from information_schema.columns where table_schema='xidb' and table_name='{table}' order by ordinal_position").split()


def crowd():
    """(accid, login, charid) of every existing crowd account."""
    out = []
    for line in sql("select a.id, a.login, ifnull(c.charid, 0) from accounts a left join chars c on c.accid = a.id where a.login like 'test%' and a.login <> 'test'").splitlines():
        accid, login, charid = line.split("\t")
        if any(login == "test" + word(n) for n in range(1, 100)):
            out.append((int(accid), login, int(charid)))
    return out


def create(n):
    have = {login for _, login, _ in crowd()}
    rng = random.Random(13)
    looks = {}
    for slot, col in ((16, "head"), (32, "body"), (64, "hands"), (128, "legs"), (256, "feet"), (1, "main"), (2, "sub")):
        looks[col] = sorted({int(v) & 0x0FFF for v in sql(f"select MId from item_equipment where slot={slot} and MId>0").split()} - {0})
    acct = sql(f"select content_ids, expansions, features from accounts where id=(select accid from chars where charid={TEMPLATE})").split()
    cols = {t: columns(t) for t in CLONE}
    for i in range(1, n + 1):
        login, name = "test" + word(i), ("test" + word(i)).capitalize()
        if login in have:
            continue
        subprocess.run([sys.executable, os.path.expanduser("~/git/lsb-devctl/lsb_account.py"), "create", login, "-p", PASSWORD], check=True,
                       capture_output=True)
        accid = int(sql(f"select id from accounts where login='{login}'"))
        charid = int(sql("select max(charid) + 1 from chars"))
        q = [f"update accounts set content_ids={acct[0]}, expansions={acct[1]}, features={acct[2]} where id={accid};"]
        for t in CLONE:
            sel = []
            for c in cols[t]:
                if c == "charid":
                    sel.append(str(charid))
                elif t == "chars" and c == "accid":
                    sel.append(str(accid))
                elif t == "chars" and c == "charname":
                    sel.append(f"'{name}'")
                else:
                    sel.append(f"`{c}`")
            if t != "chars":
                q.append(f"delete from {t} where charid={charid};")  # rows the chars insert trigger made
            q.append(f"insert into {t} ({', '.join('`' + c + '`' for c in cols[t])}) select {', '.join(sel)} from {t} where charid={TEMPLATE};")
        race = rng.randint(1, 8)
        look = {c: rng.choice(v) for c, v in looks.items()}
        if rng.random() < 0.3:
            look["head"] = 0  # some bare heads, so hair styles show
        if rng.random() < 0.4:
            look["sub"] = 0
        q.append(f"update char_look set race={race}, face={rng.randint(0, 15)}, size={rng.randint(0, 2)}, "
                 + ", ".join(f"{c}={v}" for c, v in look.items()) + f" where charid={charid};")
        sql("\n".join(q))
        print(f"{login} / {name}: accid {accid} charid {charid} race {race}", flush=True)


def place(zone, x, y, z, r):
    rng = random.Random(7)
    for _, _, charid in crowd():
        if charid:
            a, d = rng.uniform(0, 2 * math.pi), r * math.sqrt(rng.random())
            sql(f"update chars set pos_zone={zone}, pos_prevzone={zone}, pos_x={x + d * math.cos(a):.3f}, pos_y={y}, pos_z={z + d * math.sin(a):.3f}, "
                f"pos_rot={rng.randint(0, 255)} where charid={charid} and charid not in (select charid from accounts_sessions)")
    print(f"placed {len(crowd())} at zone {zone} ({x}, {y}, {z}) r {r}")


def delete():
    tables = sql("select table_name from information_schema.columns where table_schema='xidb' and column_name='charid'").split()
    for accid, login, charid in crowd():
        q = []
        if charid:
            q += [f"delete from {t} where charid={charid};" for t in tables]
        q.append(f"delete from accounts where id={accid};")
        sql("\n".join(q))
        print(f"deleted {login} (accid {accid}, charid {charid})")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "create":
        create(int(sys.argv[2]))
    elif cmd == "place":
        place(int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5]), float(sys.argv[6]) if len(sys.argv) > 6 else 6.0)
    elif cmd == "delete":
        delete()
    else:
        for row in crowd():
            print(*row)
