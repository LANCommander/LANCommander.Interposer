#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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

// Probe any server addresses collected since the last call that have not yet
// been probed. Stops as soon as a FastDL endpoint is confirmed. Blocking —
// call from the file-load hook just before a FastDL download is attempted.
void ProbeAllDiscoveredAddresses();

// ---------------------------------------------------------------------------
// Unified adapter enumeration — exported so plugins (via GetProcAddress) and
// managed/host apps (via P/Invoke) share one canonical adapter list instead of
// re-implementing iphlpapi parsing. Fixed-layout struct = stable ABI contract.
// ---------------------------------------------------------------------------
typedef struct InterposerNetworkAdapter {
    wchar_t friendlyName[256];
    wchar_t description[256];
    wchar_t macAddress[24];   // "00:11:22:33:44:55", empty if none
    wchar_t ipv4Address[16];  // first IPv4 (dotted-decimal), empty if none
    wchar_t ipv6Address[46];  // first IPv6, empty if none
    int     allowed;          // 1 = passes NetworkAdapters filter (or no filter), 0 = hidden
} InterposerNetworkAdapter;

// Fills up to `capacity` entries into `buffer` and writes the number written to
// *outCount. Returns the TOTAL number of adapters available (call with
// capacity 0 to query the count, then resize and call again). The DLL allocates
// nothing across the boundary — the caller owns `buffer`.
extern "C" __declspec(dllexport) DWORD InterposerEnumNetworkAdapters(
    InterposerNetworkAdapter* buffer, DWORD capacity, DWORD* outCount);
