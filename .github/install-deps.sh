#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs the build and analysis tools in the GitHub Actions job's Arch Linux container (.github/workflows/ci.yml).
# The same packages as ci/Dockerfile, the image the project's own CI runs in, so the compiler, the libraries and the
# analysis tools' versions match the baselines in ci/analysis/baseline/.
#   .github/install-deps.sh [--semgrep]
set -euo pipefail
pacman -Syu --noconfirm --needed \
    base-devel git cmake ninja ccache pkgconf python \
    vulkan-headers vulkan-icd-loader shaderc glslang \
    sdl3 ffmpeg openssl curl zlib \
    fmt spdlog nlohmann-json glm catch2 \
    fontconfig ttf-dejavu \
    clang cppcheck shellcheck ruff gitleaks flawfinder python-pip
# Vulkan Memory Allocator (header only; not in Arch's repos): the version the project builds with.
curl -fsSL -o /usr/include/vk_mem_alloc.h \
    https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h
if [ "${1:-}" = --semgrep ]; then
    # Semgrep is not packaged in Arch: a pinned copy in its own venv, metrics and version checks off.
    python -m venv /opt/semgrep
    /opt/semgrep/bin/pip install --no-cache-dir semgrep==1.180.0
    ln -sf /opt/semgrep/bin/semgrep /usr/local/bin/semgrep
fi
