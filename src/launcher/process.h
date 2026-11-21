// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

// A child process for xi-launcher (goal 44): the client, xi-setup, xi-dat or a shell line, its stdout and stderr
// merged into one pipe read without blocking, so the launcher can show the output live.
namespace xi::launcher
{

class Process
{
public:
    Process() = default;
    ~Process();
    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

    // Starts argv[0] (looked up in PATH when it has no '/') with `env` added to the launcher's environment (an empty
    // value removes the variable). With `logFile` its output goes to that file (read back by poll()), so the program
    // keeps running unharmed after detach() and the launcher's exit; else to a pipe. False with error() when it cannot
    // start.
    bool start(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& env = {},
               const std::string& logFile = {});
    // The complete lines of output that arrived since the last call (split at '\n'; the last piece once it ends).
    std::vector<std::string> poll();
    [[nodiscard]] bool running();
    // The exit code once it ended (128 + signal when killed), else nullopt.
    [[nodiscard]] std::optional<int> exitCode();
    // SIGTERM, then SIGKILL after `graceMs` if it is still running.
    void stop(int graceMs = 3000);
    // Lets it run on its own: no longer read or stopped by this object.
    void detach();
    [[nodiscard]] const std::string& error() const
    {
        return error_;
    }
    [[nodiscard]] pid_t pid() const
    {
        return pid_;
    }

private:
    void reap(bool wait);

    pid_t              pid_ = -1;
    int                fd_  = -1;
    bool               file_ = false; // fd_ reads a log file (its EOF is not the end while the program runs)
    std::string        partial_;
    std::optional<int> code_;
    std::string        error_;
};

// Runs a program to completion with `input` on its stdin; returns {exit code, stdout}. -1 when it cannot run.
std::pair<int, std::string> runCapture(const std::vector<std::string>& argv, const std::string& input = {}, int timeoutMs = 10000);

// The desktop keyring (Secret Service, through `secret-tool`): passwords for the launcher's profiles, keyed by host
// and account. Every call returns false / nullopt when there is no keyring or it is locked; nothing else is tried.
struct Keyring
{
    [[nodiscard]] static bool available();
    static bool store(const std::string& host, const std::string& user, const std::string& password);
    [[nodiscard]] static std::optional<std::string> lookup(const std::string& host, const std::string& user);
    static bool clear(const std::string& host, const std::string& user);
};

} // namespace xi::launcher
