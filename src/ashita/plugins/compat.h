// SPDX-License-Identifier: GPL-3.0-or-later
// Ashita plugins on Linux (issue #34, docs/ashita-compat.md "Plugins"): force-included (-include) before every
// translation unit that sees the Ashita SDK, the client's plugin host and the plugins built natively alike.
//
// The SDK is written for MSVC on 32-bit Windows. Its Win32 / COM / DirectX 8 types come from Wine's headers
// (/usr/include/wine/windows, types and macros only: nothing of Wine is linked or run); this file supplies the MSVC
// extensions the SDK and the plugins use and Wine's headers leave out, and win32.cpp the few Win32 functions plugins
// call. Plugins and client are compiled from the same headers, so their classes have the same 64-bit layout.
#pragma once
#pragma GCC system_header
// The MSVC names below are reserved identifiers on purpose: they stand in for what the SDK and the plugins call.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,cppcoreguidelines-macro-usage,modernize-macro-to-enum)

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <type_traits>

// Win32 functions are declared with default visibility, so a plugin's calls bind to the client's win32.cpp at load time
// (Wine's headers would mark them hidden).
#define DECLSPEC_IMPORT
#ifndef NOMINMAX
#define NOMINMAX // std::min / std::max, not the Windows macros
#endif
#include <windows.h> // Wine's, types and macros only

// Windows' min / max macros take any two arithmetic types (min(81, uint32_t)); std::min wins when the types agree.
template <class A, class B>
constexpr std::common_type_t<A, B> min(A a, B b)
{
    return b < a ? b : a;
}
template <class A, class B>
constexpr std::common_type_t<A, B> max(A a, B b)
{
    return a < b ? b : a;
}

#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif
#define __declspec(x) __xi_declspec_##x
#define __xi_declspec_dllexport __attribute__((visibility("default")))
#define __xi_declspec_dllimport
#define __xi_declspec_noinline __attribute__((noinline))
#define __xi_declspec_novtable
#define __xi_declspec_selectany __attribute__((weak))
#define __xi_declspec_noreturn __attribute__((noreturn))
#define __xi_declspec_align(n) __attribute__((aligned(n)))

#ifndef _TRUNCATE
#define _TRUNCATE (static_cast<size_t>(-1))
#endif

// The MSVC "secure" string functions: bounded, always terminated, 0 on success.
inline int strncpy_s(char* dst, size_t size, const char* src, size_t count)
{
    if (dst == nullptr || size == 0)
        return EINVAL;
    size_t n = src != nullptr ? strnlen(src, count == _TRUNCATE ? size - 1 : count) : 0;
    if (n >= size)
        n = size - 1;
    if (n > 0)
        memcpy(dst, src, n);
    dst[n] = '\0';
    return 0;
}
template <size_t N>
inline int strncpy_s(char (&dst)[N], const char* src, size_t count)
{
    return strncpy_s(dst, N, src, count);
}
inline int strcpy_s(char* dst, size_t size, const char* src)
{
    return strncpy_s(dst, size, src, _TRUNCATE);
}
template <size_t N>
inline int strcpy_s(char (&dst)[N], const char* src)
{
    return strcpy_s(dst, N, src);
}
inline int strcat_s(char* dst, size_t size, const char* src)
{
    const size_t have = dst != nullptr ? strnlen(dst, size) : size;
    return have < size ? strcpy_s(dst + have, size - have, src) : EINVAL;
}
template <size_t N>
inline int strcat_s(char (&dst)[N], const char* src)
{
    return strcat_s(dst, N, src);
}
// The plugins' own formats, as MSVC runs them; bounded by the buffer size they pass.
#define sprintf_s(buf, size, ...) snprintf((buf), (size), __VA_ARGS__)                    // Flawfinder: ignore
#define vsprintf_s(buf, size, fmt, args) vsnprintf((buf), (size), (fmt), (args))          // Flawfinder: ignore
#define _snprintf_s(buf, size, count, ...) snprintf((buf), (size), __VA_ARGS__)           // Flawfinder: ignore
#define _vsnprintf_s(buf, size, count, fmt, args) vsnprintf((buf), (size), (fmt), (args)) // Flawfinder: ignore
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define stricmp strcasecmp
#define strnicmp strncasecmp
inline int fopen_s(FILE** f, const char* name, const char* mode)
{
    *f = fopen(name, mode);
    return *f != nullptr ? 0 : errno;
}
inline int _itoa_s(int value, char* buf, size_t size, int radix)
{
    if (buf == nullptr || size == 0)
        return EINVAL;
    const auto r = std::to_chars(buf, buf + size - 1, value, radix);
    *r.ptr       = '\0';
    return r.ec == std::errc{} ? 0 : ERANGE;
}

// Structured exceptions do not exist here: the SDK's ScopedTranslator becomes a no-op.
typedef void (*_se_translator_function)(unsigned int, struct _EXCEPTION_POINTERS*);
inline _se_translator_function _set_se_translator(_se_translator_function f)
{
    return f;
}
#ifndef EXCEPTION_POSSIBLE_DEADLOCK
#define EXCEPTION_POSSIBLE_DEADLOCK 0xC0000194L
#endif
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,cppcoreguidelines-macro-usage,modernize-macro-to-enum)
