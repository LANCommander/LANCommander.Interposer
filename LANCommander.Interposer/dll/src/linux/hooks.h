#pragma once

// Install everything: load the configuration, enable the hooks, load plugins.
// Called once, from the constructor, by the primary copy only.
void InstallHooks();

// Undo InstallHooks. Called from the destructor: on dlclose, or at process exit.
void RemoveHooks();
