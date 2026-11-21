#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The bundled Ashita addons (third_party/ashita-addons, issue #138): the standard Ashita v4 addons and libraries that
work in the client, the only addons the public release ships.

  vendor_ashita_addons.py update --ashita ASHITA_V4BETA_CLONE [--rev REV]
      replace the vendored set with the git-tracked addons/ of an Ashita-v4beta clone at REV (default HEAD), less
      EXCLUDED, then rewrite MANIFEST.sha256
  vendor_ashita_addons.py check
      the vendored tree matches MANIFEST.sha256 exactly (no extra, missing or changed file); exit 1 otherwise

Left out of the upstream set: the addons in EXCLUDED (Windows-only, or built into the client), editor settings (.vscode),
Windows binaries (.dll, .exe) and archives (.zip). Text files are stored with LF line endings.
"""
import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BUNDLE = os.path.join(ROOT, "third_party", "ashita-addons")
MANIFEST = "MANIFEST.sha256"
# Upstream addons left out of the bundle, with why (third_party/ashita-addons/README.md; the export rejects them too).
EXCLUDED = {
    "hideconsole": "Windows-only: hides Ashita's bootloader console, which the client doesn't have",
    "ime": "Windows-only: the Windows IME",
    "affinity": "Windows-only: Win32 process CPU affinity",
    "freemem": "Windows-only: SetProcessWorkingSetSize, a no-op here",
    "autologin": "built into the client: --char / --autoconnect and the launcher's profiles",
    "timestamp": "built into the client: the native /timestamp and Config's chat timestamps",
    "renamer": "crashes the client: calls game functions it scanned for (docs/ashita-compat.md)",
    "watchdog": "crashes the client: calls game functions it scanned for (docs/ashita-compat.md)",
    "filterscan": "crashes the client: calls game functions it scanned for (docs/ashita-compat.md)",
    "ahcolors": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "ahgo": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "cartographer": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "fastswap": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "filterless": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "instantah": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "mapdot": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "mipmap": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "nomad": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "noname": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "paranormal": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "quicksets": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "trimspells": "does nothing: finds no stand-in for the game code it patches (\"Failed to locate ... pointer\")",
    "chime": "does nothing: fails to load (the game's chime sounds are not mapped)",
    "casper": "does nothing: sets entity render flags, which the client does not read",
    "chamcham": "does nothing: writes model colours into actor memory the client does not have",
    "truesight": "does nothing: sets entity render flags, which the client does not read",
    "skeletonkey": "does nothing: writes an entity's status field, which the client does not read",
    "cleancs": "does nothing: the font / primitive / ImGui managers' SetVisible is a no-op here",
    "hideui": "does nothing: the font / primitive / ImGui managers' SetVisible is a no-op here",
    "chatmon": "does nothing: its feature is sounds, and addon sounds are not played",
    "chatfix": "harmful: shifts LSB's chat packets by a byte for an old server layout, cutting messages",
    "instantchat": "does nothing: the client's chat log has no delay to remove",
    "macrofix": "does nothing: the client's macro palette has no delay to remove",
    "enternity": "does nothing: edits log lines, but the dialogue box's wait comes from the event, not the log",
    "minimapmon": "does nothing: controls the Minimap plugin, which the client does not have",
    "move": "does nothing: moves and resizes the window through Win32 calls",
    "dynamic_entity_renamer": "built into the client: dynamic-entity renames (LSB's spawn-name layout and HorizonXI's "
                              "0x1FF list) are handled natively",
    "hideobs": "Windows-only: hides the window from OBS through SetWindowDisplayAffinity",
}
SKIP_DIRS = {".vscode"}
SKIP_EXT = {".dll", ".exe", ".zip"}
TEXT_EXT = {".lua", ".md", ".json", ".txt"}


def skipped(rel):
    parts = rel.split("/")
    if len(parts) > 2 and parts[0] == "addons" and parts[1] in EXCLUDED:
        return True
    return any(p in SKIP_DIRS for p in parts) or os.path.splitext(rel)[1].lower() in SKIP_EXT


def lf(data, rel):
    return data.replace(b"\r\n", b"\n") if os.path.splitext(rel)[1].lower() in TEXT_EXT else data


def bundle_files(bundle=BUNDLE):
    """Every file under bundle/addons, as paths relative to bundle ('addons/...')."""
    out = []
    for dirpath, _, names in os.walk(os.path.join(bundle, "addons")):
        for n in names:
            out.append(os.path.relpath(os.path.join(dirpath, n), bundle).replace(os.sep, "/"))
    return sorted(out)


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def read_manifest(bundle=BUNDLE):
    """{relative path: sha256} from bundle/MANIFEST.sha256."""
    out = {}
    with open(os.path.join(bundle, MANIFEST)) as f:
        for ln in f:
            ln = ln.strip()
            if ln and not ln.startswith("#"):
                digest, rel = ln.split(None, 1)
                out[rel] = digest
    return out


def addon_names(manifest):
    """The addon folder names the manifest lists (addons/<name>/...), libs excluded."""
    return sorted({p.split("/")[1] for p in manifest if p.count("/") >= 2 and p.split("/")[1] != "libs"})


def check(bundle=BUNDLE):
    """Problems as strings: files not in the manifest, listed files missing or changed."""
    want = read_manifest(bundle)
    have = bundle_files(bundle)
    problems = [f"not in {MANIFEST}: {p}" for p in have if p not in want]
    for p, digest in want.items():
        full = os.path.join(bundle, p)
        if not os.path.isfile(full):
            problems.append(f"missing: {p}")
        elif sha256(full) != digest:
            problems.append(f"changed: {p}")
    return problems


def update(ashita, rev):
    commit = subprocess.run(["git", "-C", ashita, "rev-parse", rev], check=True, capture_output=True,
                            text=True).stdout.strip()
    tar = subprocess.run(["git", "-C", ashita, "archive", commit, "addons"], check=True, capture_output=True).stdout
    shutil.rmtree(os.path.join(BUNDLE, "addons"), ignore_errors=True)
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        for m in t.getmembers():
            if not m.isfile() or skipped(m.name):
                continue
            dst = os.path.join(BUNDLE, m.name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(lf(t.extractfile(m).read(), m.name))
    with open(os.path.join(BUNDLE, MANIFEST), "w") as f:
        f.write(f"# sha256 of every bundled file (tools/vendor_ashita_addons.py). Ashita-v4beta {commit}\n")
        for name, why in sorted(EXCLUDED.items()):
            f.write(f"# left out: {name}: {why}\n")
        for p in bundle_files():
            f.write(f"{sha256(os.path.join(BUNDLE, p))}  {p}\n")
    names = addon_names(read_manifest())
    print(f"vendored Ashita-v4beta {commit[:10]}: {len(names)} addons and addons/libs")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    up = sub.add_parser("update")
    up.add_argument("--ashita", required=True, help="an Ashita-v4beta git clone")
    up.add_argument("--rev", default="HEAD")
    sub.add_parser("check")
    args = ap.parse_args(argv)
    if args.cmd == "update":
        update(args.ashita, args.rev)
        return 0
    problems = check()
    for p in problems:
        print(p)
    print(f"ashita addons: {len(read_manifest())} files, {'OK' if not problems else f'{len(problems)} problems'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
