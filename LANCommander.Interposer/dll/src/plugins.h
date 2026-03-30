#pragma once

// Load every .dll and .asi found in <dlldir>\.interposer\Plugins\.
// Call after MH_EnableHook so plugins can install their own hooks.
void LoadPlugins();

// FreeLibrary every module loaded by LoadPlugins().
// Call from RemoveHooks() before MH_Uninitialize().
void UnloadPlugins();
