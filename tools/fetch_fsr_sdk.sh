#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetches the parts of AMD's FidelityFX SDK (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, MIT) the FSR 3.1
# upscaler needs (issue #139, docs/fsr.md): the host headers, the FSR 3 upscaler component, the Vulkan back end and
# its GLSL shaders. Nothing of it is kept in this repository; the SDK's licence (LICENSE.txt in the checkout) applies,
# and its notice ships with the client (THIRD-PARTY.md). CMake runs this itself when XI_FSR_SDK isn't set.
# Usage: tools/fetch_fsr_sdk.sh DIR [TAG]
set -eu
dir=${1:?usage: fetch_fsr_sdk.sh DIR [TAG]}
tag=${2:-v1.1.4}
git clone --quiet --depth 1 --branch "$tag" --filter=blob:none --sparse \
    https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git "$dir"
git -C "$dir" sparse-checkout set --no-cone '/LICENSE.txt' '/sdk/include/' '/sdk/src/shared/' \
    '/sdk/src/components/fsr3upscaler/' '/sdk/src/backends/vk/ffx_vk.cpp' '/sdk/src/backends/vk/shaders/fsr3upscaler/' \
    '/sdk/src/backends/shared/ffx_shader_blobs.*' '/sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.*'
echo "FidelityFX SDK $tag ($(git -C "$dir" rev-parse HEAD)) in $dir"
