#pragma once

// MinHook, for the Windows subsystems that install their hooks with it directly.
#include <MinHook.h>

// Log a MinHook hook installation result. Always written regardless of other
// logging flags. Pass the MH_STATUS value returned by MH_CreateHookApi.
void LogHookInit(const wchar_t* module, const char* fn, MH_STATUS status);
