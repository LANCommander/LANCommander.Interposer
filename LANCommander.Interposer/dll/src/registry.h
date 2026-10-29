#pragma once
#include <windows.h>

void InstallRegistryHooks();   // MH_CreateHookApi x42, KernelBase-first; loads the Registry.Files stack
void RemoveRegistryHooks();    // flush VirtualRegistry.reg to disk if dirty

// InterposerSetRegistryValue / InterposerSetRegistryValueBySuffix are declared
// in the public header.
#include "LANCommander.Interposer.h"
