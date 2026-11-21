#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# One build at a time on this machine, leaving 2 cores free (user, 2026-10-22).
#   tools/build.sh BUILD_DIR [extra cmake --build args]     build
#   tools/build.sh --ctest BUILD_DIR [extra ctest args]     test
#   tools/build.sh --run CMD [ARGS...]                      any other heavy job (clang-tidy, cppcheck: CI analysis),
#                                                           with XI_JOBS set to the same nproc-2
# Every build and test (Claude, its threads, CI's tools/ci/run.sh) goes through this: it waits for the machine-wide lock
# so parallel threads and pipelines queue instead of each starting nproc jobs.
set -euo pipefail
jobs=$(( $(nproc) - 2 )); [ "$jobs" -lt 1 ] && jobs=1
# The lock lives in a persistent directory the CI runner bind-mounts into its job containers (XI_BUILD_LOCK there),
# so pipelines and local builds take turns too.
lock="${XI_BUILD_LOCK:-$HOME/.cache/ffxi-linux/build-lock/build.lock}"
mkdir -p "$(dirname "$lock")"
# flock only needs a readable fd, so a lock file made by a CI job (root inside the container) still works for the host user.
[ -e "$lock" ] || { : >"$lock" && chmod 666 "$lock"; } 2>/dev/null || true
exec 9<"$lock"
if ! flock -n 9; then echo "build.sh: waiting for another build to finish ($lock)" >&2; flock 9; fi
if [ "${1:-}" = "--run" ]; then
    shift; export XI_JOBS=$jobs
    exec nice -n 10 "$@"
fi
if [ "${1:-}" = "--ctest" ]; then
    shift; dir=${1:?usage: build.sh --ctest BUILD_DIR}; shift
    cd "$dir" && exec nice -n 10 ctest -j "$jobs" "$@"
fi
dir=${1:?usage: build.sh BUILD_DIR}; shift
exec nice -n 10 cmake --build "$dir" -j "$jobs" "$@"
