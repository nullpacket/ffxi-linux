// SPDX-License-Identifier: GPL-3.0-or-later
// Force-included into the FidelityFX SDK's sources (issue #139; cmake/fsr.cmake): the few MSVC library functions the
// SDK's FSR 3 upscaler and Vulkan back end call, which glibc doesn't have. The SDK itself is fetched unchanged
// (tools/fetch_fsr_sdk.sh).
#pragma once

#ifdef __cplusplus
#include <bit>     // std::popcount (ffx_util.h)
#include <codecvt> // std::wstring_convert (ffx_vk.cpp's Linux path)
#include <cmath>   // log2 / floor (ffx_vk.cpp)
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <locale>
#include <string>

#ifndef _countof
template <typename T, std::size_t N>
constexpr std::size_t xiFfxCountof(T (&)[N]) noexcept
{
    return N;
}
#define _countof(a) static_cast<int32_t>(xiFfxCountof(a))
#endif

// wcscpy_s / strcpy_s with the array overloads the SDK uses (the destination's size is the array's).
template <std::size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src)
{
    std::wcsncpy(dst, src, N - 1);
    dst[N - 1] = L'\0';
    return 0;
}
inline int wcscpy_s(wchar_t* dst, std::size_t n, const wchar_t* src)
{
    std::wcsncpy(dst, src, n - 1);
    dst[n - 1] = L'\0';
    return 0;
}
template <std::size_t N>
inline int strcpy_s(char (&dst)[N], const char* src)
{
    std::strncpy(dst, src, N - 1);
    dst[N - 1] = '\0';
    return 0;
}
inline int strcpy_s(char* dst, std::size_t n, const char* src)
{
    std::strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
    return 0;
}
inline int wcstombs_s(std::size_t* converted, char* dst, std::size_t n, const wchar_t* src, std::size_t count)
{
    const std::size_t len = std::wcstombs(dst, src, std::min(n - 1, count));
    const std::size_t end = len == static_cast<std::size_t>(-1) ? 0 : len;
    dst[end]              = '\0';
    if (converted)
    {
        *converted = end + 1;
    }
    return 0;
}
template <std::size_t N, typename... A>
inline int sprintf_s(char (&dst)[N], const char* fmt, A... a)
{
    return std::snprintf(dst, N, fmt, a...); // Flawfinder: ignore (the SDK's own constant formats, bounded)
}
template <typename... A>
inline int sprintf_s(char* dst, std::size_t n, const char* fmt, A... a)
{
    return std::snprintf(dst, n, fmt, a...); // Flawfinder: ignore (the SDK's own constant formats, bounded)
}
inline int swprintf_s(wchar_t* dst, std::size_t n, const wchar_t* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    const int r = std::vswprintf(dst, n, fmt, a); // Flawfinder: ignore (the SDK's own constant formats, bounded)
    va_end(a);
    return r;
}

// The SDK sizes its opaque contexts for Windows, where wchar_t is 2 bytes; its private state holds wchar_t names, so
// on Linux (4-byte wchar_t) the FSR 3 upscaler's no longer fits (ffx_fsr3upscaler.cpp's static_assert). ffx_types.h
// is included once (#pragma once), so its default can be raised here, before anything else sees it. Every
// translation unit that includes ffx_fsr3upscaler.h (the SDK's and gfx/fsr.cpp) includes this header first.
#include <FidelityFX/host/ffx_types.h>
#include <FidelityFX/host/ffx_util.h> // FFX_UNUSED (ffx_message.cpp uses it without including this off Windows)
#undef FFX_SDK_DEFAULT_CONTEXT_SIZE
#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 256)
#endif
