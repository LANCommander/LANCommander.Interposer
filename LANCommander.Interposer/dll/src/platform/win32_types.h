#pragma once

// The Win32 names the core is written in. On Windows, <windows.h> itself --
// which the Windows subsystems also rely on arriving through config.h. Elsewhere,
// the same names with their Win32 sizes, so the parsed configuration keeps one
// layout everywhere. The public header supplies BOOL, DWORD and LONG.

#include "LANCommander.Interposer.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#else

#include <cstdint>

typedef uint16_t WORD;
typedef uint8_t  BYTE;

#define WINAPI

// The OSVERSIONINFOEX values OsVersion stores. Only the Windows OsVersion hooks
// read them; they are defined here so the config parser compiles everywhere.
#define VER_NT_WORKSTATION     0x0000001
#define VER_NT_SERVER          0x0000003
#define VER_SUITE_SINGLEUSERTS 0x00000100

#endif
