// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/process.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <map>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace xi::launcher
{

namespace
{

// environ plus `env` (an empty value removes the variable), as a vector of "K=V" strings.
std::vector<std::string> mergedEnv(const std::vector<std::pair<std::string, std::string>>& env)
{
    std::map<std::string, std::string> vars;
    for (char** e = environ; e && *e; ++e)
    {
        const std::string kv = *e;
        const auto        eq = kv.find('=');
        if (eq != std::string::npos)
        {
            vars[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
    }
    for (const auto& [k, v] : env)
    {
        if (v.empty())
        {
            vars.erase(k);
        }
        else
        {
            vars[k] = v;
        }
    }
    std::vector<std::string> out;
    out.reserve(vars.size());
    for (const auto& [k, v] : vars)
    {
        out.push_back(k + "=" + v);
    }
    return out;
}

std::vector<char*> cstrs(std::vector<std::string>& v)
{
    std::vector<char*> out;
    out.reserve(v.size() + 1);
    for (auto& s : v)
    {
        out.push_back(s.data());
    }
    out.push_back(nullptr);
    return out;
}

} // namespace

Process::~Process()
{
    if (pid_ > 0 && !code_)
    {
        stop(1000);
    }
    if (fd_ >= 0)
    {
        ::close(fd_);
    }
}

bool Process::start(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& env,
                    const std::string& logFile)
{
    error_.clear();
    code_.reset();
    partial_.clear();
    if (argv.empty())
    {
        error_ = "nothing to run";
        return false;
    }
    int fds[2] = { -1, -1 }; // read, write
    if (!logFile.empty())
    {
        fds[1] = ::open(logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        fds[0] = fds[1] >= 0 ? ::open(logFile.c_str(), O_RDONLY | O_CLOEXEC) : -1;
        if (fds[0] < 0)
        {
            error_ = logFile + ": " + std::strerror(errno);
            if (fds[1] >= 0)
            {
                ::close(fds[1]);
            }
            return false;
        }
    }
    else if (::pipe2(fds, O_CLOEXEC) != 0)
    {
        error_ = std::strerror(errno);
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP); // its own group: stop() reaches its children too
    posix_spawnattr_setpgroup(&attr, 0);
    auto args = argv;
    auto envs = mergedEnv(env);
    auto av   = cstrs(args);
    auto ev   = cstrs(envs);
    const int rc = argv[0].find('/') == std::string::npos ? posix_spawnp(&pid_, av[0], &fa, &attr, av.data(), ev.data())
                                                          : posix_spawn(&pid_, av[0], &fa, &attr, av.data(), ev.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(fds[1]);
    if (rc != 0)
    {
        ::close(fds[0]);
        pid_   = -1;
        error_ = std::string("cannot start ") + argv[0] + ": " + std::strerror(rc);
        return false;
    }
    if (fd_ >= 0)
    {
        ::close(fd_);
    }
    fd_   = fds[0];
    file_ = !logFile.empty();
    ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);
    return true;
}

std::vector<std::string> Process::poll()
{
    std::vector<std::string> lines;
    if (fd_ < 0)
    {
        return lines;
    }
    char buf[4096];
    for (;;)
    {
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0)
        {
            partial_.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0 && file_ && running()) // a log file: its end for now, more may come
        {
            break;
        }
        if (n == 0) // the writer side is closed (for a log file: the program ended): no more output
        {
            ::close(fd_);
            fd_ = -1;
        }
        break;
    }
    std::size_t start = 0;
    for (std::size_t nl; (nl = partial_.find('\n', start)) != std::string::npos; start = nl + 1)
    {
        lines.push_back(partial_.substr(start, nl - start));
    }
    partial_.erase(0, start);
    if (fd_ < 0 && !partial_.empty())
    {
        lines.push_back(std::move(partial_));
        partial_.clear();
    }
    return lines;
}

void Process::reap(bool wait)
{
    if (pid_ <= 0 || code_)
    {
        return;
    }
    int        status = 0;
    const auto r      = ::waitpid(pid_, &status, wait ? 0 : WNOHANG);
    if (r == pid_)
    {
        code_ = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    }
}

bool Process::running()
{
    reap(false);
    return pid_ > 0 && !code_;
}

std::optional<int> Process::exitCode()
{
    reap(false);
    return code_;
}

void Process::stop(int graceMs)
{
    if (!running())
    {
        return;
    }
    ::kill(-pid_, SIGTERM);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
    while (running() && std::chrono::steady_clock::now() < until)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (running())
    {
        ::kill(-pid_, SIGKILL);
        reap(true);
    }
}

void Process::detach()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
    pid_  = -1;
    file_ = false;
    code_.reset();
}

std::pair<int, std::string> runCapture(const std::vector<std::string>& argv, const std::string& input, int timeoutMs)
{
    int inPipe[2], outPipe[2];
    if (::pipe2(inPipe, O_CLOEXEC) != 0)
    {
        return { -1, {} };
    }
    if (::pipe2(outPipe, O_CLOEXEC) != 0)
    {
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        return { -1, {} };
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    auto  args = argv;
    auto  av   = cstrs(args);
    pid_t pid  = -1;
    const int rc = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(inPipe[0]);
    ::close(outPipe[1]);
    if (rc != 0)
    {
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        return { -1, {} };
    }
    if (!input.empty())
    {
        [[maybe_unused]] const auto w = ::write(inPipe[1], input.data(), input.size());
    }
    ::close(inPipe[1]);
    std::string out;
    char        buf[4096];
    const auto  until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
        pollfd     p{ outPipe[0], POLLIN, 0 };
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
        if (left <= 0 || ::poll(&p, 1, static_cast<int>(left)) <= 0)
        {
            ::kill(pid, SIGKILL);
            break;
        }
        const ssize_t n = ::read(outPipe[0], buf, sizeof(buf));
        if (n <= 0)
        {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(outPipe[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return { WIFEXITED(status) ? WEXITSTATUS(status) : -1, out };
}

bool Keyring::available()
{
    // `secret-tool` present (libsecret's CLI); whether the keyring is unlocked shows on first use.
    return runCapture({ "sh", "-c", "command -v secret-tool" }).first == 0;
}

bool Keyring::store(const std::string& host, const std::string& user, const std::string& password)
{
    // secret-tool reads the secret from stdin, so it never appears in a process listing.
    return runCapture({ "secret-tool", "store", "--label=ffxi-linux " + user + "@" + host, "application", "ffxi-linux", "host", host,
                        "user", user },
                      password, 20000)
               .first == 0;
}

std::optional<std::string> Keyring::lookup(const std::string& host, const std::string& user)
{
    const auto [code, out] = runCapture({ "secret-tool", "lookup", "application", "ffxi-linux", "host", host, "user", user }, {}, 20000);
    if (code != 0 || out.empty())
    {
        return std::nullopt;
    }
    return out;
}

bool Keyring::clear(const std::string& host, const std::string& user)
{
    return runCapture({ "secret-tool", "clear", "application", "ffxi-linux", "host", host, "user", user }, {}, 20000).first == 0;
}

} // namespace xi::launcher
