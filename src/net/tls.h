// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/socket.h"

#include <memory>
#include <string>

namespace xi::net
{

// Minimal TLS client for the LSB auth port. The server presents a self-signed certificate whose CN
// is an IP address, so peer verification is off unless a pinned certificate file is supplied.
class TlsStream
{
public:
    static TlsStream connect(const Endpoint& ep, const std::string& pinnedCertPath = {});

    TlsStream(TlsStream&&) noexcept;
    TlsStream& operator=(TlsStream&&) noexcept;
    ~TlsStream();

    void        write(std::span<const std::byte> data);
    std::string readSome(std::chrono::milliseconds timeout = 10s);

private:
    TlsStream() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xi::net
