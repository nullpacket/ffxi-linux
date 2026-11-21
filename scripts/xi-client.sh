#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Launch xi-client: from the build directory in a source tree, or the one next to this script once installed (as
# `ffxi-linux`). Arguments are passed on, e.g.
#   scripts/xi-client.sh                                  (log in from the form)
#   scripts/xi-client.sh --user someone --pass secret --char Name --autoconnect
#   scripts/xi-client.sh --char ""                        (choose the character in the game's lobby)
# The game data comes from `xi-setup import` (~/.local/share/ffxi-linux/game). Your own defaults (FFXI_ROOT to use
# another folder, LSB_DIR, XI_HOST, XI_DISPLAY, a pre-filled login in XI_ARGS) go in
# scripts/local.env (git ignores it) or ~/.config/ffxi-linux/local.env; see local.env.example.
self="$(cd "$(dirname "$0")" && pwd)"
here="$(cd "$self/.." && pwd)"
exe="$self/xi-client"
[ -x "$here/build/xi-client" ] && exe="$here/build/xi-client"
for env in "$here/scripts/local.env" "${XDG_CONFIG_HOME:-$HOME/.config}/ffxi-linux/local.env"; do
    [ -f "$env" ] && . "$env"
done
export XI_DISPLAY="${XI_DISPLAY:-secondary}" # the secondary monitor if there is one (primary, or an index, to change)
exec "$exe" --host "${XI_HOST:-127.0.0.1}" ${XI_ARGS[@]+"${XI_ARGS[@]}"} "$@"
