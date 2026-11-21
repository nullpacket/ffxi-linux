# CI: the pipeline, the analysis and the build image

Every push runs the same checks, analysis, builds and tests, in four stages. A stage starts only when every job of the
stage before passed. **The analysis gates the build:** a commit with a leaked secret, a forbidden file, a lint error or
a new analysis finding is never built, and the slow jobs only run for commits the fast ones accepted. The job bodies
are `tools/ci/run.sh` and `tools/ci/analysis.py`, and every job runs in the image built from `ci/Dockerfile`.

## The stages

| Stage | Job | What |
|---|---|---|
| check (seconds) | `check:secrets` | gitleaks on the commits since the base (see "Analysis") + the forbidden-file check on the whole tree; **any finding fails** |
| | `check:lint` | shellcheck on `*.sh`, ruff (`ruff.toml`) on the Python tools, and the `SPDX-License-Identifier: GPL-3.0-or-later` line in every own source file (`spdx.missing`) |
| | `check:format` | clang-format (`.clang-format`) on the C++ lines changed since the base; **advisory** (never blocks; the code predates the file and clang-format re-aligns neighbouring lines) |
| analysis (minutes) | `analysis:sast` | the repo's semgrep rules (`ci/analysis/semgrep.yml`) + flawfinder (level 4+) on src/ apps/ tests/ |
| | `analysis:clang-tidy` | `.clang-tidy` (bugprone incl. the CERT aliases, performance, concurrency, the clang static analyzer) on every repo TU; needs only a configure (+ LuaJIT's generated header), no build |
| | `analysis:cppcheck` | cppcheck (warning, performance, portability) on the same TUs |
| build | `build-test:default` | configure (Ninja, RelWithDebInfo, XI_WERROR=ON, ccache), build, ctest (must print `100% tests passed`; tests that need the game data, an Ashita install or a Vulkan driver skip without them) |
| | `build-test:asan` | the same with XI_SANITIZE=ON (AddressSanitizer + UBSan, no recovery; warnings not fatal, the others check them): a memory error or undefined behaviour fails the test that hit it (leak checking is off) |
| docs | `docs` | `python3 tools/build_docs.py --check-links` (the built pages are an artifact; they are not committed; a link to a missing page or file fails) |

A warm build of a branch takes about 1.5 minutes per configuration.

## Analysis

The check and analysis jobs run `tools/ci/analysis.py TOOL` (through `tools/ci/run.sh analysis TOOL`). Each prints a
summary line first (`=== TOOL: N findings, M new, K fixed since the baseline`), then the new findings as `path:line:
[check] message`. Every finding also goes into a Code Quality report (`ci-out/codequality-TOOL.json`), which a GitLab
merge request page shows as new / fixed findings; with `--github` they become workflow annotations and a step summary
instead.

- **Baseline.** `ci/analysis/baseline/TOOL.json` counts the findings that existed when the tool was added, keyed by
  check, file and message (no line numbers, so moving code does not make a finding new). A job fails when a key has
  more findings than its baseline allows. When findings are fixed the job says so; shrink the baseline then.
- **Base commit** (secrets, format): the merge or pull request's base, else on main the push's previous tip (or the
  merge's first parent), else the branch's merge base with main (`$XI_CI_BASE` sets it).
- **Heavy jobs** (clang-tidy, cppcheck) configure the branch's default build directory (the one build-test:default
  then builds in) and build only LuaJIT (its `luajit.h` is generated), then run through `tools/build.sh --run`, so they
  wait for the machine-wide build lock and use `nproc - 2` workers. clang-tidy keeps its results in a cache shared by
  all branches, keyed by the compile command and the contents of the TU and every repo header it includes (found by
  following its `#include "..."` lines), so a branch pipeline re-analyses only the TUs its changes reach and takes the
  rest from the cache; on main every TU is analysed again (a full run is about 3.5 minutes) and the cache refreshed. A
  TU that does not parse is always a finding (`clang-diagnostic-error`).
- **Run one locally** (needs the tool installed, or run it in the CI image): `python3 tools/ci/analysis.py lint`;
  for clang-tidy / cppcheck build first and pass the build: `tools/build.sh --run python3 tools/ci/analysis.py
  clang-tidy --build-dir build`. In the CI image, from the repo: `docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v
  "$PWD:$PWD" -w "$PWD" ffxi-linux-ci:2 python3 tools/ci/analysis.py sast` (a worktree needs its main repo mounted too).
  `secrets --full-history` scans every commit instead of the push.
- **A finding that is not a bug:** suppress it on the line, with the reason, rather than growing the baseline:
  clang-tidy `// NOLINT(check-name): reason`, cppcheck `// cppcheck-suppress checkId`, semgrep `// nosemgrep: rule-id`,
  flawfinder `// flawfinder: ignore`, shellcheck `# shellcheck disable=SCnnnn`, ruff `# noqa: CODE`. A check that is
  wrong for the whole code base goes out of `.clang-tidy` / `ruff.toml` / the cppcheck command line in analysis.py.
- **Update a baseline** (after fixing findings, or to accept many at once): `tools/build.sh --run python3
  tools/ci/analysis.py clang-tidy --build-dir build --update-baseline` (likewise cppcheck, sast, lint), then commit
  `ci/analysis/baseline/`. Run it with the CI image's tool versions (above): another clang-tidy or cppcheck version
  reports different findings.
- **Forbidden files** (`check:secrets`): game data and captures (.dat, .pcap, ROM*/), images and textures, audio and
  video, binaries and libraries (.so, .dll, .exe, nvngx* ...), archives, fonts, keys and credential files, anything
  binary or over 2 MB, and `addons/` / `libs/` directories (third-party). `ci/analysis/allowed-files.txt` lists the
  exceptions (our own logo); add to it only for our own work.
- **Secrets**: gitleaks' default rules plus the GitLab token formats (`.gitleaks.toml`). A secret that was pushed is
  out: revoke it first (a new token), then remove it from the branch.
- **Format**: fix with `git clang-format BASE` (the job prints the command and the diff, `ci-out/format.diff`).

## The build image

`ci/Dockerfile` (Arch Linux, pinned by digest: gcc, cmake, ninja, ccache, Vulkan headers / loader / Memory Allocator,
shaderc (glslc), glslang, SDL3, FFmpeg, OpenSSL, curl, zlib, fmt, spdlog, nlohmann-json, glm, Catch2, python,
fontconfig + DejaVu for the gdifonts tests), plus clang (clang-tidy, clang-format), cppcheck, shellcheck, ruff,
gitleaks, flawfinder and semgrep (pinned, in /opt/semgrep, run with metrics and version checks off). Build it with
`docker build -t ffxi-linux-ci:2 ci/` (bump the tag for incompatible changes). No test opens a Vulkan device (the ImGui
contexts in the Ashita tests are headless), so jobs need no GPU.

## Build parallelism

Every build and test run (CI and local) goes through `tools/build.sh BUILD_DIR` / `tools/build.sh --ctest
BUILD_DIR`: one build at a time on the whole machine (a `flock` on ~/.cache/ffxi-linux/build-lock/build.lock, which
CI job containers can mount, so pipelines and local builds take turns; others wait), at `nproc - 2` jobs and `nice
10`, so 2 cores always stay free. Use it instead of calling `cmake --build` / `ninja` / `ctest -j` directly.

## Generated HTML docs

`docs/index.html` and `docs/html/` are generated by `python3 tools/build_docs.py` and are not committed (they embed
the git log and would conflict on every merge). Run the script locally to read them in a browser; CI's `docs` job
checks that they build, with `--check-links` (every link between the docs, and to repo files, must lead somewhere),
and keeps them as an artifact. Never `git add` them.
