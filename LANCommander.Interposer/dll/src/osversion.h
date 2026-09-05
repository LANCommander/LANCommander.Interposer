#pragma once

// Install hooks for GetVersion / GetVersionExA / GetVersionExW (kernel32) and
// RtlGetVersion (ntdll). Nothing is hooked unless OsVersion.Version selected a
// version in Config.yml — g_osVersionEnabled is the gate.
//
// This is the hook equivalent of ticking a compatibility mode on the game
// executable, without the AppCompat shim engine: no per-user registry state to
// lose on reinstall, and none of the elevation the pre-Vista layers force.
void InstallOsVersionHooks();
