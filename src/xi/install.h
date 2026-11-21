// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Finding what the client needs outside this repo: the user's FINAL FANTASY XI install and LSB's compress.dat.
// Used by the apps for defaults and for clear first-run messages (README "First run").
namespace xi::install
{

// Where we look for the game when FFXI_ROOT and --root are not given: our own imported copy (`xi-setup import`,
// game_data.h), then the default Wine prefix.
std::vector<std::filesystem::path> defaultGameDirs();

// FFXI_ROOT if set, else the first of defaultGameDirs() that holds FFXiMain.dll.
std::optional<std::filesystem::path> findGameDir();

// findGameDir() as a C string for code that used getenv("FFXI_ROOT") (tests, tools): nullptr when there is none.
// The pointer stays valid for the program's life.
const char* gameRootOrNull();

// Empty if `dir` looks like a usable install, else what is wrong with it (one line, for the user).
std::string gameDirProblem(const std::filesystem::path& dir);

// The newest client version the install has been patched to ("30260604_0"), from the version entries in its
// patch.cfg (INFERRED: the PlayOnline updater lists every patched file by version). LSB compares it with
// login.CLIENT_VER.
std::optional<std::string> installedClientVersion(const std::filesystem::path& dir);

// The client's own copy of LSB's compress.dat, $XDG_DATA_HOME/ffxi-linux/compress.dat (else
// ~/.local/share/ffxi-linux/compress.dat), next to the imported game data. `xi-setup compress-dat FILE` and the
// launcher (Tools, first run) put a chosen file there, so it is found without LSB_DIR or a server checkout (#136).
std::filesystem::path dataCompressDat();

// Where we look for LSB's res/compress.dat, in order: XI_COMPRESS_DAT, $LSB_DIR/res, our own copy
// (dataCompressDat()), then ~/server, ~/git/server, ~/LandSandBoat, ~/git/LandSandBoat (each /res/compress.dat).
std::vector<std::filesystem::path> compressDatCandidates();

// Empty if `path` holds a compress table the client can use (2048 bytes: 256 codes then 256 lengths, a complete
// prefix code, as xi::Huffman::fromCompressDat loads it), else one line saying what is wrong.
std::string compressDatProblem(const std::filesystem::path& path);

// The first candidate that exists and is usable.
std::optional<std::filesystem::path> findCompressDat();

// What `xi-setup check` and the launcher report.
struct CompressDatStatus
{
    std::optional<std::filesystem::path> found;   // the file the client will use, if any
    std::vector<std::filesystem::path>   tried;   // every candidate, in order
    std::vector<std::string>             skipped; // "<path>: <problem>" for candidates that exist but are unusable
};
CompressDatStatus compressDatStatus();

// One line for the user: "<path>" when found, else how to fix it (and what was skipped).
std::string describeCompressDat(const CompressDatStatus& s);

// Checks `from` (compressDatProblem) and copies it to `to` (default dataCompressDat()), replacing any earlier copy.
// Empty on success, else the error. The file is LandSandBoat's (GPL-3.0); it is copied only on the user's machine,
// never shipped (THIRD-PARTY.md).
std::string saveCompressDat(const std::filesystem::path& from, const std::filesystem::path& to = dataCompressDat());

// Where we look for the LSB checkout itself: $LSB_DIR, then ~/server, ~/git/server, ~/LandSandBoat,
// ~/git/LandSandBoat. findLsbDir() is the first that holds settings/default/login.lua.
std::vector<std::filesystem::path>   lsbDirCandidates();
std::optional<std::filesystem::path> findLsbDir();

// The client version an LSB checkout targets: CLIENT_VER in its settings/default/login.lua ("30261006_0"). Its zone
// message ids (scripts/zones/*/IDs.lua) are numbered for that version's dialog DATs, so older game data shows the
// wrong lines for the server's system messages (issue #58: "Obtained 218 gil." for an item). A server's own
// settings/login.lua may lower CLIENT_VER so older data can still log in; that does not renumber the messages, so
// this reads the default file.
std::optional<std::string> lsbTargetClientVersion(const std::filesystem::path& lsbDir);

// A one-line warning when the game data (`installed`) is older than what LSB targets (`target`), else "".
std::string dataVersionWarning(const std::string& installed, const std::string& target);

} // namespace xi::install
