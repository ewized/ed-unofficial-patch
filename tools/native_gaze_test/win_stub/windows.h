#pragma once
#include <cstdint>

// CPU fixture substitute, selected only by this test's include path.
using HMODULE = void*;
using DWORD = unsigned long;
using FARPROC = std::intptr_t (*)();
struct LARGE_INTEGER { std::int64_t QuadPart; };
inline constexpr DWORD LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x100;
inline constexpr DWORD LOAD_LIBRARY_SEARCH_SYSTEM32 = 0x800;

HMODULE LoadLibraryExW(const wchar_t*, void*, DWORD) noexcept;
DWORD GetModuleFileNameW(HMODULE, wchar_t*, DWORD) noexcept;
FARPROC GetProcAddress(HMODULE, const char*) noexcept;
bool QueryPerformanceCounter(LARGE_INTEGER*) noexcept;
bool QueryPerformanceFrequency(LARGE_INTEGER*) noexcept;
