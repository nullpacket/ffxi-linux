// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/md5.h"

#include <openssl/evp.h>

#include <stdexcept>

namespace xi
{

Md5Digest md5(std::span<const std::byte> data)
{
    Md5Digest    out{};
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), reinterpret_cast<unsigned char*>(out.data()), &len, EVP_md5(), nullptr) != 1 || len != out.size())
    {
        throw std::runtime_error("EVP_Digest(md5) failed");
    }
    return out;
}

} // namespace xi
