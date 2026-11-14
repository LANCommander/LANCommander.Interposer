// The network relay's exports, for a build without it: always on Linux, and in
// an embedded build with INTERPOSER_FEATURE_RELAY off. They answer as the relay
// does when it is switched off. See core/interposer_features.h.

#include "instance.h"

extern "C" {

// A zeroed status, state Disabled.
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

} // extern "C"
