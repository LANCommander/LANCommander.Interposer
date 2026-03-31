#pragma once
#include <windows.h>

void InstallRegistryHooks();   // MH_CreateHookApi x17 on advapi32; loads VirtualRegistry.reg
void RemoveRegistryHooks();    // flush VirtualRegistry.reg to disk if dirty

// Inject a transient REG_SZ into the virtual store by exact key path (not persisted to Registry.reg).
// valueName may be "@" or nullptr/empty to target the default (unnamed) value.
extern "C" __declspec(dllexport) void InterposerSetRegistryValue(const wchar_t* keyPath, const wchar_t* valueName, const wchar_t* value);

// Inject a transient REG_SZ into every virtual store key whose path ends with keySuffix
// (backslash-boundary match, case-insensitive). Returns the number of keys updated.
// valueName may be "@" or nullptr/empty to target the default (unnamed) value.
extern "C" __declspec(dllexport) DWORD InterposerSetRegistryValueBySuffix(const wchar_t* keySuffix, const wchar_t* valueName, const wchar_t* value);
