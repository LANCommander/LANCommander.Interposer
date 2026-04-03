#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Install all MinHook API hooks. Called from DllMain on DLL_PROCESS_ATTACH.
void InstallHooks();

// Remove all hooks and uninitialize MinHook. Called from DllMain on DLL_PROCESS_DETACH.
void RemoveHooks();
