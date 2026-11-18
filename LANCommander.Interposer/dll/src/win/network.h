#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// t_netHookDepth and NetHookScope: see there.
#include "net_hook.h"

// Install hooks for ws2_32 (getaddrinfo, GetAddrInfoW, connect, WSAConnect,
// send/recv variants) and wsock32 (gethostbyname, connect, sendto/recvfrom,
// send/recv) for older Winsock 1 applications.
// Call from InstallHooks(), after LoadConfig() and InitFastDL().
// No-op if both g_logNetwork and g_fastdlProbeConnections are false.
void InstallNetworkHooks();

// Called when a DLL is loaded post-startup. Installs and enables any network
// hooks for moduleName (the DLL's basename, e.g. L"wsock32.dll") that could
// not be installed at startup because the module was not yet loaded.
void LateInstallNetworkHooks(const wchar_t* moduleName);

// Clear internal state. Call from RemoveHooks() before MH_DisableHook.
void RemoveNetworkHooks();

// ProbeAllDiscoveredAddresses, which the network hooks define for FastDL, is
// declared in fastdl.h.
#include "fastdl.h"

// InterposerNetworkAdapter and InterposerEnumNetworkAdapters -- the unified
// adapter enumeration plugins and managed hosts share -- are declared in the
// public header.
#include "LANCommander.Interposer.h"
