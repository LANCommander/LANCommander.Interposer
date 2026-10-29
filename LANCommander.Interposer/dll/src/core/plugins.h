#pragma once

// Load every plugin (.dll and .asi on Windows) found in the Plugins directory under
// <base>\.interposer, and its subdirectories. Call after the hooks are enabled so
// plugins can install their own hooks.
void LoadPlugins();

// Unload every module loaded by LoadPlugins(), in reverse load order.
// Call from RemoveHooks() before the hooking engine is torn down.
void UnloadPlugins();
