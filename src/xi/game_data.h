// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The client's own copy of the game data (goal 49): `xi-setup import` copies only the files the client and tools read
// from a FINAL FANTASY XI install (a Wine prefix, a mounted Windows drive, any folder) into the user's data dir, so
// nothing needs the original install afterwards. Square Enix's files are never shipped; each user imports their own.
namespace xi::install
{

// $XDG_DATA_HOME/ffxi-linux/game (else ~/.local/share/ffxi-linux/game): where the import goes and the first place
// findGameDir() looks after FFXI_ROOT.
std::filesystem::path dataGameDir();

// Whether a path relative to the FINAL FANTASY XI folder is one the client reads:
//   VTABLE.DAT, FTABLE.DAT              the DAT index (ROM)
//   ROM/, ROM2/ ... ROMn/               DATs, with their own VTABLEn / FTABLEn
//   sound/, sound2/ ... soundn/         sound effects (.spw) and music (.bgw)
//   FFXiMain.dll                        tables read from the game code (dll_data, the Ashita scan registry)
//   patch.cfg                           the client version LSB checks
//   USER/<id>/mcr*                      the game's macro files (first fill of our macro folder, goal 35)
// Never: mov/ (FMV), Tools*/, TEMP/, SYS/, patch.txt / file.txt, the other DLLs, executables, cursors, dgVoodoo files.
bool neededGameFile(const std::filesystem::path& rel);

struct FoundInstall
{
    std::filesystem::path dir;     // the FINAL FANTASY XI folder
    std::string           version; // from patch.cfg, "" if none
    std::string           problem; // "" if it looks usable (gameDirProblem)
    std::string source; // where it was found: "Steam", "Lutris: <game>", "Faugus: <game>", "Bottles: <bottle>",
                        // "Wine (~/.wine)", "Wine prefix under ~/Games", "mounted drive"
};

// Where detectInstalls() looks. fromEnvironment() fills it from HOME, XDG_DATA_HOME, XDG_CONFIG_HOME and USER; tests
// point it at a fake tree.
struct DetectPlaces
{
    std::filesystem::path              home;
    std::filesystem::path              dataHome;   // $XDG_DATA_HOME, else ~/.local/share
    std::filesystem::path              configHome; // $XDG_CONFIG_HOME, else ~/.config
    std::vector<std::filesystem::path> mountBases; // folders whose entries are mounted drives (/mnt, /media ...)

    static DetectPlaces fromEnvironment();
};

// Installs found on this machine, each once (the first source that finds it names it):
//   Steam    app 230330 in every library listed in steamapps/libraryfolders.vdf (native, ~/.steam and Flatpak
//            Steam): its Proton prefix (compatdata/230330) and its own copy (common/FFXINA)
//   Lutris   the `prefix:` (and the exe's folder) of every game in lutris/games/*.yml (data and config dirs)
//   Faugus   the `prefix` (and `path`'s folder) of every game in faugus-launcher/games.json
//   Bottles  every bottle (native and Flatpak bottles/bottles/*)
//   Wine     ~/.wine, and Wine prefixes one or two levels under ~/Games
//   drives   every folder under the mount bases (/mnt, /media, /media/$USER, /run/media/$USER): Windows drives
// Our own data dir (dataHome/ffxi-linux/game) is never listed.
std::vector<FoundInstall> detectInstalls(const DetectPlaces& places);
std::vector<FoundInstall> detectInstalls();

// The library folders in a Steam libraryfolders.vdf ("path" values), in file order; empty if unreadable.
std::vector<std::filesystem::path> steamLibraryFolders(const std::filesystem::path& vdf);

struct NamedPrefix
{
    std::string           name;   // the game's or bottle's name
    std::filesystem::path prefix; // a Wine prefix, or a folder that may hold the game itself
};

// Lutris games (*.yml in `gamesDir`): `name:` and every `prefix:` / `exe:` folder; "~" expands to `home`.
std::vector<NamedPrefix> lutrisPrefixes(const std::filesystem::path& gamesDir, const std::filesystem::path& home);
// Faugus Launcher games (games.json): `title`, `prefix` and the folder of `path`.
std::vector<NamedPrefix> faugusPrefixes(const std::filesystem::path& gamesJson);
// Bottles (each folder of `bottlesDir`, named by the folder).
std::vector<NamedPrefix> bottlesPrefixes(const std::filesystem::path& bottlesDir);

// The FINAL FANTASY XI folder inside `from`: `from` itself, or a Wine prefix / drive root holding it. Under a drive
// it tries the usual PlayOnline paths, then any `Program Files*/<A>[/<B>]/SquareEnix/FINAL FANTASY XI` (private-server
// launchers such as HorizonXI's `Program Files/HorizonXI/Game/...`).
std::optional<std::filesystem::path> gameFolderIn(const std::filesystem::path& from);

struct ImportProgress
{
    std::uint64_t filesDone = 0, filesTotal = 0;
    std::uint64_t bytesDone = 0, bytesTotal = 0;
    std::string   current; // relative path being copied
};

struct ImportResult
{
    std::uint64_t copied = 0, unchanged = 0, removed = 0, kept = 0; // kept: USER macro files never overwritten
    std::uint64_t bytes  = 0;                                       // bytes of the copied files
    bool          reflink = false;                                  // at least one file was a reflink (no extra space)
    std::string   version;                                          // the imported client version
};

// Copy the needed files from the install `from` (the FINAL FANTASY XI folder) into `to`. Re-running copies only files
// whose size or modification time changed (after a game patch) and removes files the install no longer has. Files are
// reflinked where the filesystem allows (FICLONE: btrfs, XFS; no extra space), else copied. USER macro files are
// copied only when missing, so macros edited in our copy are never overwritten. Writes `to`/import.manifest and
// `to`/import.version. Throws on errors.
ImportResult importGame(const std::filesystem::path& from, const std::filesystem::path& to,
                        const std::function<void(const ImportProgress&)>& progress = {});

// Checking a copy of the game data (after an import, and in `xi-setup check`).
enum class FileState
{
    Missing,        // not in the copy (required ones stop the client)
    SizeDiffers,    // the copy's size is not what the import recorded, or not the install's
    ContentDiffers, // same size, different bytes than the install's file
    Outdated,       // the install's file changed since the import (a game patch): import again
    Stale,          // the copy has it, the install no longer does: import again removes it
};

struct FileIssue
{
    std::string rel; // path relative to the FINAL FANTASY XI folder ("ROM/" for the folder itself)
    FileState   state    = FileState::Missing;
    bool        required = false; // the client cannot start without it (requiredGameFile)
};

struct VerifyOptions
{
    // The install to compare with; if empty, the one recorded in `dir`/import.version (when it still exists).
    std::optional<std::filesystem::path> install;
    // Compare every file's bytes with the install's (slow: reads both copies). Without it only the top-level files
    // (VTABLE.DAT / FTABLE.DAT / FFXiMain.dll / patch.cfg) are compared byte for byte; the rest by size.
    bool compareContents = false;
};

struct VerifyReport
{
    std::filesystem::path                dir;
    std::optional<std::filesystem::path> install;          // what it was compared with, if anything
    bool                                 manifest = false; // `dir` is an import (has import.manifest)
    bool                                 contents = false; // every file's bytes were compared
    std::uint64_t                        checked  = 0;     // files looked at
    std::vector<FileIssue>               issues;

    std::size_t count(FileState s) const;
    std::size_t requiredMissing() const;
    // Something is missing or wrong in the copy (Missing / SizeDiffers / ContentDiffers): exit non-zero. Outdated and
    // Stale only mean the install moved on since the import.
    bool broken() const;
};

// Files the client cannot start without: FFXiMain.dll, VTABLE.DAT, FTABLE.DAT, and each ROMn folder's VTABLEn.DAT /
// FTABLEn.DAT (n >= 2).
bool requiredGameFile(const std::filesystem::path& rel);

VerifyReport verifyGameData(const std::filesystem::path& dir, const VerifyOptions& options = {});

// The report as lines for a terminal: what was compared, up to `perState` files of each state, and the summary.
std::vector<std::string> formatReport(const VerifyReport& report, std::size_t perState = 15);
// Just the summary ("13042 files checked: all ok" / "13042 files checked: 3 missing (1 required), 2 out of date").
std::string reportSummary(const VerifyReport& report);

} // namespace xi::install
