#pragma once

void InstallRegistryHooks();   // MH_CreateHookApi x17 on advapi32; loads VirtualRegistry.reg
void RemoveRegistryHooks();    // flush VirtualRegistry.reg to disk if dirty
