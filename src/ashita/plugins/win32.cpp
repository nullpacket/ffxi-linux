// SPDX-License-Identifier: GPL-3.0-or-later
// Ashita plugins on Linux (issue #34, docs/ashita-compat.md "Plugins"): the Win32 functions natively built plugins
// call, linked into the client and exported to them (exports.list). Declarations are Wine's (compat.h); only what the
// supported plugins use is here, and a plugin that calls anything else fails to load with the missing name.
//
// FFXiMain.dll: plugins find game data by scanning the game's code for byte signatures. There is no game code here,
// so GetModuleHandle("FFXiMain.dll") answers a small stand-in image, placed below 2 GB (the plugins keep pointers in
// 32-bit DWORDs), that holds the signatures of the supported plugins followed by the data they read.
#include "ashita/plugins/compat.h"

#include <psapi.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xi::ashita
{
namespace
{

thread_local DWORD lastError = 0;

// The stand-in image: Shorthand's wardrobe signature "A1 ?? ?? ?? ?? 8B 88 B4 00 00 00 C1 E9 07 F6 C1 01 E9"
// (mov eax, [pointer]; mov ecx, [eax + 0xB4] ...). The pointer leads to a pointer to an object whose byte +0xB4 holds
// the wardrobes' enabled bits (bit n - 9 for container n), kept current by the plugin host.
struct Image
{
    uint8_t* base  = nullptr;
    DWORD    size  = 0;
    uint8_t* flags = nullptr;
};

Image& image()
{
    static Image img = [] {
        Image                 i;
        constexpr std::size_t kSize = 0x1000;
        void* p = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED)
        {
            return i;
        }
        i.base     = static_cast<uint8_t*>(p);
        i.size     = kSize;
        auto put32 = [](uint8_t* at, const void* ptr) {
            const auto v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr));
            std::memcpy(at, &v, 4);
        };
        uint8_t* object  = i.base + 0x800; // the object: +0xB4 the flags
        uint8_t* pointer = i.base + 0x700; // the global that points to it
        put32(pointer, object);
        static const uint8_t kCode[] = { 0xA1, 0,    0,    0,    0,    0x8B, 0x88, 0xB4, 0x00,
                                         0x00, 0x00, 0xC1, 0xE9, 0x07, 0xF6, 0xC1, 0x01, 0xE9 };
        std::memcpy(i.base + 0x100, kCode, sizeof(kCode));
        put32(i.base + 0x101, pointer);
        i.flags = object + 0xB4;
        return i;
    }();
    return img;
}

bool isGameModule(LPCSTR name)
{
    if (name == nullptr)
    {
        return false;
    }
    std::string n(name);
    for (auto& c : n)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return n == "ffximain.dll" || n == "ffximain";
}

} // namespace

void pluginSetBagFlags(uint8_t flags)
{
    if (auto& i = image(); i.flags)
    {
        *i.flags = flags;
    }
}

} // namespace xi::ashita

using xi::ashita::lastError;

extern "C"
{
    DWORD WINAPI GetLastError(void)
    {
        return lastError;
    }

    void WINAPI SetLastError(DWORD error)
    {
        lastError = error;
    }

    // Paths come with Windows separators from some plugins; the folders are made as the path names them.
    BOOL WINAPI CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES)
    {
        if (path == nullptr)
        {
            lastError = ERROR_PATH_NOT_FOUND;
            return FALSE;
        }
        std::string p(path);
        for (auto& c : p)
        {
            c = c == '\\' ? '/' : c;
        }
        if (mkdir(p.c_str(), 0755) == 0)
        {
            return TRUE;
        }
        lastError = errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_PATH_NOT_FOUND;
        return FALSE;
    }

    HANDLE WINAPI GetCurrentProcess(void)
    {
        return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1));
    }

    DWORD WINAPI GetCurrentProcessId(void)
    {
        return static_cast<DWORD>(getpid());
    }

    HMODULE WINAPI GetModuleHandleA(LPCSTR name)
    {
        if (xi::ashita::isGameModule(name) && xi::ashita::image().base)
        {
            return reinterpret_cast<HMODULE>(xi::ashita::image().base);
        }
        lastError = ERROR_MOD_NOT_FOUND;
        return nullptr;
    }

    BOOL WINAPI K32GetModuleInformation(HANDLE, HMODULE module, LPMODULEINFO info, DWORD size)
    {
        const auto& img = xi::ashita::image();
        if (info == nullptr || size < sizeof(MODULEINFO) || module == nullptr ||
            reinterpret_cast<uint8_t*>(module) != img.base)
        {
            lastError = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        info->lpBaseOfDll = img.base;
        info->SizeOfImage = img.size;
        info->EntryPoint  = img.base;
        return TRUE;
    }

    // The registry: there is none (plugins read PlayOnline's install paths from it and fall back when absent).
    LSTATUS WINAPI RegOpenKeyExA(HKEY, LPCSTR, DWORD, REGSAM, PHKEY result)
    {
        if (result != nullptr)
        {
            *result = nullptr;
        }
        return ERROR_FILE_NOT_FOUND;
    }

    LSTATUS WINAPI RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD)
    {
        return ERROR_FILE_NOT_FOUND;
    }

    LSTATUS WINAPI RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD)
    {
        return ERROR_ACCESS_DENIED;
    }

    LSTATUS WINAPI RegCloseKey(HKEY)
    {
        return ERROR_SUCCESS;
    }

    void WINAPI Sleep(DWORD ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    DWORD WINAPI GetTickCount(void)
    {
        return static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    ULONGLONG WINAPI GetTickCount64(void)
    {
        return static_cast<ULONGLONG>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER* counter)
    {
        counter->QuadPart =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        return TRUE;
    }

    BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER* frequency)
    {
        frequency->QuadPart = 1000000000LL;
        return TRUE;
    }

    void WINAPI OutputDebugStringA(LPCSTR text)
    {
        if (text != nullptr)
        {
            std::fputs(text, stderr);
        }
    }

} // extern "C"
