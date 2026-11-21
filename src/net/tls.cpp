// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/tls.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <poll.h>
#include <stdexcept>

#include <fmt/format.h>

namespace xi::net
{

namespace
{

std::string sslError()
{
    char buf[256] = {};
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return buf;
}

} // namespace

struct TlsStream::Impl
{
    TcpStream tcp;
    SSL_CTX*  ctx = nullptr;
    SSL*      ssl = nullptr;

    ~Impl()
    {
        if (ssl)
        {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (ctx)
        {
            SSL_CTX_free(ctx);
        }
    }

    // Drives a non-blocking SSL call until it completes, waiting on the socket as OpenSSL asks.
    template <typename F>
    int drive(F&& op, std::chrono::milliseconds timeout)
    {
        for (;;)
        {
            const int rc = op();
            if (rc > 0)
            {
                return rc;
            }
            const int err = SSL_get_error(ssl, rc);
            short     ev  = 0;
            if (err == SSL_ERROR_WANT_READ)
            {
                ev = POLLIN;
            }
            else if (err == SSL_ERROR_WANT_WRITE)
            {
                ev = POLLOUT;
            }
            else if (err == SSL_ERROR_ZERO_RETURN)
            {
                return 0;
            }
            else
            {
                throw std::runtime_error(fmt::format("TLS error {}: {}", err, sslError()));
            }
            pollfd pfd{ .fd = tcp.fd(), .events = ev, .revents = 0 };
            if (::poll(&pfd, 1, static_cast<int>(timeout.count())) <= 0)
            {
                throw std::runtime_error("TLS: timed out");
            }
        }
    }
};

TlsStream::TlsStream(TlsStream&&) noexcept            = default;
TlsStream& TlsStream::operator=(TlsStream&&) noexcept = default;
TlsStream::~TlsStream()                               = default;

TlsStream TlsStream::connect(const Endpoint& ep, const std::string& pinnedCertPath)
{
    TlsStream s;
    s.impl_      = std::make_unique<Impl>();
    auto& impl   = *s.impl_;
    impl.tcp     = TcpStream::connect(ep);
    impl.ctx     = SSL_CTX_new(TLS_client_method());
    if (!impl.ctx)
    {
        throw std::runtime_error("SSL_CTX_new: " + sslError());
    }
    SSL_CTX_set_min_proto_version(impl.ctx, TLS1_2_VERSION);
    if (!pinnedCertPath.empty())
    {
        if (SSL_CTX_load_verify_locations(impl.ctx, pinnedCertPath.c_str(), nullptr) != 1)
        {
            throw std::runtime_error(fmt::format("load pinned cert {}: {}", pinnedCertPath, sslError()));
        }
        SSL_CTX_set_verify(impl.ctx, SSL_VERIFY_PEER, nullptr);
    }
    else
    {
        SSL_CTX_set_verify(impl.ctx, SSL_VERIFY_NONE, nullptr);
    }

    impl.ssl = SSL_new(impl.ctx);
    SSL_set_fd(impl.ssl, impl.tcp.fd());
    impl.drive([&] { return SSL_connect(impl.ssl); }, 10s);
    return s;
}

void TlsStream::write(std::span<const std::byte> data)
{
    impl_->drive([&] { return SSL_write(impl_->ssl, data.data(), static_cast<int>(data.size())); }, 10s);
}

std::string TlsStream::readSome(std::chrono::milliseconds timeout)
{
    std::string out(4096, '\0');
    const int   n = impl_->drive([&] { return SSL_read(impl_->ssl, out.data(), static_cast<int>(out.size())); }, timeout);
    out.resize(static_cast<std::size_t>(n));
    return out;
}

} // namespace xi::net
