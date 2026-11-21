// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <fmt/format.h>

namespace xi::net
{

namespace
{

[[noreturn]] void throwErrno(const char* what)
{
    throw std::runtime_error(fmt::format("{}: {}", what, std::strerror(errno)));
}

sockaddr_in toSockaddr(const Endpoint& ep)
{
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = ep.addr;
    sa.sin_port        = htons(ep.port);
    return sa;
}

// Returns false on timeout.
bool pollFor(int fd, short events, std::chrono::milliseconds timeout)
{
    pollfd pfd{ .fd = fd, .events = events, .revents = 0 };
    for (;;)
    {
        const int rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        if (rc < 0 && errno == EINTR)
        {
            continue;
        }
        if (rc < 0)
        {
            throwErrno("poll");
        }
        return rc > 0;
    }
}

} // namespace

Endpoint Endpoint::resolve(const std::string& host, uint16_t port)
{
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res     = nullptr;
    if (const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res); rc != 0)
    {
        throw std::runtime_error(fmt::format("resolve {}: {}", host, gai_strerror(rc)));
    }
    Endpoint ep{ reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr, port };
    ::freeaddrinfo(res);
    return ep;
}

std::string Endpoint::toString() const
{
    char buf[INET_ADDRSTRLEN] = {};
    in_addr a{ addr };
    ::inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return fmt::format("{}:{}", buf, port);
}

Fd::Fd(Fd&& other) noexcept
: fd_(std::exchange(other.fd_, -1))
{
}

Fd& Fd::operator=(Fd&& other) noexcept
{
    if (this != &other)
    {
        reset();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

Fd::~Fd()
{
    reset();
}

void Fd::reset()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

TcpStream TcpStream::connect(const Endpoint& ep, std::chrono::milliseconds timeout)
{
    TcpStream s;
    s.fd_ = Fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!s.fd_.valid())
    {
        throwErrno("socket");
    }

    const int one = 1;
    ::setsockopt(s.fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    const auto sa = toSockaddr(ep);
    if (::connect(s.fd(), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) < 0 && errno != EINPROGRESS)
    {
        throwErrno(fmt::format("connect {}", ep.toString()).c_str());
    }
    if (!pollFor(s.fd(), POLLOUT, timeout))
    {
        throw std::runtime_error(fmt::format("connect {}: timed out", ep.toString()));
    }
    int       err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(s.fd(), SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0)
    {
        throw std::runtime_error(fmt::format("connect {}: {}", ep.toString(), std::strerror(err)));
    }
    return s;
}

void TcpStream::sendAll(std::span<const std::byte> data)
{
    while (!data.empty())
    {
        if (!pollFor(fd(), POLLOUT, 5s))
        {
            throw std::runtime_error("send: timed out");
        }
        const ssize_t n = ::send(fd(), data.data(), data.size(), MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
            {
                continue;
            }
            throwErrno("send");
        }
        data = data.subspan(static_cast<std::size_t>(n));
    }
}

void TcpStream::recvExact(std::span<std::byte> data, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!data.empty())
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0 || !pollFor(fd(), POLLIN, left))
        {
            throw std::runtime_error("recv: timed out");
        }
        const ssize_t n = ::recv(fd(), data.data(), data.size(), 0);
        if (n == 0)
        {
            throw std::runtime_error("recv: connection closed by peer");
        }
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
            {
                continue;
            }
            throwErrno("recv");
        }
        data = data.subspan(static_cast<std::size_t>(n));
    }
}

bool TcpStream::waitForClose(std::chrono::milliseconds timeout)
{
    std::byte scratch[256];
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0 || !pollFor(fd(), POLLIN, left))
        {
            return false;
        }
        const ssize_t n = ::recv(fd(), scratch, sizeof(scratch), 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
        {
            return true;
        }
    }
}

UdpSocket::UdpSocket()
: fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0))
{
    if (!fd_.valid())
    {
        throwErrno("socket(udp)");
    }
}

void UdpSocket::sendTo(const Endpoint& ep, std::span<const std::byte> data)
{
    const auto sa = toSockaddr(ep);
    if (::sendto(fd_.get(), data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) < 0)
    {
        throwErrno("sendto");
    }
    sent_ += data.size();
}

std::size_t UdpSocket::recvFrom(std::span<std::byte> buf, Endpoint& from, std::chrono::milliseconds timeout)
{
    if (!pollFor(fd_.get(), POLLIN, timeout))
    {
        return 0;
    }
    sockaddr_in   sa{};
    socklen_t     len = sizeof(sa);
    const ssize_t n   = ::recvfrom(fd_.get(), buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&sa), &len);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EINTR)
        {
            return 0;
        }
        throwErrno("recvfrom");
    }
    from = Endpoint{ sa.sin_addr.s_addr, ntohs(sa.sin_port) };
    received_ += static_cast<uint64_t>(n);
    return static_cast<std::size_t>(n);
}

} // namespace xi::net
