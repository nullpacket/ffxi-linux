#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the browsable docs: docs/index.html (status, plans, recent work) and docs/html/*.html (every docs/*.md).

The Markdown files stay the source of truth; run this after changing them:
    python3 tools/build_docs.py [--check-links]
--check-links (CI) also fails on a relative link, in docs/*.md or a Markdown file at the repo root, to a page or file
that is not there. The index lists only the pages that exist, and leaves out the status, plans and report panels when
their pages are not there (the public export has no STATUS.md or PLANS.md).
No dependencies beyond the standard library (a small Markdown subset: headings, paragraphs, nested lists, tables,
fenced code, inline code, bold, italics, links, rules).
"""

import datetime
import html
import pathlib
import re
import subprocess
import sys
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
OUT = DOCS / "html"

# The public export's private-block markers: not drawn, and a table continues across them.
EXPORT_MARKERS = ("<!-- private -->", "<!-- /private -->")

# Reading order and a line for each document on the index.
DOC_INFO = {
    "setup": "Setting up: build dependencies, importing the game data from your own install, compress.dat, a server, the launcher, optional DLSS and Ashita plugins.",
    "STATUS": "Where every area stands, the environment outside the repo, and the open items.",
    "PLANS": "The goals ffxi-linux can be set off on, each with its finish line.",
    "status-report-2026-10-18": "The long-form verified status report, with addenda per milestone.",
    "zone-sweep-2026-10-18": "Every zone loaded by a bot: what loads, what the server redirects, what was found and fixed.",
    "protocol-login": "Login and lobby: the PlayOnline handshake, character list and hand-off to the map server.",
    "protocol-map": "The map server's UDP protocol: framing, encryption, compression and every packet the client uses.",
    "lsb-packet-timing": "What the LSB map server expects of the client's send rate, its limits and timeouts, and the game's traced 400 ms base (#77).",
    "dat-format": "The game's data files: locator, chunks, textures, zones, collision, weather, characters, interface.",
    "effect-format": "Visual effects: schedulers, generators, particles, meshes, zone effects, shadows.",
    "event-format": "Event scripts and cutscenes: the opcode VM, camera shots, actors, costumes.",
    "sound-format": "Sound effects, music, footsteps, ambience and their formats.",
    "ui-format-notes": "The original interface: layouts, sprites, menus, Search, Config.",
    "input-format": "Keyboard layouts, bindings and macros.",
    "commands": "The text commands the client understands.",
    "performance": "Frame-time target, how to measure it, the test crowd, and before / after numbers.",
    "dlss": "NVIDIA DLSS 4 behind --dlss: what runs on Linux, the inputs, building it, the licence, measurements.",
    "bugreport": "/bugreport: the in-game bug report bundle, how it reaches GitLab, and how Claude fetches one (goal 48).",
    "workflow": "How work flows through the local GitLab: issues, branches, CI pipelines and merge requests (goal 40).",
    "ci": "CI: the pipeline's stages, the analysis and its baselines, the build image, the build lock and the generated docs.",
    "graphics-guide":"Reference guide to every optional graphics flag (DLSS, ray tracing, post, path tracing, HD textures): what each does, how they combine, costs and recommended combinations.",
    "launcher": "xi-launcher (goal 44): server profiles, graphics presets and every option, one-click Connect; the files it writes and the client options it uses.",
    "enhanced-graphics": "Goal 41's optional graphics flags for side-by-side comparison: HD textures, and the others as they land.",
    "raytracing": "Optional ray-traced lighting behind --rt: how it re-weights the game's lighting, the passes, Ray Reconstruction, costs.",
    "ashita-compat": "Ashita addons: the two Ashita releases, what the layer covers, tests, and what does not load yet.",
    "public-release": "Plan for a public GitHub release (#125): the private-only inventory, the export with a fresh history, licensing, CI, setup guide and checklist.",
}


# ---------------------------------------------------------------------------------------------------------------
# Markdown subset


def slug(text, used):
    s = re.sub(r"[^a-z0-9]+", "-", re.sub(r"<[^>]+>", "", text).lower()).strip("-") or "section"
    base, n = s, 2
    while s in used:
        s = f"{base}-{n}"
        n += 1
    used.add(s)
    return s


def inline(text):
    """Inline Markdown -> HTML (escaped)."""
    codes = []

    def keep_code(m):
        codes.append("<code>" + html.escape(m.group(1).replace("\\|", "|")) + "</code>")
        return f"\x00{len(codes) - 1}\x00"

    text = re.sub(r"`([^`]+)`", keep_code, text)
    text = html.escape(text, quote=False).replace("\\|", "|")

    def link(m):
        label, target = m.group(1), m.group(2)
        if not re.match(r"^[a-z]+:", target):
            target = re.sub(r"\.md(#|$)", r".html\1", target)
        return f'<a href="{html.escape(target)}">{label}</a>'

    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![^\s(])\*(?=\w)(.+?)(?<=\w)\*(?!\w)", r"<em>\1</em>", text)
    # Tags people care about in these docs.
    text = re.sub(r"\b(CODE|DATA|INFERRED)\b", r'<span class="tag tag-\1">\1</span>', text)
    return re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], text)


def split_row(line):
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith("\\|"):
        line = line[:-1]
    return [c.strip() for c in re.split(r"(?<!\\)\|", line)]


LIST_RE = re.compile(r"^(\s*)([-*]|\d+\.)\s+(.*)$")


def markdown(md):
    """Returns (html, toc) where toc is [(level, id, text)] for h2 / h3."""
    lines = md.split("\n")
    out, toc, used = [], [], set()
    para = []
    lists = []  # stack of (indent, tag)
    i = 0

    def flush_para():
        if para:
            out.append("<p>" + inline(" ".join(s.strip() for s in para)) + "</p>")
            para.clear()

    def close_lists(to_indent=-1):
        while lists and lists[-1][0] > to_indent:
            out.append(f"</li></{lists.pop()[1]}>")

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if stripped in EXPORT_MARKERS:
            i += 1
            continue
        if stripped.startswith("```"):
            flush_para()
            close_lists()
            body = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                body.append(lines[i])
                i += 1
            out.append("<pre><code>" + html.escape("\n".join(body)) + "</code></pre>")
            i += 1
            continue
        m = re.match(r"^(#{1,6})\s+(.*)$", line)
        if m:
            flush_para()
            close_lists()
            level, text = len(m.group(1)), inline(m.group(2).strip())
            hid = slug(text, used)
            if level in (2, 3):
                toc.append((level, hid, re.sub(r"<[^>]+>", "", text)))
            out.append(f'<h{level} id="{hid}">{text}</h{level}>')
            i += 1
            continue
        if re.match(r"^\s*-{3,}\s*$", line):
            flush_para()
            close_lists()
            out.append("<hr>")
            i += 1
            continue
        if stripped.startswith("|") and i + 1 < len(lines) and re.match(r"^\s*\|?\s*:?-{2,}", lines[i + 1]):
            flush_para()
            close_lists()
            head = split_row(line)
            i += 2
            rows = []
            while i < len(lines) and (lines[i].strip().startswith("|") or lines[i].strip() in EXPORT_MARKERS):
                if lines[i].strip() not in EXPORT_MARKERS:
                    rows.append(split_row(lines[i]))
                i += 1
            t = ['<div class="table-wrap"><table><thead><tr>']
            t += [f"<th>{inline(c)}</th>" for c in head]
            t.append("</tr></thead><tbody>")
            for r in rows:
                t.append("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>")
            t.append("</tbody></table></div>")
            out.append("".join(t))
            continue
        m = LIST_RE.match(line)
        if m:
            flush_para()
            indent = len(m.group(1))
            tag = "ol" if m.group(2)[0].isdigit() else "ul"
            if lists and indent > lists[-1][0]:
                out.append(f"<{tag}><li>")
                lists.append((indent, tag))
            else:
                close_lists(indent)
                if lists and lists[-1][0] == indent:
                    out.append("</li><li>")
                else:
                    out.append(f"<{tag}><li>")
                    lists.append((indent, tag))
            item = [m.group(3)]
            i += 1
            # Continuation lines: indented, not a new item, not blank.
            while i < len(lines) and lines[i].strip() and not LIST_RE.match(lines[i]) and lines[i].startswith(" ") \
                    and not lines[i].strip().startswith("```") and not lines[i].strip().startswith("|"):
                item.append(lines[i].strip())
                i += 1
            out.append(inline(" ".join(item)))
            continue
        if not stripped:
            flush_para()
            # A blank line ends a list unless the next line continues it.
            nxt = next((l for l in lines[i + 1:] if l.strip()), "")
            if not LIST_RE.match(nxt):
                close_lists()
            i += 1
            continue
        if lists and line.startswith(" "):
            out.append(" " + inline(stripped))
            i += 1
            continue
        close_lists()
        para.append(line)
        i += 1
    flush_para()
    close_lists()
    return "\n".join(out), toc


# ---------------------------------------------------------------------------------------------------------------
# Pages

CSS = r"""
:root {
  --night: #070a1c;
  --win-top: rgba(46, 54, 122, 0.94);
  --win-bottom: rgba(16, 19, 58, 0.94);
  --edge: #9aa6dc;
  --edge-dim: #3b4386;
  --text: #e9ebf7;
  --muted: #a8aed6;
  --gold: #f0d58a;
  --gold-deep: #c99a3e;
  --done: #79d99a;
  --progress: #f0d58a;
  --open: #9aa6dc;
  --code-bg: rgba(5, 7, 24, 0.7);
  --serif: "Palatino Linotype", "Book Antiqua", Palatino, "URW Palladio L", Georgia, serif;
  --sans: "Segoe UI", "Noto Sans", "DejaVu Sans", system-ui, sans-serif;
  --mono: "JetBrains Mono", "DejaVu Sans Mono", Consolas, monospace;
}
* { box-sizing: border-box; }
* { scrollbar-color: #5d68b8 rgba(0, 0, 0, 0.3); scrollbar-width: thin; }
html { background: var(--night); }
body {
  margin: 0; color: var(--text); font: 15px/1.55 var(--sans);
  background:
    radial-gradient(1200px 600px at 80% -10%, rgba(80, 96, 200, 0.35), transparent 60%),
    radial-gradient(900px 500px at 0% 110%, rgba(150, 90, 200, 0.18), transparent 60%),
    radial-gradient(1px 1px at 12% 18%, #fff8, transparent), radial-gradient(1px 1px at 33% 72%, #fff6, transparent),
    radial-gradient(1px 1px at 58% 28%, #fff7, transparent), radial-gradient(1px 1px at 76% 62%, #fff5, transparent),
    radial-gradient(1px 1px at 91% 12%, #fff8, transparent), radial-gradient(1px 1px at 45% 90%, #fff5, transparent),
    linear-gradient(180deg, #0b1030 0%, #070a1c 60%, #05060f 100%);
  background-attachment: fixed;
  min-height: 100vh;
}
a { color: var(--gold); text-decoration: none; }
a:hover { text-decoration: underline; }
.page { max-width: 1240px; margin: 0 auto; padding: 28px 16px 60px; }

/* The game's menu window: a blue gradient panel with a light rim and a title tab. */
.window {
  position: relative; border-radius: 8px; padding: 18px 20px 16px;
  background: linear-gradient(180deg, var(--win-top), var(--win-bottom));
  border: 1px solid var(--edge);
  box-shadow: inset 0 0 0 1px rgba(0,0,0,0.55), inset 0 1px 0 rgba(255,255,255,0.18), 0 0 0 1px #0008, 0 10px 30px #0009;
  margin: 26px 0 0;
}
.window::before {
  content: ""; position: absolute; inset: 0; border-radius: 8px; pointer-events: none;
  background: repeating-linear-gradient(180deg, rgba(255,255,255,0.025) 0 1px, transparent 1px 3px);
}
.window > .title {
  position: absolute; top: -13px; left: 14px; padding: 1px 12px 2px;
  font: italic 600 14px/20px var(--serif); letter-spacing: 0.04em; color: #fff;
  background: linear-gradient(180deg, #5d68b8, #262c70); border: 1px solid var(--edge); border-radius: 5px;
  text-shadow: 0 1px 0 #000;
}

header.hero { display: flex; align-items: center; gap: 22px; padding: 10px 4px 0; }
.crystal { width: 64px; height: 80px; flex: none; filter: drop-shadow(0 0 14px rgba(120, 170, 255, 0.55)); }
header.hero h1 {
  margin: 0; font: 600 44px/1.05 var(--serif); letter-spacing: 0.02em;
  background: linear-gradient(180deg, #ffffff 0%, #dfe4ff 45%, #9aa6dc 100%);
  -webkit-background-clip: text; background-clip: text; color: transparent;
  text-shadow: 0 0 30px rgba(140, 160, 255, 0.25);
}
header.hero p { margin: 6px 0 0; color: var(--muted); }
.stats { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 12px; }
.stat { text-align: center; padding: 10px 6px; border-radius: 6px; background: rgba(0,0,0,0.25); border: 1px solid var(--edge-dim); }
.stat b { display: block; font: 600 26px/1.2 var(--serif); color: var(--gold); }
.stat span { color: var(--muted); font-size: 13px; }

.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(290px, 1fr)); gap: 14px; }
.card {
  padding: 12px 14px; border-radius: 6px; background: rgba(4, 6, 30, 0.45); border: 1px solid var(--edge-dim);
  transition: border-color .15s, transform .15s;
}
.card:hover { border-color: var(--edge); transform: translateY(-1px); }
.card h3 { margin: 0 0 6px; font: 600 16px/1.3 var(--serif); color: #fff; }
.card p { margin: 6px 0 0; color: var(--muted); font-size: 14px; }
.card .docs { margin-top: 8px; font-size: 13px; color: var(--muted); }

.chip { display: inline-block; padding: 1px 9px; border-radius: 999px; font-size: 12px; font-weight: 600; letter-spacing: .02em; border: 1px solid; }
.chip.done { color: var(--done); border-color: #2f7a4b; background: rgba(40, 120, 70, 0.18); }
.chip.progress { color: var(--progress); border-color: #8a6a22; background: rgba(160, 120, 40, 0.18); }
.chip.open { color: var(--open); border-color: var(--edge-dim); background: rgba(80, 90, 160, 0.15); }
.usertest { margin-top: 10px; padding: 8px 10px; border: 1px solid #8a6a22; border-radius: 6px; background: rgba(160, 120, 40, 0.12); font-size: 13px; }
.usertest b { color: var(--progress); display: block; margin-bottom: 4px; }
.plan-group { margin: 18px 0 10px; font: 600 15px/1.3 var(--serif); color: #dfe8ff; letter-spacing: .03em; }
.plan-group:first-of-type { margin-top: 4px; }

.cursor-list { list-style: none; margin: 0; padding: 0; }
.cursor-list li { position: relative; padding: 7px 10px 7px 28px; border-radius: 5px; }
.cursor-list li:hover { background: rgba(255,255,255,0.05); }
.cursor-list li:hover::before {
  content: "\25B6"; position: absolute; left: 8px; top: 7px; color: var(--gold); font-size: 12px;
  text-shadow: 0 0 6px var(--gold-deep);
}
.cursor-list .when { color: var(--muted); font: 12px var(--mono); margin-right: 8px; }
.cursor-list .hash { color: var(--edge); font: 12px var(--mono); margin-right: 8px; }

.columns { display: grid; grid-template-columns: 1fr 1fr; gap: 0 22px; }
@media (max-width: 900px) { .columns { grid-template-columns: 1fr; } }

/* Document pages */
.doc-layout { display: grid; grid-template-columns: 260px minmax(0, 1fr); gap: 22px; align-items: start; }
@media (max-width: 900px) { .doc-layout { grid-template-columns: 1fr; } .toc { position: static !important; } .toc-scroll { max-height: 320px; } }
.toc { position: sticky; top: 24px; }
.toc-scroll { max-height: calc(100vh - 80px); overflow: auto; margin: 0 -8px; padding: 0 8px; }
.toc a { display: block; color: var(--text); padding: 3px 6px 3px 18px; border-radius: 4px; font-size: 13px; position: relative; }
.toc a.h3 { padding-left: 32px; color: var(--muted); }
.toc a:hover { background: rgba(255,255,255,0.06); text-decoration: none; }
.toc a:hover::before { content: "\25B6"; position: absolute; left: 4px; color: var(--gold); font-size: 9px; top: 6px; }
.toc a.h3:hover::before { left: 18px; }
.crumbs { font-size: 13px; color: var(--muted); margin-bottom: 4px; }
.doc h1 { font: 600 32px/1.2 var(--serif); margin: 4px 0 14px; color: #fff; }
.doc h2 { font: 600 23px/1.3 var(--serif); margin: 30px 0 10px; color: var(--gold); border-bottom: 1px solid var(--edge-dim); padding-bottom: 4px; }
.doc h3 { font: 600 18px/1.3 var(--serif); margin: 22px 0 8px; color: #fff; }
.doc h4, .doc h5 { font: 600 15px/1.3 var(--sans); margin: 16px 0 6px; color: #dfe4ff; }
.doc p, .doc li { color: var(--text); }
.doc ul, .doc ol { padding-left: 22px; }
.doc li { margin: 3px 0; }
.doc li::marker { color: var(--gold-deep); }
.doc hr { border: 0; border-top: 1px solid var(--edge-dim); margin: 22px 0; }
code { font: 0.88em var(--mono); background: var(--code-bg); border: 1px solid #2a3070; border-radius: 4px; padding: 0 4px; color: #cfe0ff; }
pre { background: var(--code-bg); border: 1px solid #2a3070; border-radius: 6px; padding: 12px 14px; overflow: auto; }
pre code { border: 0; padding: 0; background: none; font-size: 13px; line-height: 1.45; }
.table-wrap { overflow-x: auto; margin: 10px 0; }
table { border-collapse: collapse; width: 100%; font-size: 14px; }
th { text-align: left; font-weight: 600; color: var(--gold); background: rgba(0,0,0,0.3); border-bottom: 1px solid var(--edge); }
th, td { padding: 6px 10px; vertical-align: top; }
tbody tr:nth-child(odd) { background: rgba(255,255,255,0.03); }
tbody tr:hover { background: rgba(240, 213, 138, 0.06); }
.tag { font: 600 10px/1 var(--sans); padding: 2px 5px; border-radius: 3px; letter-spacing: .04em; vertical-align: 1px; }
.tag-CODE { background: rgba(80, 140, 255, 0.22); color: #a9c8ff; }
.tag-DATA { background: rgba(80, 200, 140, 0.2); color: #9fe6bd; }
.tag-INFERRED { background: rgba(240, 180, 80, 0.2); color: #f3cf8e; }
footer { margin-top: 36px; text-align: center; color: var(--muted); font-size: 12px; }
"""

CRYSTAL = """<svg class="crystal" viewBox="0 0 64 80" aria-hidden="true"><defs>
<linearGradient id="cg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#e8f2ff"/><stop offset=".45" stop-color="#7fa6ff"/><stop offset="1" stop-color="#2a3a9a"/></linearGradient>
<linearGradient id="cs" x1="1" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff" stop-opacity=".9"/><stop offset="1" stop-color="#9fc0ff" stop-opacity=".1"/></linearGradient></defs>
<polygon points="32,2 54,26 32,78 10,26" fill="url(#cg)" stroke="#dfe8ff" stroke-width="1.2"/>
<polygon points="32,2 54,26 32,34" fill="url(#cs)"/><polygon points="10,26 32,34 32,78" fill="#1a2470" opacity=".45"/>
<polyline points="10,26 32,34 54,26" fill="none" stroke="#eef4ff" stroke-width="1" opacity=".8"/></svg>"""


def page(title, body, rel="."):
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title><link rel="stylesheet" href="{rel}/html/style.css"></head>
<body><div class="page">{body}
<footer>ffxi-linux: a native Linux client for FINAL FANTASY XI (local, unofficial). Built from docs/*.md by tools/build_docs.py.</footer>
</div></body></html>
"""


def git(*args):
    try:
        return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def goal_kind(status):
    """done / progress (in progress, partial, mostly done) / open (to do)."""
    s = status.lower()
    if s.startswith("done"):
        return "done"
    if "progress" in s or "partial" in s or s.startswith("mostly"):
        return "progress"
    return "open"


def chip(status):
    kind = goal_kind(status)
    word = "Done" if kind == "done" else "In progress" if kind == "progress" else "To do"
    return f'<span class="chip {kind}">{word}</span>'


def doc_page(name, md):
    body, toc = markdown(md)
    toc_html = "".join(f'<a class="h{lvl}" href="#{hid}">{text}</a>' for lvl, hid, text in toc)  # text is escaped already
    title = next((l[2:].strip() for l in md.splitlines() if l.startswith("# ")), name)
    content = f"""<div class="crumbs"><a href="../index.html">ffxi-linux</a> &rsaquo; {html.escape(name)}.md</div>
<div class="doc-layout">
<nav class="window toc"><div class="title">Contents</div><div class="toc-scroll">{toc_html or '<span class="muted">No sections</span>'}</div></nav>
<article class="window doc"><div class="title">{html.escape(name)}</div>{body}</article>
</div>"""
    return page(f"{title} · ffxi-linux", content, "..")


def status_areas(md):
    """The 'Current state, by area' table of STATUS.md."""
    rows = []
    lines = md.splitlines()
    for i, l in enumerate(lines):
        if l.startswith("| Area |"):
            for r in lines[i + 2:]:
                if not r.startswith("|"):
                    break
                rows.append(split_row(r))
            break
    return rows


def section(md, heading):
    m = re.search(rf"^## {re.escape(heading)}\s*$(.*?)(?=^## |\Z)", md, re.S | re.M)
    return m.group(1).strip() if m else ""


def plans(md):
    out = []
    for m in re.finditer(r"^### (\d+)\. (.+?)\n(.*?)(?=^### |^## |\Z)", md, re.S | re.M):
        body = m.group(3)
        status = re.search(r"- Status: (.+)", body)
        done = re.search(r"- Done when: (.+?)(?=\n- |\Z)", body, re.S)
        user = re.search(r"- User test[^:]*: (.+?)(?=\n- |\Z)", body, re.S)
        out.append((m.group(1), m.group(2), status.group(1).strip() if status else "open",
                    re.sub(r"\s+", " ", done.group(1)).strip() if done else "",
                    user.group(0)[2:].strip() if user else ""))
    return out


def build():
    OUT.mkdir(exist_ok=True)
    (OUT / "style.css").write_text(CSS.strip() + "\n")
    names = []
    for md_path in sorted(DOCS.glob("*.md")):
        name = md_path.stem
        names.append(name)
        (OUT / f"{name}.html").write_text(doc_page(name, md_path.read_text()))

    status_md = (DOCS / "STATUS.md").read_text() if (DOCS / "STATUS.md").exists() else ""
    plans_md = (DOCS / "PLANS.md").read_text() if (DOCS / "PLANS.md").exists() else ""

    # Numbers for the hero.
    commits = git("rev-list", "--count", "HEAD").strip() or "?"
    last = git("log", "-1", "--date=format:%Y-%m-%d %H:%M", "--pretty=%ad").strip()
    tests = sum(len(re.findall(r"^TEST_CASE\(", p.read_text(errors="ignore"), re.M)) for p in (ROOT / "tests").glob("*.cpp"))
    loc = 0
    for d in ("src", "apps", "shaders"):
        for p in (ROOT / d).rglob("*"):
            if p.suffix in (".cpp", ".h", ".vert", ".frag"):
                loc += sum(1 for _ in p.open(errors="ignore"))
    goals = plans(plans_md)
    done_goals = sum(1 for g in goals if g[2].lower().startswith("done"))

    stats = "".join(f'<div class="stat"><b>{v}</b><span>{k}</span></div>' for k, v in [
        ("commits", commits), ("lines of C++ / GLSL", f"{loc:,}"), ("test cases", tests), ("documents", len(names)),
        *([("goals done", f"{done_goals} / {len(goals)}")] if goals else []),
        ("last change", last.split(" ")[0] if last else "?")])

    # Status by area.
    area_cards = []
    for row in status_areas(status_md):
        if len(row) < 3:
            continue
        area, state, docs = row[0], row[1], row[2]
        kind = "done" if state.lower().startswith("done") else "open"
        docs_html = ", ".join(
            f'<a href="html/{d.strip().split(" ")[0].replace(".md", "")}.html">{html.escape(d.strip())}</a>'
            if d.strip().split(" ")[0].endswith(".md") else html.escape(d.strip()) for d in docs.split(","))
        area_cards.append(f'<div class="card"><h3>{inline(area)}</h3><span class="chip {kind}">{"Done" if kind == "done" else "Partial"}</span>'
                          f'<p>{inline(state)}</p><div class="docs">{docs_html}</div></div>')

    def plan_card(n, t, s, d, user=""):
        user_html = ""
        if user:
            body, _ = markdown(user)
            user_html = f'<div class="usertest"><b>Test for you to run</b>{body}</div>'
        return (f'<div class="card"><h3>{n}. {html.escape(t)}</h3>{chip(s)}<p>{inline(d)}</p>'
                + (f'<p class="docs">{inline(s)}</p>' if s.lower() not in ("open",) else "") + user_html + "</div>")

    plan_sections = ""
    for kind, title in (("progress", "In progress or partial"), ("open", "To do"), ("done", "Done")):
        cards = "".join(plan_card(*g) for g in goals if goal_kind(g[2]) == kind)
        plan_sections += (f'<h3 class="plan-group">{title} ({sum(1 for g in goals if goal_kind(g[2]) == kind)})</h3>'
                          f'<div class="grid">{cards or "<p class=muted>None.</p>"}</div>')

    open_html, _ = markdown(section(status_md, "Open items (not done)"))

    log_lines = git("log", "-40", "--date=short", "--pretty=%h\t%ad\t%s").strip().splitlines()
    log_html = "".join(
        f'<li><span class="hash">{html.escape(h)}</span><span class="when">{html.escape(d)}</span>{html.escape(s)}</li>'
        for h, d, s in (l.split("\t", 2) for l in log_lines if l.count("\t") >= 2))

    doc_links = "".join(
        f'<li><a href="html/{n}.html">{html.escape(n)}.md</a><br><span class="when">{html.escape(DOC_INFO.get(n, ""))}</span></li>'
        for n in sorted(names, key=lambda n: (list(DOC_INFO).index(n) if n in DOC_INFO else 99, n)))

    def doc_link(name, label):
        return f'<a href="html/{name}.html">{label}</a>' if name in names else ""

    panels = ""
    if area_cards:
        more = " &nbsp; ".join(filter(None, (doc_link("STATUS", "Full status notes &rsaquo;"),
                                              doc_link("status-report-2026-10-18", "Status report &rsaquo;"))))
        panels += (f'<section class="window"><div class="title">Status by area</div><div class="grid">{"".join(area_cards)}</div>'
                   + (f'\n<p class="docs" style="margin:12px 0 0">{more}</p>' if more else "") + "</section>\n")
    if goals:
        more = doc_link("PLANS", "Plans in full &rsaquo;")
        panels += (f'<section class="window"><div class="title">Plans</div>{plan_sections}'
                   + (f'\n<p class="docs" style="margin:12px 0 0">{more}</p>' if more else "") + "</section>\n")
    open_panel = f'<section class="window doc"><div class="title">Open items</div>{open_html}</section>\n' if open_html else ""
    recent = f'<section class="window"><div class="title">Recent work</div><ul class="cursor-list">{log_html}</ul></section>'
    work = f'<div class="columns">\n{open_panel}{recent}\n</div>' if open_panel else recent

    body = f"""<header class="hero">{CRYSTAL}<div><h1>ffxi-linux</h1>
<p>A native Linux client for FINAL FANTASY XI, written in C++20 and Vulkan, playing against a LandSandBoat server.
Status as of {html.escape(last or datetime.date.today().isoformat())}.</p></div></header>
<section class="window"><div class="title">At a glance</div><div class="stats">{stats}</div></section>
{panels}{work}
<section class="window"><div class="title">Documents</div><ul class="cursor-list columns">{doc_links}</ul></section>"""
    (DOCS / "index.html").write_text(page("ffxi-linux · status", body, "."))
    print(f"wrote docs/index.html and {len(names)} pages in docs/html/")


# ---------------------------------------------------------------------------------------------------------------
# Link check


LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)\s]+)\)")


def without_code(md):
    """The Markdown with fenced blocks and code spans blanked (keeping line numbers): links there are not links."""
    out, fenced = [], False
    for line in md.split("\n"):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            out.append("")
        else:
            out.append("" if fenced else re.sub(r"`[^`]*`", "", line))
    return out


def broken_links():
    """Each relative link in docs/*.md and the repo root's *.md whose target file or folder is not there, as
    "path:line: text". Links resolve as on a forge: relative to the Markdown file's folder. Anchors are not checked
    (this script and GitHub make different ids)."""
    bad = []
    for md_path in sorted(DOCS.glob("*.md")) + sorted(ROOT.glob("*.md")):
        for n, line in enumerate(without_code(md_path.read_text(errors="replace")), 1):
            for m in LINK_RE.finditer(line):
                target = m.group(2)
                if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith("#"):
                    continue
                path = urllib.parse.unquote(target.split("#", 1)[0])
                if path and not (md_path.parent / path).exists():
                    bad.append(f"{md_path.relative_to(ROOT)}:{n}: links to {path}, which is not there")
    return bad


if __name__ == "__main__":
    build()
    if "--check-links" in sys.argv[1:]:
        problems = broken_links()
        for p in problems:
            print(p)
        print(f"link check: {len(problems)} broken links")
        sys.exit(1 if problems else 0)
