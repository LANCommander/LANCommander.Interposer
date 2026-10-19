#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Install all MinHook API hooks. Called from DllMain on DLL_PROCESS_ATTACH.
void InstallHooks();

// Remove all hooks and uninitialize MinHook. Called from DllMain on DLL_PROCESS_DETACH.
// processTerminating is DllMain's lpvReserved != nullptr: the process is exiting
// and the OS has already ended every other thread, so none are waited for.
void RemoveHooks(bool processTerminating);

// Called from LoadLibrary hooks after a DLL is successfully loaded.
// Checks whether any hooks that failed at startup (module not yet loaded) can
// now be installed for the newly loaded module and enables them.
void OnLibraryLoaded(HMODULE hModule);
