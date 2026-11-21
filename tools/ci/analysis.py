#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CI's lint, quality and security analysis (#109; docs/ci.md "Analysis").

    tools/ci/analysis.py TOOL [--build-dir DIR] [--base REV] [--update-baseline] [--github]

TOOL is one of
    clang-tidy   .clang-tidy's checks + the clang static analyzer on every repo TU in DIR's compile_commands.json
    cppcheck     cppcheck (warning, performance, portability) on the same TUs
    sast         the repo's semgrep rules (ci/analysis/semgrep.yml) + flawfinder level 4+ on src/ apps/ tests/
    lint         shellcheck on the shell scripts, ruff (ruff.toml) on the Python tools, the SPDX line on own sources
    format       clang-format (.clang-format) on the C++ lines changed since BASE (advisory: the code predates it)
    secrets      gitleaks on the commits since BASE + the forbidden-file check on the whole tree

Each finding is keyed by tool, check, file and message (no line numbers, so moving code does not make it new).
ci/analysis/baseline/<tool>.json holds the counts of the findings that existed when the tool was added; a run fails
when a key has more findings than the baseline allows. `secrets` has no baseline: any finding fails. Every run writes
a GitLab Code Quality report (ci-out/codequality-<tool>.json, shown on merge requests) and prints a short summary
first, then the new findings. --update-baseline rewrites the baseline from this run (after fixing findings, or to
accept ones that are not bugs; prefer an inline suppression with a reason, see docs/ci.md).

--github (the public repo's GitHub Actions workflow, #131) reports the same way for GitHub: each new finding becomes a
workflow annotation (::error, ::warning for format) and the summary goes to the job's step summary; no Code Quality
report is written. The baseline gating is the same. Without --base, the base is $XI_CI_BASE (the workflow passes the
pull request's base or the push's previous tip) when that commit exists, else the GitLab rules below.

Heavy tools (clang-tidy, cppcheck) are run through `tools/build.sh --run` by tools/ci/run.sh, so they take the
machine-wide build lock and use XI_JOBS = nproc - 2 workers.
"""
import argparse
import collections
import concurrent.futures
import csv
import fnmatch
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "ci-out")
BASELINE_DIR = os.path.join(ROOT, "ci", "analysis", "baseline")
SOURCE_DIRS = ("src/", "apps/", "tests/")
JOBS = max(1, int(os.environ.get("XI_JOBS", "0")) or (os.cpu_count() or 3) - 2)

# Code Quality severities: info, minor, major, critical, blocker.
SEVERITY = {"error": "critical", "warning": "major", "style": "minor", "info": "info", "note": "info",
            "performance": "minor", "portability": "minor", "information": "info"}


class Finding:
    def __init__(self, tool, check, path, line, message, severity="major"):
        self.tool, self.check, self.path, self.line = tool, check, path, int(line or 1)
        self.message, self.severity = message.strip(), severity

    def key(self):
        # Digits are folded so "previous declaration at line 40" survives edits above it.
        return f"{self.check}|{self.path}|{re.sub(r'[0-9]+', 'N', self.message)}"


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=kw.pop("cwd", ROOT), text=True, capture_output=True, **kw)


def rel(path):
    p = os.path.relpath(os.path.abspath(os.path.join(ROOT, path)), ROOT)
    return p.replace(os.sep, "/")


def git(*args):
    r = run(["git", *args])
    return r.stdout.strip() if r.returncode == 0 else ""


def base_rev(arg):
    """The commit this push is compared with: the MR base, the push's previous tip, or the merge base with main."""
    if arg:
        return arg
    gh = os.environ.get("XI_CI_BASE", "")
    if gh and set(gh) != {"0"} and git("rev-parse", "--verify", "--quiet", f"{gh}^{{commit}}"):
        return gh
    if os.environ.get("GITHUB_ACTIONS") == "true":
        # No usable base (a new branch, or the release's single commit): this commit by itself.
        return git("rev-parse", "HEAD^1") or "HEAD"
    for env in ("CI_MERGE_REQUEST_DIFF_BASE_SHA",):
        if os.environ.get(env):
            return os.environ[env]
    branch = os.environ.get("CI_COMMIT_BRANCH") or git("rev-parse", "--abbrev-ref", "HEAD")
    default = os.environ.get("CI_DEFAULT_BRANCH", "main")
    before = os.environ.get("CI_COMMIT_BEFORE_SHA", "")
    if branch == default:
        if before and set(before) != {"0"} and run(["git", "merge-base", "--is-ancestor", before, "HEAD"]).returncode == 0:
            return before
        return git("rev-parse", "HEAD^1") or "HEAD"
    # A branch: compare with where it left main. The runner clones only this branch, so fetch main first.
    for ref in (f"origin/{default}", f"gitlab/{default}", default):
        if git("rev-parse", "--verify", "--quiet", ref):
            break
    else:
        run(["git", "fetch", "--quiet", "origin", f"+refs/heads/{default}:refs/remotes/origin/{default}"])
        ref = f"origin/{default}"
    return git("merge-base", ref, "HEAD") or git("rev-parse", "HEAD^1") or "HEAD"


def compile_db(build_dir):
    path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(path):
        sys.exit(f"analysis: no {path}; build first (tools/build.sh {build_dir})")
    with open(path) as f:
        entries = json.load(f)
    seen, units = set(), []
    for e in entries:
        f = os.path.normpath(os.path.join(e["directory"], e["file"]))
        r = rel(f)
        if r.startswith(SOURCE_DIRS) and f not in seen:
            seen.add(f)
            units.append(f)
    return path, sorted(units)


INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def repo_includes(unit, include_dirs, memo):
    """Every repo file `unit` includes, directly or not (quoted includes, resolved like the compiler: the including
    file's directory, then the -I directories). A superset when an include is behind an #if, which only costs a
    re-run. Replaces a build's dependency files, so the analysis needs only a configure."""
    seen, todo = set(), [unit]
    while todo:
        f = todo.pop()
        if f in memo:
            found = memo[f]
        else:
            found = []
            try:
                with open(f, encoding="utf-8", errors="replace") as fh:
                    names = INCLUDE.findall(fh.read())
            except OSError:
                names = []
            for name in names:
                for d in (os.path.dirname(f), *include_dirs):
                    cand = os.path.normpath(os.path.join(d, name))
                    if os.path.isfile(cand):
                        if rel(cand).startswith(SOURCE_DIRS):
                            found.append(cand)
                        break
            memo[f] = found
        for g in found:
            if g not in seen:
                seen.add(g)
                todo.append(g)
    return seen


_HASHES = {}


def file_hash(path, cache=_HASHES):
    if path not in cache:
        try:
            with open(path, "rb") as f:
                cache[path] = hashlib.sha256(f.read()).hexdigest()
        except OSError:
            cache[path] = "missing"
    return cache[path]


def prune_cache(path, days):
    """Drop cache entries nobody has used for `days` (a hit refreshes the entry's mtime)."""
    cutoff = time.time() - days * 86400
    for name in os.listdir(path):
        full = os.path.join(path, name)
        try:
            if os.path.getmtime(full) < cutoff:
                os.remove(full)
        except OSError:
            pass


def fetchcontent_dir(build_dir):
    """FETCHCONTENT_BASE_DIR from the build folder's CMakeCache.txt (CI keeps it outside the build folder)."""
    try:
        with open(os.path.join(build_dir, "CMakeCache.txt")) as f:
            for line in f:
                if line.startswith("FETCHCONTENT_BASE_DIR:"):
                    return os.path.abspath(line.split("=", 1)[1].strip())
    except OSError:
        pass
    return None


DIAG = re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): (?P<sev>warning|error): (?P<msg>.*?) "
                  r"\[(?P<check>[^\]]+)\]$")


def tool_clang_tidy(args):
    db, units = compile_db(args.build_dir)
    version = run(["clang-tidy", "--version"]).stdout
    config = file_hash(os.path.join(ROOT, ".clang-tidy"))
    with open(db) as f:
        commands = {os.path.normpath(os.path.join(e["directory"], e["file"])): e.get("command") or " ".join(e["arguments"])
                    for e in json.load(f)}
    include_dirs = sorted({os.path.normpath(d) for c in commands.values() for d in re.findall(r"-I\s*(\S+)", c)
                           if rel(d).startswith(SOURCE_DIRS) or rel(d) == "src"})
    memo = {}
    # The key uses repo-relative paths: the checkout, the build folder and the FetchContent folder are replaced by
    # placeholders, so the same sources analysed from another folder (the release dry run's exported tree, #131) find
    # the same cached results.
    places = [(os.path.abspath(args.build_dir), "<build>"), (ROOT, "<src>")]
    deps = fetchcontent_dir(args.build_dir)
    if deps:
        places.append((deps, "<deps>"))
    # Longest first: one may be a prefix of another (the dry run's release-tree/ next to its release/ build folder).
    places.sort(key=lambda p: len(p[0]), reverse=True)

    def portable(text):
        for path, name in places:
            text = text.replace(path, name)
        return text

    # Results are keyed by everything that decides them (the tool, the config, the compile command, the TU and every
    # repo header it includes), so a branch pipeline re-analyses only the TUs its changes reach, and one cache serves
    # every branch (CI shares it across pipelines: XI_TIDY_CACHE). On main (or with --no-cache) every TU is analysed
    # again, which also refreshes the cache, unless XI_TIDY_READ_CACHE=1 (the release dry run on main, which runs
    # after the analysis stage has just refreshed it).
    cache_dir = os.environ.get("XI_TIDY_CACHE") or os.path.join(args.build_dir, "clang-tidy-cache")
    os.makedirs(cache_dir, exist_ok=True)
    prune_cache(cache_dir, days=21)
    full = args.no_cache or (os.environ.get("CI_COMMIT_BRANCH") and
                             os.environ.get("CI_COMMIT_BRANCH") == os.environ.get("CI_DEFAULT_BRANCH") and
                             os.environ.get("XI_TIDY_READ_CACHE") != "1")

    def one(unit):
        h = hashlib.sha256((version + config + rel(unit) + portable(commands.get(unit, ""))).encode())
        for p in sorted({unit, *repo_includes(unit, include_dirs, memo)}):
            h.update(f"{rel(p)}:{file_hash(p)}".encode())
        cached = os.path.join(cache_dir, h.hexdigest())
        if not full and os.path.exists(cached):
            os.utime(cached)
            with open(cached) as f:
                return f.read(), True
        r = run(["clang-tidy", "-p", os.path.dirname(db), "--quiet", "--extra-arg=-Wno-unknown-warning-option",
                 "--extra-arg=-Wno-error", unit])
        text = r.stdout
        if r.returncode != 0 and "error:" not in text:
            text += f"\n{unit}:1:1: error: clang-tidy failed: {r.stderr.strip()[-300:]} [clang-tidy-failure]\n"
        with open(cached + ".tmp", "w") as f:
            f.write(text)
        os.replace(cached + ".tmp", cached)
        return text, False

    findings, seen, hits = [], set(), 0
    with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
        for unit, (text, was_cached) in zip(units, pool.map(one, units)):
            hits += was_cached
            for line in text.splitlines():
                m = DIAG.match(line)
                if not m:
                    continue
                path, check = rel(m["file"]), m["check"]
                if check == "clang-diagnostic-error" and not path.startswith(SOURCE_DIRS):
                    # A TU that does not parse (a missing generated header, say) is only half analysed: always a
                    # finding, on the TU, wherever the error is.
                    m = {"file": unit, "line": "1", "col": "1", "sev": "error", "check": check,
                         "msg": f"does not parse: {rel(m['file'])}: {m['msg']}"}
                    path = rel(unit)
                # Plain compiler warnings are the build's job (gcc, -Werror); keep only real parse errors.
                if not path.startswith(SOURCE_DIRS) or (check.startswith("clang-diagnostic-") and m["sev"] != "error"):
                    continue
                k = (path, m["line"], m["col"], check)
                if k not in seen:
                    seen.add(k)
                    findings.append(Finding("clang-tidy", check, path, m["line"], m["msg"], SEVERITY[m["sev"]]))
    print(f"clang-tidy: {len(units)} translation units, {len(units) - hits} analysed, {hits} unchanged since a "
          f"cached run{' (full run: cache not read)' if full else ''}, {JOBS} jobs")
    return findings


def tool_cppcheck(args):
    db, units = compile_db(args.build_dir)
    cache = os.path.join(args.build_dir, "cppcheck-cache")
    os.makedirs(cache, exist_ok=True)
    # Only the repo's own TUs: a filtered copy of the compile database.
    filtered = os.path.join(args.build_dir, "compile_commands.cppcheck.json")
    with open(db) as f:
        entries = [e for e in json.load(f) if os.path.normpath(os.path.join(e["directory"], e["file"])) in set(units)]
    with open(filtered, "w") as f:
        json.dump(entries, f)
    template = "{file}:{line}:{column}: {severity}: {message} [{id}]"
    r = run(["cppcheck", f"--project={filtered}", f"-j{JOBS}", f"--cppcheck-build-dir={cache}", "--quiet",
             "--enable=warning,performance,portability", "--inline-suppr", "--std=c++20", "--library=posix",
             "--suppress=missingIncludeSystem", "--suppress=missingInclude", "--suppress=unmatchedSuppression",
             "--suppress=normalCheckLevelMaxBranches", "--suppress=checkersReport",
             f"--suppress=*:{args.build_dir}/*", "--suppress=*:*/_deps/*", "--suppress=*:/usr/*",
             f"--template={template}"])
    pat = re.compile(r"^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+): (?P<sev>\w+): (?P<msg>.*) \[(?P<check>[^\]]+)\]$")
    findings, seen = [], set()
    for line in r.stderr.splitlines():
        m = pat.match(line)
        if not m:
            continue
        path = rel(m["file"])
        if not path.startswith(SOURCE_DIRS):
            continue
        k = (path, m["line"], m["check"], m["msg"])
        if k not in seen:
            seen.add(k)
            findings.append(Finding("cppcheck", m["check"], path, m["line"], m["msg"], SEVERITY.get(m["sev"], "major")))
    print(f"cppcheck: {len(units)} translation units, {JOBS} jobs")
    return findings


def tool_sast(args):
    findings = []
    rules = os.path.join(ROOT, "ci", "analysis", "semgrep.yml")
    r = run(["semgrep", "scan", "--metrics=off", "--disable-version-check", "--quiet", "--json", "--config", rules,
             f"--jobs={JOBS}", "src", "apps", "tests"])
    data = json.loads(r.stdout or "{}")
    if "results" not in data:
        sys.exit(f"analysis: semgrep failed:\n{r.stderr[-2000:]}")
    for x in data["results"]:
        sev = {"ERROR": "critical", "WARNING": "major", "INFO": "minor"}.get(x["extra"].get("severity"), "major")
        check = "semgrep." + x["check_id"].split(".")[-1]
        findings.append(Finding("sast", check, rel(x["path"]), x["start"]["line"], x["extra"]["message"], sev))
    skipped = len(data.get("errors", []))
    r = run(["flawfinder", "--minlevel=4", "--columns", "--dataonly", "--quiet", "--csv", "src", "apps", "tests"])
    for x in csv.DictReader(io.StringIO(r.stdout)):
        findings.append(Finding("sast", f"flawfinder.{x['Name']}", rel(x["File"]), x["Line"],
                                f"{x['Warning']} (CWE {x['CWEs']})", "critical" if int(x["Level"]) >= 5 else "major"))
    print(f"sast: semgrep ({skipped} constructs semgrep's C++ parser skipped) + flawfinder level 4+")
    return findings


def tool_lint(args):
    findings = []
    scripts = [f for f in git("ls-files", "*.sh").splitlines() if f]
    r = run(["shellcheck", "--format=gcc", "--external-sources", *scripts]) if scripts else None
    pat = re.compile(r"^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+): (?P<sev>\w+): (?P<msg>.*) \[(?P<check>SC\d+)\]$")
    for line in (r.stdout.splitlines() if r else []):
        m = pat.match(line)
        if m:
            findings.append(Finding("lint", f"shellcheck.{m['check']}", rel(m["file"]), m["line"], m["msg"],
                                    SEVERITY.get(m["sev"], "minor")))
    pys = [f for f in git("ls-files", "*.py").splitlines() if f]
    r = run(["ruff", "check", "--no-cache", "--output-format=json", "--exit-zero", *pys]) if pys else None
    for x in json.loads(r.stdout or "[]") if r else []:
        code = x.get("code") or "syntax"
        sev = "major" if code.startswith(("F", "E9", "B", "S")) else "minor"
        findings.append(Finding("lint", f"ruff.{code}", rel(x["filename"]), x["location"]["row"], x["message"], sev))
    sources = spdx_sources()
    for f in sources:
        with open(os.path.join(ROOT, f), encoding="utf-8", errors="replace") as fh:
            head = [fh.readline() for _ in range(3)]
        if not any(SPDX_TAG in line for line in head):
            findings.append(Finding("lint", "spdx.missing", f, 1, f"no '{SPDX_TAG}' line in the first three lines"))
    print(f"lint: shellcheck on {len(scripts)} scripts, ruff on {len(pys)} Python files, SPDX on {len(sources)} sources")
    return findings


# The project's own sources carry this line (#132, LICENSE; THIRD-PARTY.md lists what is not ours). A new file without
# it is a new lint finding.
SPDX_TAG = "SPDX-License-Identifier: GPL-3.0-or-later"
SPDX_PATTERNS = ("*.cpp", "*.h", "*.py", "*.sh", "*.lua", "*.glsl", "*.vert", "*.frag", "*.comp", "*.cmake",
                 "CMakeLists.txt", "*/CMakeLists.txt")


def spdx_sources():
    # third_party/ (the bundled Ashita addons, #138) keeps its upstream headers and licenses.
    return [f for f in git("ls-files", *SPDX_PATTERNS).splitlines() if f and not f.startswith("third_party/")]


def tool_format(args):
    base = base_rev(args.base)
    r = run(["git", "clang-format", "--diff", "--quiet", "--extensions", "cpp,h,hpp,inc", base, "--",
             "src", "apps", "tests"])
    findings, path, hunk = [], None, 0
    for line in r.stdout.splitlines():
        if line.startswith("+++ b/"):
            path = line[6:]
        elif line.startswith("@@") and path:
            m = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)", line)
            hunk = int(m.group(1)) if m else 1
            findings.append(Finding("format", "clang-format", path, hunk,
                                    "Changed lines differ from .clang-format", "info"))
    if r.stdout.strip():
        with open(os.path.join(OUT, "format.diff"), "w") as f:
            f.write(r.stdout)
    short = base[:10] if re.fullmatch(r"[0-9a-f]{40}", base) else base
    print(f"format: C++ lines changed since {short}; fix with `git clang-format {short}`"
          + (" (diff in ci-out/format.diff)" if r.stdout.strip() else ""))
    args.format_diff = r.stdout
    return findings


# --- secrets and forbidden files --------------------------------------------------------------------------------

FORBIDDEN_EXT = {
    "game data / captures": {"dat", "pcap", "pcapng", "cap", "bgw", "spw", "ftable", "vtable"},
    "images / screenshots / textures": {"png", "jpg", "jpeg", "gif", "bmp", "webp", "tga", "dds", "tif", "tiff",
                                        "ktx", "ktx2", "exr", "hdr"},
    "audio / video": {"wav", "ogg", "mp3", "flac", "opus", "mp4", "mkv", "webm", "avi", "mov"},
    "binaries / libraries": {"so", "dll", "exe", "dylib", "a", "lib", "o", "obj", "pdb", "bin", "spv"},
    "archives": {"zip", "7z", "rar", "tar", "gz", "xz", "bz2", "zst", "tgz"},
    "fonts (third-party)": {"ttf", "otf", "ttc", "woff", "woff2"},
    "keys / credentials": {"pem", "key", "p12", "pfx", "kdbx", "keystore", "jks"},
}
FORBIDDEN_NAME = [
    (re.compile(r"(^|/)(ROM\d*|addons|libs)/", re.I), "game data / third-party addons and libs"),
    (re.compile(r"(nvngx|nvsdk|libnvidia)[^/]*$|\.so(\.\d+)+$", re.I), "NVIDIA / binary libraries"),
    (re.compile(r"(^|/)(\.env|local\.env|id_[rd]sa[^/]*|[^/]*gitlab-token[^/]*|[^/]*\.htpasswd|credentials[^/]*)$",
                re.I), "credentials"),
]
MAX_SIZE = 2 * 1024 * 1024

# The bundled Ashita addons (#138, third_party/ashita-addons/README.md): the one third-party addon set allowed in the
# repo, file by file as its MANIFEST.sha256 lists them (with their hashes), plus the bundle's own README, LICENSE and
# manifest. Anything else there, or a listed file that changed, stays forbidden.
BUNDLE_DIR = "third_party/ashita-addons/"
BUNDLE_OWN = {"README.md", "LICENSE-GPLv3.md", "MANIFEST.sha256"}


def bundle_manifest(top):
    """{repo path: sha256} of the bundled addons' files, from the manifest under top ({} without one)."""
    path = os.path.join(top, BUNDLE_DIR, "MANIFEST.sha256")
    out = {}
    if os.path.exists(path):
        with open(path) as f:
            for ln in f:
                ln = ln.strip()
                if ln and not ln.startswith("#"):
                    digest, rel = ln.split(None, 1)
                    out[BUNDLE_DIR + rel] = digest
    return out


def bundle_allowed(top, path, manifest):
    """Whether `path` is a file of the bundled addon set, unchanged."""
    if not path.startswith(BUNDLE_DIR):
        return False
    if path[len(BUNDLE_DIR):] in BUNDLE_OWN:
        return True
    full = os.path.join(top, path)
    if path not in manifest or not os.path.isfile(full) or os.path.islink(full):
        return False
    with open(full, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest() == manifest[path]


def forbidden_files():
    allow_path = os.path.join(ROOT, "ci", "analysis", "allowed-files.txt")
    allow = []
    if os.path.exists(allow_path):
        with open(allow_path) as f:
            allow = [ln.split("#")[0].strip() for ln in f if ln.split("#")[0].strip()]
    findings = []
    manifest = bundle_manifest(ROOT)
    for path in git("ls-files", "-z").split("\0"):
        if not path or any(fnmatch.fnmatch(path, a) for a in allow) or bundle_allowed(ROOT, path, manifest):
            continue
        full = os.path.join(ROOT, path)
        ext = path.rsplit(".", 1)[-1].lower() if "." in os.path.basename(path) else ""
        why = next((k for k, v in FORBIDDEN_EXT.items() if ext in v), None)
        why = why or next((w for p, w in FORBIDDEN_NAME if p.search(path)), None)
        if not why and os.path.isfile(full) and not os.path.islink(full):
            size = os.path.getsize(full)
            with open(full, "rb") as f:
                head = f.read(8192)
            if size > MAX_SIZE:
                why = f"large file ({size // 1024} KiB)"
            elif b"\0" in head:
                why = "binary file"
        if why:
            findings.append(Finding("secrets", "forbidden-file", path, 1,
                                    f"Not allowed in the repo: {why} (docs/ci.md 'Forbidden files'); allowlist in "
                                    "ci/analysis/allowed-files.txt only if it is our own work", "blocker"))
    return findings


def tool_secrets(args):
    base = base_rev(args.base)
    report = os.path.join(OUT, "gitleaks.json")
    cmd = ["gitleaks", "git", "--no-banner", "--redact", "--exit-code", "0", "--report-format", "json",
           "--report-path", report, "--config", os.path.join(ROOT, ".gitleaks.toml")]
    span = "the whole history" if args.full_history else f"commits since {base[:10]}"
    if not args.full_history:
        cmd += ["--log-opts", f"{base}..HEAD"] if base != git("rev-parse", "HEAD") else ["--log-opts", "-1"]
    r = run(cmd + ["."])
    if r.returncode != 0 or not os.path.exists(report):
        sys.exit(f"analysis: gitleaks failed:\n{r.stderr[-2000:]}")
    findings = []
    with open(report) as f:
        for x in json.load(f) or []:
            findings.append(Finding("secrets", f"gitleaks.{x['RuleID']}", x["File"], x["StartLine"],
                                    f"{x['Description']} in commit {x['Commit'][:10]} (the value is not printed)",
                                    "blocker"))
    findings += forbidden_files()
    print(f"secrets: gitleaks on {span}, forbidden files on the whole tree")
    return findings


TOOLS = {"clang-tidy": tool_clang_tidy, "cppcheck": tool_cppcheck, "sast": tool_sast, "lint": tool_lint,
         "format": tool_format, "secrets": tool_secrets}
NO_BASELINE = {"secrets", "format"}


def codequality(findings, tool):
    out, used = [], collections.Counter()
    for f in findings:
        k = f.key()
        used[k] += 1
        fp = hashlib.sha256(f"{f.tool}|{k}|{used[k]}".encode()).hexdigest()
        out.append({"type": "issue", "check_name": f.check, "description": f"{f.check}: {f.message}"[:1000],
                    "categories": ["Security" if tool in ("sast", "secrets") or "security" in f.check or
                                   f.check.startswith("cert-") else "Bug Risk"],
                    "severity": f.severity, "fingerprint": fp, "engine_name": f.tool,
                    "location": {"path": f.path, "lines": {"begin": f.line}}})
    with open(os.path.join(OUT, f"codequality-{tool}.json"), "w") as fh:
        json.dump(out, fh, indent=1)


def github_report(tool, findings, new, over, fixed, by_check):
    """--github: annotations for the new findings and a step summary (GitHub Actions workflow commands)."""
    level = "warning" if tool == "format" else "error"

    def esc(text, prop=False):
        text = text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        return text.replace(":", "%3A").replace(",", "%2C") if prop else text

    for f in new[:50]:
        print(f"::{level} file={esc(f.path, True)},line={f.line},title={esc(f'{tool}: {f.check}', True)}::"
              f"{esc(f.message[:500])}")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary:
        return
    lines = [f"### {tool}: {len(findings)} findings, {sum(over.values())} new, {fixed} fixed since the baseline", ""]
    if new:
        lines += ["| File | Line | Check | Message |", "|---|---|---|---|"]
        for f in new[:40]:
            msg = f.message[:200].replace("|", "\\|")
            lines.append(f"| `{f.path}` | {f.line} | `{f.check}` | {msg} |")
        if len(new) > 40:
            lines.append(f"\n... {len(new) - 40} more")
        if by_check:
            lines += ["", "New by check: " + ", ".join(f"`{c}` {n}" for c, n in by_check.most_common(10))]
    with open(summary, "a") as fh:
        fh.write("\n".join(lines) + "\n\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tool", choices=sorted(TOOLS))
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    ap.add_argument("--base", help="compare with this commit (default: the MR base / previous push / merge base)")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--full-history", action="store_true", help="secrets: scan every commit, not just this push's")
    ap.add_argument("--no-cache", action="store_true", help="clang-tidy: analyse every TU, ignoring cached results")
    ap.add_argument("--github", action="store_true",
                    help="report as GitHub Actions annotations and a step summary, not a GitLab Code Quality file")
    args = ap.parse_args()
    args.build_dir = os.path.abspath(args.build_dir)
    os.makedirs(OUT, exist_ok=True)
    for exe in {"clang-tidy": ["clang-tidy"], "cppcheck": ["cppcheck"], "sast": ["semgrep", "flawfinder"],
                "lint": ["shellcheck", "ruff"], "format": ["git-clang-format", "clang-format"],
                "secrets": ["gitleaks"]}[args.tool]:
        if not shutil.which(exe):
            sys.exit(f"analysis: {exe} is not installed (the CI image ci/Dockerfile has it)")

    findings = TOOLS[args.tool](args)
    findings.sort(key=lambda f: (f.path, f.line, f.check))
    if not args.github:
        codequality(findings, args.tool)
    counts = collections.Counter(f.key() for f in findings)
    base_file = os.path.join(BASELINE_DIR, f"{args.tool}.json")

    if args.update_baseline and args.tool not in NO_BASELINE:
        os.makedirs(BASELINE_DIR, exist_ok=True)
        with open(base_file, "w") as fh:
            json.dump({"tool": args.tool, "total": len(findings), "findings": dict(sorted(counts.items()))}, fh,
                      indent=1, ensure_ascii=False)
            fh.write("\n")
        print(f"=== {args.tool}: baseline written ({len(findings)} findings, {len(counts)} keys) -> {rel(base_file)}")
        return 0

    baseline = {}
    if args.tool not in NO_BASELINE and os.path.exists(base_file):
        with open(base_file) as fh:
            baseline = json.load(fh).get("findings", {})
    over = {k: n - baseline.get(k, 0) for k, n in counts.items() if n > baseline.get(k, 0)}
    new = [f for f in findings if f.key() in over]
    fixed = sum(max(0, n - counts.get(k, 0)) for k, n in baseline.items())
    by_check = collections.Counter(f.check for f in new)

    where = "" if args.github else f" (full list: ci-out/codequality-{args.tool}.json)"
    print(f"=== {args.tool}: {len(findings)} findings, {sum(over.values())} new, {fixed} fixed since the baseline"
          f"{where} ===")
    if by_check:
        print("new by check: " + ", ".join(f"{c} {n}" for c, n in by_check.most_common(10)))
    for f in new[:40]:
        extra = f" (+{over[f.key()]} over the baseline for this key)" if baseline.get(f.key()) else ""
        print(f"{f.path}:{f.line}: [{f.check}] {f.message[:300]}{extra}")
    if len(new) > 40:
        print(f"... {len(new) - 40} more" + ("" if args.github else f" in ci-out/codequality-{args.tool}.json"))
    if args.tool == "format" and getattr(args, "format_diff", ""):
        print("--- clang-format would change (first 60 lines):")
        print("\n".join(args.format_diff.splitlines()[:60]))
    if args.github:
        github_report(args.tool, findings, new, over, fixed, by_check)
    if fixed and not new:
        print(f"{fixed} baselined findings are gone: shrink the baseline with "
              f"`tools/ci/analysis.py {args.tool} --update-baseline` (docs/ci.md)")
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
