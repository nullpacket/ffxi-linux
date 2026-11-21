#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Render 4K desktop wallpapers from zone scenes with xi-viewer and stamp the ffxi-linux logo on each.
#   tools/make_wallpapers.sh [OUT_DIR]   (default ~/Pictures/Wallpapers/ffxi-linux)
# The window is clamped by the desktop panel, so each render is cropped to 16:9 and scaled to 3840x2160.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="${1:-$HOME/Pictures/Wallpapers/ffxi-linux}"
export FFXI_ROOT="${FFXI_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/ffxi-linux/game}" # xi-setup import
export XI_DISPLAY="${XI_DISPLAY:-secondary}" XI_NO_FOCUS=1
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

# name  zone  time  camera (X,Y,Z,YAW,PITCH; empty = viewer default)
SCENES=(
  "heavens-tower       239 17:30 0,-30,-120,0,10"
  "windurst-waters     238 17:30 -"
  "port-windurst       240 06:30 -"
  "southern-sandoria   230 18:00 -"
  "northern-sandoria   231 16:30 -"
  "la-theine-plateau   102 10:00 -"
  "tahrongi-canyon     117 06:30 -"
  "buburimu-peninsula  118 16:30 -"
)

# Watermark: logo with a soft shadow so it reads on light scenes too.
rsvg-convert -w 960 docs/branding/logo.svg -o "$TMP/logo.png"
magick "$TMP/logo.png" \( +clone -background black -shadow 75x14+0+4 \) +swap -background none -layers merge +repage "$TMP/mark.png"

for s in "${SCENES[@]}"; do
  read -r name zone time cam <<<"$s"
  camArgs=()
  [[ "$cam" != "-" ]] && camArgs=(--cam "$cam")
  ./build/xi-viewer --size 3840x2160 --frames 30 --time "$time" "${camArgs[@]}" --screenshot "$TMP/raw.png" zone "$zone" >/dev/null 2>&1
  read -r w h < <(magick identify -format '%w %h\n' "$TMP/raw.png")
  cw=$(( h * 16 / 9 < w ? h * 16 / 9 : w ))
  ch=$(( cw * 9 / 16 ))
  magick "$TMP/raw.png" -gravity center -crop "${cw}x${ch}+0+0" +repage -resize '3840x2160!' \
    -attenuate 0.35 +noise Uniform \
    "$TMP/mark.png" -gravity southeast -geometry +90+80 -compose over -composite \
    "$OUT/$name.png"
  echo "$OUT/$name.png"
done
