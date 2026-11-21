// SPDX-License-Identifier: GPL-3.0-or-later
// Ashita plugins on Linux (issue #34): linked into every natively built plugin (not the client). Its operator new hands
// out zeroed memory: plugins written for MSVC leave members uninitialized and still work there (Shorthand's
// SettingsHelper never sets its Document pointer before testing it), because a fresh Windows heap is mostly zero. The
// replacement binds inside the plugin only (it is linked with -Bsymbolic, cmake/ashita_plugins.cmake).
#include <cstdlib>
#include <new>

void* operator new(std::size_t n)
{
    if (void* p = std::calloc(1, n == 0 ? 1 : n))
    {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n)
{
    return ::operator new(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    return std::calloc(1, n == 0 ? 1 : n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    return std::calloc(1, n == 0 ? 1 : n);
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete[](void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}
