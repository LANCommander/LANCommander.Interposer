// Every export the public header declares that has no Linux implementation
// yet. Defined anyway, so a patch written against the header links on both
// platforms and finds out at run time, the way it would on a Windows version
// without the subsystem: each reports "nothing there" rather than failing.
//
// The virtual registry, the audio device filter and the DirectInput mouse
// transform are Windows features and stay here. Network adapters and the relay
// arrive with the Linux network hooks; rich presence with a Linux Discord IPC.

#include "instance.h"

extern "C" {

// ── Virtual registry ──────────────────────────────────────────────────────────

INTERPOSER_API void InterposerSetRegistryValue(const wchar_t* keyPath, const wchar_t* valueName, const wchar_t* value)
{
    FORWARD_TO_PRIMARY(InterposerSetRegistryValue, keyPath, valueName, value);
}

INTERPOSER_API DWORD InterposerSetRegistryValueBySuffix(const wchar_t* keySuffix, const wchar_t* valueName, const wchar_t* value)
{
    FORWARD_TO_PRIMARY(InterposerSetRegistryValueBySuffix, keySuffix, valueName, value);
    return 0;
}

// ── Devices ───────────────────────────────────────────────────────────────────

INTERPOSER_API DWORD InterposerEnumNetworkAdapters(InterposerNetworkAdapter* buffer, DWORD capacity, DWORD* outCount)
{
    FORWARD_TO_PRIMARY(InterposerEnumNetworkAdapters, buffer, capacity, outCount);

    if (outCount)
        *outCount = 0;

    return 0;
}

INTERPOSER_API DWORD InterposerEnumAudioDevices(InterposerAudioDevice* buffer, DWORD capacity, DWORD* outCount)
{
    FORWARD_TO_PRIMARY(InterposerEnumAudioDevices, buffer, capacity, outCount);

    if (outCount)
        *outCount = 0;

    return 0;
}

INTERPOSER_API BOOL InterposerRegisterMouseTransform(InterposerMouseTransform callback, void* userData)
{
    FORWARD_TO_PRIMARY(InterposerRegisterMouseTransform, callback, userData);
    return FALSE;
}

// ── Network relay ─────────────────────────────────────────────────────────────

// As on Windows with the relay switched off: a zeroed status, state Disabled.
INTERPOSER_API BOOL InterposerRelayGetStatus(InterposerRelayStatus* status)
{
    FORWARD_TO_PRIMARY(InterposerRelayGetStatus, status);

    if (!status || status->size < sizeof(InterposerRelayStatus))
        return FALSE;

    *status = InterposerRelayStatus{};
    status->size = sizeof(InterposerRelayStatus);
    return TRUE;
}

INTERPOSER_API DWORD InterposerRelayEnumPeers(InterposerRelayPeer* buffer, DWORD capacity, DWORD* outCount)
{
    FORWARD_TO_PRIMARY(InterposerRelayEnumPeers, buffer, capacity, outCount);

    if (outCount)
        *outCount = 0;

    return 0;
}

INTERPOSER_API BOOL InterposerRelayConnect(const wchar_t* yamlOverrides)
{
    FORWARD_TO_PRIMARY(InterposerRelayConnect, yamlOverrides);
    return FALSE;
}

INTERPOSER_API void InterposerRelayDisconnect(void)
{
    FORWARD_TO_PRIMARY(InterposerRelayDisconnect);
}

// ── Rich presence ─────────────────────────────────────────────────────────────

INTERPOSER_API void InterposerSetPresenceDetails(const char* details)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceDetails, details);
}

INTERPOSER_API void InterposerSetPresenceState(const char* state)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceState, state);
}

INTERPOSER_API void InterposerSetPresenceTimestamps(long long start, long long end)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceTimestamps, start, end);
}

INTERPOSER_API void InterposerSetPresenceLargeImage(const char* key, const char* text)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceLargeImage, key, text);
}

INTERPOSER_API void InterposerSetPresenceSmallImage(const char* key, const char* text)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceSmallImage, key, text);
}

INTERPOSER_API void InterposerSetPresenceParty(const char* id, int size, int max)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceParty, id, size, max);
}

INTERPOSER_API void InterposerSetPresenceButton(int index, const char* text, const char* url)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceButton, index, text, url);
}

INTERPOSER_API void InterposerSetPresenceType(int type)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceType, type);
}

INTERPOSER_API void InterposerSetPresenceName(const char* name)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceName, name);
}

INTERPOSER_API void InterposerUpdatePresence(void)
{
    FORWARD_TO_PRIMARY(InterposerUpdatePresence);
}

INTERPOSER_API void InterposerClearPresence(void)
{
    FORWARD_TO_PRIMARY(InterposerClearPresence);
}

} // extern "C"
