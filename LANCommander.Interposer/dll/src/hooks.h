#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Install all MinHook API hooks. Called from DllMain on DLL_PROCESS_ATTACH.
void InstallHooks();

// Remove all hooks and uninitialize MinHook. Called from DllMain on DLL_PROCESS_DETACH.
void RemoveHooks();

// Publicly callable: apply borderless style + centering to an already-visible window.
// Safe to call before InstallHooks (falls back to real Win32 calls when trampolines are null).
void ForceBorderless(HWND hwnd);
