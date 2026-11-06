#pragma once

// The callback typedefs and the InterposerRegister*Callback exports are part of
// the public API, declared in LANCommander.Interposer.h.
#include "LANCommander.Interposer.h"

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
void FireRelayCallback(const wchar_t* kind, const wchar_t* address, const wchar_t* detail, int value);
void FirePresenceCallback(const wchar_t* kind, const wchar_t* json);
