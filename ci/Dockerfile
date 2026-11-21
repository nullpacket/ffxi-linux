# CI build image for ffxi-linux (goal 40; docs/ci.md "The build image").
# Arch Linux, like the CachyOS dev machine, so the compiler and libraries match what developers build with.
# Rebuild: docker build -t ffxi-linux-ci:2 ci/      (bump the tag when this file changes, here and wherever CI names
# the image)
FROM archlinux:base@sha256:52dfbd5afb23a220d5ee92dafcb4c788d96c026ed04179890c23adef4c897fa0
RUN pacman -Syu --noconfirm --needed \
        base-devel git cmake ninja ccache pkgconf python \
        vulkan-headers vulkan-icd-loader shaderc glslang \
        sdl3 ffmpeg openssl curl zlib \
        fmt spdlog nlohmann-json glm catch2 \
        fontconfig ttf-dejavu \
        clang cppcheck shellcheck ruff gitleaks flawfinder python-pip \
    && pacman -Scc --noconfirm \
    && rm -rf /var/cache/pacman/pkg/*
# Semgrep (not packaged in Arch) in its own venv, pinned; it only runs the repo's own rules (ci/analysis/semgrep.yml)
# with metrics and version checks off, so jobs need no network.
RUN python -m venv /opt/semgrep && /opt/semgrep/bin/pip install --no-cache-dir semgrep==1.180.0 \
    && ln -s /opt/semgrep/bin/semgrep /usr/local/bin/semgrep
# Vulkan Memory Allocator (header only; CachyOS packages it, Arch's repos do not): the same 3.4.0 as the dev machine.
RUN curl -fsSL -o /usr/include/vk_mem_alloc.h \
        https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h
# Where the runner mounts the shared data (config.toml volumes): XDG_DATA_HOME holds ffxi-linux/game (read-only) and
# ffxi-linux/dlss-sdk (read-only); /ci/cache is the persistent build / ccache volume; /ci/lock is the host's
# ~/.cache/ffxi-linux/build-lock, so tools/build.sh's lock is shared with builds outside CI.
ENV XDG_DATA_HOME=/ci/share \
    XI_CI_CACHE=/ci/cache \
    XI_DLSS_SDK=/ci/share/ffxi-linux/dlss-sdk \
    XI_BUILD_LOCK=/ci/lock/build.lock \
    SEMGREP_ENABLE_VERSION_CHECK=0 \
    SEMGREP_SEND_METRICS=off
RUN mkdir -p /ci/share /ci/lock /ci/cache /root/.local/share /root/.cache \
    && ln -s /ci/share/ffxi-linux /root/.local/share/ffxi-linux \
    && ln -s /ci/cache /root/.cache/ffxi-ci
# (The two links keep branches cut before the docker runner working: their run.sh looks under $HOME.)
