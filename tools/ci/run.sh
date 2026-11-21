#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# CI job body for the local GitLab runner (docker executor, image ci/Dockerfile; docs/ci.md).
#   tools/ci/run.sh build-test CONFIG   CONFIG = default | dlss | asan (AddressSanitizer + UBSan) | public (no
#                                       private-only features)
#   tools/ci/run.sh analysis TOOL       TOOL = clang-tidy | cppcheck | sast | lint | format | secrets (#109)
#   tools/ci/run.sh docs
# Builds in a persistent per-branch, per-config directory with ccache, runs ctest against the imported game data
# ($XDG_DATA_HOME/ffxi-linux/game, FFXI_ROOT unset) and, on failure, prints a short summary first: the first compiler
# errors and the failing tests. Full logs and the ctest JUnit XML go to ci-out/ (job artifacts).
set -uo pipefail
mode=${1:?usage: run.sh build-test CONFIG | analysis TOOL | release | docs}
cfg=${2:-default}
src=$(pwd)
out="$src/ci-out"
jobs=$(( $(nproc) - 2 )); [ "$jobs" -lt 1 ] && jobs=1 # leave 2 cores free for the desktop (user, 2026-10-22)
mkdir -p "$out"
unset FFXI_ROOT
cache="${XI_CI_CACHE:-$HOME/.cache/ffxi-ci}" # /ci/cache in the CI image (a persistent volume)
export CCACHE_DIR="${CCACHE_DIR:-$cache/ccache}"

summary_build() {
    echo "=== first errors (full log: ci-out/build-$cfg.log) ==="
    grep -E "error:|Error |undefined reference|ninja: build stopped|CMake Error" "$out/build-$cfg.log" | head -n 20
}
summary_test() {
    echo "=== failing tests (full log: ci-out/ctest-$cfg.log, JUnit: ci-out/ctest-$cfg.xml) ==="
    grep -E "\*\*\*Failed|Failed  |Exception|FAILED:|REQUIRE|CHECK\(" "$out/ctest-$cfg.log" | head -n 30
    grep -E "tests passed" "$out/ctest-$cfg.log"
}

# configure CONFIG: sets bdir and runs cmake in the persistent per-branch, per-config build directory. The analysis
# jobs configure the same directory as build-test:default (a configure is enough for compile_commands.json), so the
# build after them finds it configured.
configure() {
    local c=$1 tree=${2:-$src}
    bdir="$cache/${CI_COMMIT_REF_SLUG:-local}/$c"
    local deps="$cache/deps-$c"
    mkdir -p "$bdir" "$deps"
    local extra=()
    if [ "$c" = dlss ]; then
        extra=(-DXI_DLSS=ON "-DXI_DLSS_SDK=${XI_DLSS_SDK:-$HOME/.local/share/ffxi-linux/dlss-sdk}")
    elif [ "$c" = public ]; then
        # The public release's build: the private-only features (/bugreport) left out.
        extra=(-DXI_PRIVATE_FEATURES=OFF)
    elif [ "$c" = asan ]; then
        # Warnings are the other configurations' job: gcc's sanitizer instrumentation adds false
        # -Wmaybe-uninitialized / -Wconversion warnings inside libstdc++.
        extra=(-DXI_SANITIZE=ON -DXI_WERROR=OFF)
    fi
    cmake -S "$tree" -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXI_WERROR=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        "-DFETCHCONTENT_BASE_DIR=$deps" "${extra[@]}"
}

case "$mode" in
build-test)
    if [ "$cfg" = asan ]; then
        # Any sanitizer report fails the test that triggered it. Leaks are not checked: the client keeps some
        # process-lifetime allocations (LuaJIT, Vulkan loader) on purpose.
        export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1"
        export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
    fi
    echo "=== configure + build ($cfg) in $cache/${CI_COMMIT_REF_SLUG:-local}/$cfg ==="
    if ! { configure "$cfg" && "$src/tools/build.sh" "$bdir"; } > "$out/build-$cfg.log" 2>&1; then
        summary_build
        exit 1
    fi
    tail -n 1 "$out/build-$cfg.log"
    echo "=== ctest ($cfg), game data from the import ==="
    "$src/tools/build.sh" --ctest "$bdir" --output-on-failure --output-junit "$out/ctest-$cfg.xml" > "$out/ctest-$cfg.log" 2>&1
    rc=$?
    if [ $rc -ne 0 ] || ! grep -q "100% tests passed" "$out/ctest-$cfg.log"; then
        summary_test
        exit 1
    fi
    grep -E "tests passed" "$out/ctest-$cfg.log"
    ;;
analysis)
    tool=${2:?usage: run.sh analysis TOOL}
    case "$tool" in
    clang-tidy|cppcheck)
        # They read the compile database of a configure (no build: the analysis gates the build); then they take the
        # build lock and nproc-2 workers like a build.
        # LuaJIT's luajit.h is made by its own build: build that dependency (once per branch, then it is up to date).
        echo "=== configure (default) for compile_commands.json, LuaJIT's generated headers ==="
        if ! { configure default && "$src/tools/build.sh" "$bdir" --target xi_luajit_build; } > "$out/configure.log" 2>&1; then
            tail -n 30 "$out/configure.log"
            exit 1
        fi
        export XI_TIDY_CACHE="$cache/clang-tidy-results"
        "$src/tools/build.sh" --run python3 "$src/tools/ci/analysis.py" "$tool" --build-dir "$bdir" ;;
    *)
        python3 "$src/tools/ci/analysis.py" "$tool" ;;
    esac
    ;;
docs)
    python3 tools/build_docs.py --check-links > "$out/docs.log" 2>&1 || { tail -n 20 "$out/docs.log"; exit 1; }
    tail -n 1 "$out/docs.log"
    ;;
*)
    echo "unknown mode $mode"
    exit 2
    ;;
esac
