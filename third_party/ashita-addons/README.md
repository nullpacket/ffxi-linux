# Bundled Ashita addons

The only Ashita addons ffxi-linux ships (issue #138): **the standard Ashita v4 addons and their shared libraries**
(`addons/`, `addons/libs/`), from the git-tracked `addons/` folder of
[Ashita-v4beta](https://github.com/AshitaXI/Ashita-v4beta) at the commit named on the first line of `MANIFEST.sha256`:
44 addons and the libraries. Text files are stored with LF line endings; nothing else is changed.

dynamic_entity_renamer (HorizonXI's) is not bundled: the client shows those names itself (docs/protocol-map.md,
"Entity names").

Left out of the upstream set, and rejected by the release export if they come back (user, 2026-11-07: no addon
that crashes the client, and none that does nothing in it):

| Addon | Why |
|---|---|
| hideconsole | Windows-only: hides Ashita's bootloader console, which the client doesn't have |
| ime | Windows-only: the Windows IME |
| affinity | Windows-only: Win32 process CPU affinity |
| freemem | Windows-only: SetProcessWorkingSetSize, a no-op here |
| autologin | built into the client: --char / --autoconnect and the launcher's profiles |
| timestamp | built into the client: the native /timestamp and Config's chat timestamps |
| renamer | crashes the client: calls game functions it scanned for |
| watchdog | crashes the client: calls game functions it scanned for |
| filterscan | crashes the client: calls game functions it scanned for |
| ahcolors | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| ahgo | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| cartographer | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| fastswap | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| filterless | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| instantah | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| mapdot | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| mipmap | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| nomad | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| noname | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| paranormal | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| quicksets | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| trimspells | does nothing: finds no stand-in for the game code it patches ("Failed to locate ... pointer") |
| chime | does nothing: fails to load (the game's chime sounds are not mapped) |
| casper | does nothing: sets entity render flags, which the client does not read |
| chamcham | does nothing: writes model colours into actor memory the client does not have |
| truesight | does nothing: sets entity render flags, which the client does not read |
| skeletonkey | does nothing: writes an entity's status field, which the client does not read |
| cleancs | does nothing: the font / primitive / ImGui managers' SetVisible is a no-op here |
| hideui | does nothing: the font / primitive / ImGui managers' SetVisible is a no-op here |
| chatmon | does nothing: its feature is sounds, and addon sounds are not played |
| chatfix | harmful: shifts LSB's chat packets by a byte for an old server layout, cutting messages |
| instantchat | does nothing: the client's chat log has no delay to remove |
| macrofix | does nothing: the client's macro palette has no delay to remove |
| enternity | does nothing: edits log lines, but the dialogue box's wait comes from the event, not the log |
| minimapmon | does nothing: controls the Minimap plugin, which the client does not have |
| move | does nothing: moves and resizes the window through Win32 calls |
| dynamic_entity_renamer | built into the client: dynamic-entity renames (LSB's spawn-name layout and HorizonXI's 0x1FF list) are handled natively |
| hideobs | Windows-only: hides the window from OBS through SetWindowDisplayAffinity |

Also left out: editor settings (`.vscode`), Windows binaries (`libs/socket/ssl.dll`) and archives. The manifest's
comment lines repeat the list (`tools/vendor_ashita_addons.py`, `EXCLUDED`). How each was decided, and the live run
in the windowed client, are in docs/ashita-compat.md ("Live in the windowed client: the bundled set").

Copyright the Ashita Development Team and the addons' authors (named in each addon's header). Ashita is released under
the GNU GPL v3 ([LICENSE-GPLv3.md](LICENSE-GPLv3.md)).

The client copies this set into the game folder's `addons/` on start, adding only files that are not there yet, so it
never overwrites an addon or a library the user already has. An addon the user deletes is not added back.
`xi-setup addons` does the same on demand. Only the addons listed in `MANIFEST.sha256` may be bundled: the CI's
forbidden-file check and the release export reject any other addon, and any change to these files that is not in the
manifest.

Updating: `python3 tools/vendor_ashita_addons.py update --ashita PATH_TO_ASHITA_V4BETA_CLONE`, then `python3 tools/vendor_ashita_addons.py check`.
