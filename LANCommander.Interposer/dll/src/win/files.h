#pragma once

// Install CreateFileW/A and GetFileAttributesW/A hooks.
// Called from InstallHooks() after LoadConfig() and before MH_EnableHook.
void InstallFileHooks();
