// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "xi/game_data.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The launcher's first-run import (#135): when it starts (or a profile is about to connect) with no usable game data,
// it offers the installs found on this machine (detectInstalls), or a folder the user picks, imports one into the
// client's data folder with progress, and shows the file check. The logic lives here so tests can drive it with fake
// trees; apps/xi-launcher/main.cpp only draws it.
namespace xi::launcher
{

// Whether there is game data the client can use: the profile's own game folder when it sets one (the client gets it
// as --root), else `found` (findGameDir(): FFXI_ROOT, the imported copy, ~/.wine). A folder counts when
// gameDirProblem() has nothing to say about it.
bool haveGameData(const std::string& profileGameDir, const std::optional<std::filesystem::path>& found);

// The installs to offer, usable ones first, newest client version first among them; the rest keep detectInstalls()'s
// order (they are shown greyed out with their problem).
std::vector<xi::install::FoundInstall> firstRunChoices(std::vector<xi::install::FoundInstall> found);

// The FINAL FANTASY XI folder for a folder the user typed or picked: the folder itself, or a Wine prefix / drive
// holding one (gameFolderIn), with "~" expanded against `home`. `problem` says what is wrong when nothing usable is
// there.
struct PickedFolder
{
    std::optional<std::filesystem::path> dir;
    std::string                          problem;
};
PickedFolder resolvePickedFolder(const std::string& typed, const std::filesystem::path& home);

// A compress.dat the user typed or picked (#136): "~" expanded against `home`, then checked and copied to `to`
// (saveCompressDat; the launcher passes dataCompressDat()). Empty on success, else what is wrong.
std::string useCompressDat(const std::string& typed, const std::filesystem::path& home,
                           const std::filesystem::path& to);

// One import running on a worker thread: importGame() from `from` into `to`, then verifyGameData() against `from`.
class FirstRunImport
{
public:
    enum class State
    {
        Idle,
        Running,
        Done,   // copied and checked; report() says whether the copy is usable
        Failed, // importGame threw: error()
    };

    FirstRunImport() = default;
    ~FirstRunImport();
    FirstRunImport(const FirstRunImport&)            = delete;
    FirstRunImport& operator=(const FirstRunImport&) = delete;

    // Starts the import unless one is running; returns false if one is.
    bool start(const std::filesystem::path& from, const std::filesystem::path& to);
    // Waits for a running import (tests, and the launcher on exit).
    void wait();

    State                                    state() const;
    xi::install::ImportProgress              progress() const;
    std::optional<xi::install::ImportResult> result() const;
    std::optional<xi::install::VerifyReport> report() const;
    std::string                              error() const;
    std::filesystem::path                    from() const;

    // The copy is there and nothing required is missing or damaged (Outdated / Stale do not count).
    bool succeeded() const;

private:
    mutable std::mutex                       m_;
    std::thread                              worker_;
    State                                    state_ = State::Idle;
    xi::install::ImportProgress              progress_;
    std::optional<xi::install::ImportResult> result_;
    std::optional<xi::install::VerifyReport> report_;
    std::string                              error_;
    std::filesystem::path                    from_;
};

// The headline for a finished import: what was copied and the check's summary, or what went wrong.
std::string firstRunOutcome(const FirstRunImport& import);

} // namespace xi::launcher
