# Third-party software

ffxi-linux is licensed under the GNU General Public License, version 3 or (at your option) any later version
(`GPL-3.0-or-later`; the full text is in [LICENSE](LICENSE)). Every source file of the project's own carries an
`SPDX-License-Identifier: GPL-3.0-or-later` line.

The repository contains one set of third-party code, the bundled Ashita addons (below). Everything else is fetched
by CMake at configure time, installed from the distribution's packages, or supplied by the user. The versions are the ones pinned in `CMakeLists.txt`,
`ci/Dockerfile` and `.github/install-deps.sh`. Binary packages built from this tree must ship this file.

The game itself is not included and is not covered by this license. FINAL FANTASY XI and its data are the property of
SQUARE ENIX; the client reads them from the user's own installed copy.

## Bundled in the repository

| Component | Version | How it is used | License | Source |
|---|---|---|---|---|
| Ashita v4 addons and Lua libraries (44 addons and `addons/libs`) | Ashita-v4beta commit `877d9e76b65b0361706f9e2be3423a713f63d3c5` | `third_party/ashita-addons/`, installed into the game folder's `addons/` (share/ffxi-linux/ashita-addons when installed) | GPL-3.0 (Ashita's `LICENSE-GPLv3.md`, beside them) | https://github.com/AshitaXI/Ashita-v4beta |

The set is listed file by file, with hashes, in `third_party/ashita-addons/MANIFEST.sha256`; its README says what was
left out and why. No other third-party addon may be committed (the CI's forbidden-file check).

## Linked into the programs

| Component | Version | How it is used | License | Source |
|---|---|---|---|---|
| Dear ImGui | v1.92.9 | FetchContent, static | MIT | https://github.com/ocornut/imgui |
| LuaJIT | commit `95f931cb1ee7ab14f249d61fb3734803429025f8` | FetchContent, static | MIT | https://github.com/LuaJIT/LuaJIT |
| stb | commit `a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d` | FetchContent, headers | MIT or public domain (Unlicense), at the user's choice | https://github.com/nothings/stb |
| LuaSocket | v3.1.0 | FetchContent, static | MIT | https://github.com/lunarmodules/luasocket |
| LuaSec | v1.3.2 | FetchContent, static | MIT | https://github.com/lunarmodules/luasec |
| AMD FidelityFX SDK (FSR 3.1 upscaler, Vulkan back end) | v1.1.4, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` | fetched at configure time (`cmake/fsr.cmake`, `tools/fetch_fsr_sdk.sh`), static; its shaders compiled by `tools/fsr_shaders.py`; `XI_FSR=OFF` leaves it out | MIT (the notice is installed as `share/doc/ffxi-linux/FidelityFX-SDK-LICENSE.txt`) | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK |
| Vulkan Memory Allocator | v3.4.0 (`vk_mem_alloc.h`) | header, installed by the build environment | MIT | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator |
| SDL3 | distribution package | shared library | zlib | https://libsdl.org |
| Vulkan loader and headers | distribution package | shared library, headers | Apache-2.0 | https://github.com/KhronosGroup |
| fmt | distribution package | library | MIT | https://github.com/fmtlib/fmt |
| spdlog | distribution package | library | MIT | https://github.com/gabime/spdlog |
| nlohmann/json | 3.11 or newer, distribution package | headers | MIT | https://github.com/nlohmann/json |
| GLM | distribution package | headers | MIT (or the Happy Bunny License) | https://github.com/g-truc/glm |
| OpenSSL | distribution package | shared library | Apache-2.0 (3.x) | https://www.openssl.org |
| zlib | distribution package | shared library | zlib | https://zlib.net |
| FFmpeg libavcodec, libavutil | distribution package | shared library, dynamic only | LGPL-2.1-or-later (as packaged; see below) | https://ffmpeg.org |
| libcurl | distribution package | shared library, private builds only (`XI_PRIVATE_FEATURES`) | curl (MIT-style) | https://curl.se |

Dear ImGui's built-in font (ProggyClean) comes with ImGui under its own free license. The launcher and the Ashita
layer use the system's fonts (DejaVu Sans, Noto Sans, or whatever fontconfig finds) at run time; none are bundled.

## Build and test tools only (not part of the programs)

| Component | Version | License |
|---|---|---|
| CMake, Ninja, ccache, pkgconf | distribution packages | BSD-3-Clause, Apache-2.0, GPL-3.0-or-later, ISC |
| shaderc / glslang (shader compiler) | distribution packages | Apache-2.0, BSD-3-Clause and others |
| Catch2 | v3, distribution package | BSL-1.0 |
| clang, clang-tidy, clang-format | distribution packages | Apache-2.0 WITH LLVM-exception |
| cppcheck | distribution package | GPL-3.0-or-later |
| shellcheck | distribution package | GPL-3.0-or-later |
| ruff | distribution package | MIT |
| flawfinder | distribution package | GPL-2.0-or-later |
| gitleaks | distribution package | MIT |
| semgrep | 1.180.0 (pip) | LGPL-2.1 |
| Arch Linux base image | pinned digest in `ci/Dockerfile` | various (distribution) |
| actions/checkout | pinned commit in `.github/workflows/ci.yml` | MIT |

## Optional, supplied by the user, never bundled

| Component | How it is used | License |
|---|---|---|
| NVIDIA DLSS SDK | only with `-DXI_DLSS=ON`; fetched by the user (`tools/fetch_dlss_sdk.sh`) | NVIDIA's RTX SDKs license (the checkout's `LICENSE.txt`), accepted by the user |
| Ashita v4 plugin SDK headers | only for plugin support; copied from the user's Ashita install by `xi-setup addons` and prepared in the build folder | LGPL-3.0-or-later (Ashita's `LICENSE.md`); includes Microsoft's DirectX 8 headers, see "Needs review" |
| Ashita plugin sources (Shorthand and others) | built from the user's own checkout; never committed | each plugin's own license (`docs/ashita-compat.md`, "Plugins") |
| Wine headers | build time only, for plugin support | LGPL-2.1-or-later |
| Other Ashita Lua addons | put in the game folder's `addons/` by the user, or copied from the user's Ashita install (`xi-setup addons FOLDER --addon NAME`) | each addon's own license |
| LandSandBoat `res/compress.dat` | read at run time from the user's LSB checkout, or from a copy the user makes on their own machine (`xi-setup compress-dat FILE` / the launcher, into `~/.local/share/ffxi-linux/`); never shipped | GPL-3.0 (LandSandBoat) |

## Needs review before a public release

These are points for the project owner to check; they are not settled here.

1. **DLSS builds can't be distributed under the GPL as they stand.** With `XI_DLSS=ON` the client statically links
   NVIDIA's proprietary `libnvsdk_ngx.a`, and `install()` copies `libnvidia-ngx-dlss.so.*` next to the programs.
   Binary releases and packages must be built with `XI_DLSS=OFF`. Before DLSS support is published as source, decide
   whether to add a GPL linking exception for the NGX libraries, keep DLSS out of the public tree, or leave it as a
   build-it-yourself option with that limit documented.
2. **Ashita plugin SDK headers.** Ashita's `LICENSE.md` licenses the SDK (`plugins/sdk/`) under the LGPL-3.0, and
   its headers say "version 3 of the License, or (at your option) any later version". `tools/ashita_sdk_port.py`
   modifies a copy of the SDK in the build folder. The LGPL asks that modifications be released, and the port script
   that makes them is public source here. Confirm this reading. Also confirm that plugins built against the SDK (Shorthand
   and others, each under its own license) may be loaded by a GPL-3.0-or-later program.
3. **Microsoft DirectX 8 headers inside the Ashita SDK.** The SDK ships Microsoft's `d3d8.h` family
   (`plugins/sdk/d3d8/includes/`, "Copyright (C) Microsoft Corporation. All Rights Reserved."), and a build with
   plugin support compiles against them. They come from the user's own Ashita install and are never committed or
   bundled. The public CI and release packages build without plugin support (no Wine headers or SDK), so no binary we
   publish contains them. Decide whether plugin-enabled binaries may ever be distributed, or whether plugin support
   stays build-it-yourself.
4. **FFmpeg.** Distribution packages are LGPL-2.1-or-later, or GPL-2.0-or-later when built with `--enable-gpl`;
   either is fine for a GPL-3.0-or-later program linking dynamically. A package must not bundle a static FFmpeg, or
   one built with `--enable-nonfree`.
5. **OpenSSL.** OpenSSL 3.x (Apache-2.0) is compatible with GPL-3.0. An OpenSSL 1.x system library has the old
   OpenSSL/SSLeay license, which is not. Packages should depend on OpenSSL 3.
6. **The Blowfish tables** (`src/xi/blowfish_table.inc`) are the standard hexadecimal digits of pi, generated by
   `tools/gen_blowfish_table.py`. They are not copyrightable and need no notice.

