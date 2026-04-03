#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Install CreateWindowExW/A, SetWindowPos, SetWindowLongW/A hooks.
// Called from InstallHooks() when g_borderlessEnabled is true.
void InstallBorderlessHooks();

// Publicly callable: strip border styles, center, and track hwnd as the main window.
// Safe to call before InstallBorderlessHooks (falls back to real Win32 when trampolines are null).
void ForceBorderless(HWND hwnd);
