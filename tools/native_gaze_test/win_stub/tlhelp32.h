#pragma once
#include "windows.h"

inline constexpr DWORD TH32CS_SNAPMODULE = 8;
struct MODULEENTRY32W {
    DWORD dwSize;
    HMODULE hModule;
    wchar_t szModule[256];
    wchar_t szExePath[MAX_PATH];
};

HANDLE CreateToolhelp32Snapshot(DWORD, DWORD) noexcept;
BOOL Module32FirstW(HANDLE, MODULEENTRY32W*) noexcept;
BOOL Module32NextW(HANDLE, MODULEENTRY32W*) noexcept;
