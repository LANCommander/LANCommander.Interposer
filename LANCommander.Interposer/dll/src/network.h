#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Install hooks for Winsock2 getaddrinfo, GetAddrInfoW, connect, and WSAConnect.
// Call from InstallHooks(), after LoadConfig() and InitFastDL().
// No-op if both g_logNetwork and g_fastdlProbeConnections are false.
void InstallNetworkHooks();

// Clear internal state. Call from RemoveHooks() before MH_DisableHook.
void RemoveNetworkHooks();
