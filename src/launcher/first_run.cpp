// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/first_run.h"

#include "xi/install.h"

#include <fmt/format.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace fs = std::filesystem;

namespace xi::launcher
{

bool haveGameData(const std::string& profileGameDir, const std::optional<fs::path>& found)
{
    if (!profileGameDir.empty())
    {
        return xi::install::gameDirProblem(profileGameDir).empty();
    }
    return found && xi::install::gameDirProblem(*found).empty();
}

std::vector<xi::install::FoundInstall> firstRunChoices(std::vector<xi::install::FoundInstall> found)
{
    // Version strings are "YYYYMMDD_N", so text order is version order.
    std::stable_sort(found.begin(), found.end(), [](const auto& x, const auto& y) {
        const bool ux = x.problem.empty(), uy = y.problem.empty();
        if (ux != uy)
        {
            return ux;
        }
        return ux && x.version > y.version;
    });
    return found;
}

namespace
{

fs::path expandHome(const std::string& typed, const fs::path& home)
{
    if (typed == "~" || typed.starts_with("~/"))
    {
        return home / typed.substr(typed.size() > 1 ? 2 : 1);
    }
    return typed;
}

} // namespace

PickedFolder resolvePickedFolder(const std::string& typed, const fs::path& home)
{
    PickedFolder r;
    if (typed.empty())
    {
        r.problem = "no folder given";
        return r;
    }
    const fs::path  p = expandHome(typed, home);
    std::error_code ec;
    if (!fs::is_directory(p, ec))
    {
        r.problem = "no such folder: " + p.string();
        return r;
    }
    const auto game = xi::install::gameFolderIn(p);
    if (!game)
    {
        r.problem = "no FINAL FANTASY XI install (FFXiMain.dll) in " + p.string() + " or its usual places under it";
        return r;
    }
    if (auto problem = xi::install::gameDirProblem(*game); !problem.empty())
    {
        r.problem = std::move(problem);
        return r;
    }
    r.dir = *game;
    return r;
}

std::string useCompressDat(const std::string& typed, const fs::path& home, const fs::path& to)
{
    if (typed.empty())
    {
        return "no file given";
    }
    return xi::install::saveCompressDat(expandHome(typed, home), to);
}

FirstRunImport::~FirstRunImport()
{
    wait();
}

bool FirstRunImport::start(const fs::path& from, const fs::path& to)
{
    {
        std::lock_guard lock(m_);
        if (state_ == State::Running)
        {
            return false;
        }
    }
    wait(); // join a finished worker before starting the next
    std::lock_guard lock(m_);
    state_    = State::Running;
    progress_ = {};
    result_.reset();
    report_.reset();
    error_.clear();
    from_   = from;
    worker_ = std::thread([this, from, to] {
        try
        {
            auto res = xi::install::importGame(from, to, [this](const xi::install::ImportProgress& p) {
                std::lock_guard l(m_);
                progress_ = p;
            });
            xi::install::VerifyOptions opt;
            opt.install         = from;
            auto            rep = xi::install::verifyGameData(to, opt);
            std::lock_guard l(m_);
            result_ = std::move(res);
            report_ = std::move(rep);
            state_  = State::Done;
        }
        catch (const std::exception& e)
        {
            std::lock_guard l(m_);
            error_ = e.what();
            state_ = State::Failed;
        }
    });
    return true;
}

void FirstRunImport::wait()
{
    if (worker_.joinable())
    {
        worker_.join();
    }
}

FirstRunImport::State FirstRunImport::state() const
{
    std::lock_guard lock(m_);
    return state_;
}

xi::install::ImportProgress FirstRunImport::progress() const
{
    std::lock_guard lock(m_);
    return progress_;
}

std::optional<xi::install::ImportResult> FirstRunImport::result() const
{
    std::lock_guard lock(m_);
    return result_;
}

std::optional<xi::install::VerifyReport> FirstRunImport::report() const
{
    std::lock_guard lock(m_);
    return report_;
}

std::string FirstRunImport::error() const
{
    std::lock_guard lock(m_);
    return error_;
}

fs::path FirstRunImport::from() const
{
    std::lock_guard lock(m_);
    return from_;
}

bool FirstRunImport::succeeded() const
{
    std::lock_guard lock(m_);
    return state_ == State::Done && report_ && !report_->broken();
}

std::string firstRunOutcome(const FirstRunImport& import)
{
    switch (import.state())
    {
        case FirstRunImport::State::Idle: return {};
        case FirstRunImport::State::Running: return "Importing...";
        case FirstRunImport::State::Failed: return "The import failed: " + import.error();
        case FirstRunImport::State::Done: break;
    }
    const auto res = import.result();
    const auto rep = import.report();
    if (!res || !rep)
    {
        return {};
    }
    const std::string copied = fmt::format("{} files copied, {} unchanged; client version {}", res->copied,
                                           res->unchanged, res->version.empty() ? "unknown" : res->version);
    if (rep->broken())
    {
        return fmt::format("The copy is incomplete ({}): {}", copied, xi::install::reportSummary(*rep));
    }
    return fmt::format("Game data imported ({}): {}", copied, xi::install::reportSummary(*rep));
}

} // namespace xi::launcher
