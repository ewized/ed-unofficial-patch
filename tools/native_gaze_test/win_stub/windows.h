#pragma once
#include <cstdint>

// CPU fixture substitute, selected only by this test's include path.
using HMODULE = void*;
using HANDLE = void*;
using BOOL = int;
using DWORD = unsigned long;
using LPCWSTR = const wchar_t*;
using FARPROC = std::intptr_t (*)();
struct LARGE_INTEGER { std::int64_t QuadPart; };
inline constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 2;
inline constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4;

inline constexpr unsigned MAX_PATH = 260;
inline constexpr DWORD ERROR_SUCCESS = 0, ERROR_NO_MORE_FILES = 18, ERROR_BAD_LENGTH = 24;
inline const HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1));
inline constexpr BOOL TRUE = 1;
inline constexpr int CSTR_EQUAL = 2;

DWORD GetCurrentProcessId() noexcept;
DWORD GetLastError() noexcept;
void SetLastError(DWORD) noexcept;
BOOL CloseHandle(HANDLE) noexcept;
int CompareStringOrdinal(LPCWSTR, int, LPCWSTR, int, BOOL) noexcept;
bool GetModuleHandleExW(DWORD, LPCWSTR, HMODULE*) noexcept;
bool FreeLibrary(HMODULE) noexcept;
DWORD GetModuleFileNameW(HMODULE, wchar_t*, DWORD) noexcept;
FARPROC GetProcAddress(HMODULE, const char*) noexcept;
bool QueryPerformanceCounter(LARGE_INTEGER*) noexcept;
bool QueryPerformanceFrequency(LARGE_INTEGER*) noexcept;
