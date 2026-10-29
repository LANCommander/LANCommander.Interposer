#pragma once

// Install hooks for GetUserNameW/A (advapi32) and GetComputerNameW/A (kernel32).
// Each pair is only hooked when its respective global is non-empty.
// Reads Local\InterposerUsername_<pid> and Local\InterposerComputerName_<pid>
// MMFs first; if present they override values loaded from Config.yml.
void InstallIdentityHooks();
