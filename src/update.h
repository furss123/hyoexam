#pragma once
#include <windows.h>
#include <string>

constexpr UINT WM_HYO_UPDATE_READY = WM_APP + 42;

struct PreparedUpdate {
    std::wstring version;
    std::wstring path;
};

void StartUpdateCheck(HWND hwnd, const wchar_t* currentVersion);
