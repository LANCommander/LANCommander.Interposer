#pragma once

// Resolve the libc trampolines the file detours call. Called from InstallHooks
// before the hooks are enabled.
void InstallFileHooks();
