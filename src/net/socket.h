// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xi::net
{

using namespace std::chrono_literals;

struct Endpoint
{
    uint32_t addr = 0; // IPv4, network byte order (as in in_addr.s_addr)
    uint16_t port = 0; // host byte order

    static Endpoint resolve(const std::string& host, uint16_t port);
    [[nodiscard]] std::string toString() const;
};

// RAII file descriptor.
class Fd
{
public:
    Fd() = default;
    explicit Fd(int fd)
    : fd_(fd)
    {
    }
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&)            = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd();

    [[nodiscard]] int get() const
    {
        return fd_;
    }
    [[nodiscard]] bool valid() const
    {
        return fd_ >= 0;
    }
    void reset();

private:
    int fd_ = -1;
};

// Blocking-with-timeout TCP stream. Used for the lobby sockets, which are strictly request/response.
class TcpStream
{
public:
    static TcpStream connect(const Endpoint& ep, std::chrono::milliseconds timeout = 5s);

    // Writes the whole buffer with one send() call where possible (the LSB login server treats
    // each read as one message, so a request must not be split or coalesced).
    void sendAll(std::span<const std::byte> data);

    // Reads exactly data.size() bytes or throws on timeout/EOF.
    void recvExact(std::span<std::byte> data, std::chrono::milliseconds timeout = 10s);

    // Returns true if the peer closed the connection within the timeout.
    bool waitForClose(std::chrono::milliseconds timeout);

    [[nodiscard]] int fd() const
    {
        return fd_.get();
    }
    void close()
    {
        fd_.reset();
    }

private:
    Fd fd_;
};

// Non-blocking UDP socket for the map server.
class UdpSocket
{
public:
    UdpSocket();

    void sendTo(const Endpoint& ep, std::span<const std::byte> data);

    // Waits up to timeout for one datagram. Returns its size (0 if none arrived).
    std::size_t recvFrom(std::span<std::byte> buf, Endpoint& from, std::chrono::milliseconds timeout);

    // Bytes sent / received so far (the network window's S / R rates).
    [[nodiscard]] uint64_t bytesSent() const
    {
        return sent_;
    }
    [[nodiscard]] uint64_t bytesReceived() const
    {
        return received_;
    }

private:
    Fd       fd_;
    uint64_t sent_     = 0;
    uint64_t received_ = 0;
};

} // namespace xi::net
