#pragma once
#include <windows.h>

// ---------------------------------------------------------------------------
// Callback typedefs — registered by plugins/managed code via the exported
// InterposerRegister*Callback functions. Called from hook implementations
// on the same thread that triggered the hook.
//
// All callbacks are notification-only (observe, not modify). They fire
// regardless of logging flags but respect reentrancy guards.
// ---------------------------------------------------------------------------

// File operations: CreateFile, GetFileAttributes, FindFirstFile,
// Delete, Move, Copy, LoadLibrary.
typedef void (*InterposerFileCallback)(
    const wchar_t* verb,           // e.g. "FILE READ", "FILE REDIRECT", "FILE DELETE"
    const wchar_t* path,           // primary path
    const wchar_t* secondaryPath); // redirect/destination path, or null

// Registry operations: Open, Create, Query, Set, Delete, Enum.
typedef void (*InterposerRegistryCallback)(
    const wchar_t* verb,       // e.g. "REG OPEN", "REG READ", "REG WRITE"
    const wchar_t* keyPath,    // full key path
    const wchar_t* valueName); // value name, or null

// DNS resolution: getaddrinfo, gethostbyname, GetAddrInfoEx.
typedef void (*InterposerDnsCallback)(
    const wchar_t* hostname,            // original hostname
    const wchar_t* redirectedHostname); // redirected hostname (same if no redirect)

// Network connections: connect, send/recv address discovery.
typedef void (*InterposerNetworkCallback)(
    const wchar_t* address, // IP address or hostname
    int port);              // port number (0 if unknown)

// Identity: GetUserName, GetComputerName.
typedef void (*InterposerIdentityCallback)(
    const wchar_t* type,  // "USERNAME" or "COMPUTERNAME"
    const wchar_t* value); // the returned value

// ---------------------------------------------------------------------------
// Registration exports — called by plugins via GetProcAddress or by
// managed code via P/Invoke. Pass null to unregister.
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void InterposerRegisterFileCallback(InterposerFileCallback cb);
extern "C" __declspec(dllexport) void InterposerRegisterRegistryCallback(InterposerRegistryCallback cb);
extern "C" __declspec(dllexport) void InterposerRegisterDnsCallback(InterposerDnsCallback cb);
extern "C" __declspec(dllexport) void InterposerRegisterNetworkCallback(InterposerNetworkCallback cb);
extern "C" __declspec(dllexport) void InterposerRegisterIdentityCallback(InterposerIdentityCallback cb);

// ---------------------------------------------------------------------------
// Internal fire helpers — called from hook implementations. No-op when the
// corresponding callback is null. Inline so the null check is branch-predicted
// away in the common (no callback registered) case.
// ---------------------------------------------------------------------------
void FireFileCallback(const wchar_t* verb, const wchar_t* path, const wchar_t* secondaryPath = nullptr);
void FireRegistryCallback(const wchar_t* verb, const wchar_t* keyPath, const wchar_t* valueName = nullptr);
void FireDnsCallback(const wchar_t* hostname, const wchar_t* redirectedHostname);
void FireNetworkCallback(const wchar_t* address, int port);
void FireIdentityCallback(const wchar_t* type, const wchar_t* value);
