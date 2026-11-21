#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetches the parts of NVIDIA's DLSS SDK (github.com/NVIDIA/DLSS) the optional DLSS build needs: the NGX headers, the
# Linux x86-64 static library and the release DLSS runtime. Nothing of it is kept in this repository; the SDK's own
# licence (LICENSE.txt in the checkout) applies. Usage: tools/fetch_dlss_sdk.sh DIR [TAG]
# Then: cmake -B build -DXI_DLSS=ON -DXI_DLSS_SDK=DIR
set -eu
dir=${1:?usage: fetch_dlss_sdk.sh DIR [TAG]}
tag=${2:-v310.9.16}
git clone --quiet --depth 1 --branch "$tag" --filter=blob:none --sparse https://github.com/NVIDIA/DLSS.git "$dir"
git -C "$dir" sparse-checkout set --no-cone '/include/' '/lib/Linux_x86_64/libnvsdk_ngx.a' '/lib/Linux_x86_64/rel/' '/LICENSE.txt' '/doc/DLSS_Programming_Guide_Release.pdf'
echo "DLSS SDK $tag in $dir"
