// The adapter enumeration's export, for an embedded build with
// INTERPOSER_FEATURE_NETWORK off: no adapters. See core/interposer_features.h.

#include "instance.h"

extern "C" {

INTERPOSER_API DWORD InterposerEnumNetworkAdapters(InterposerNetworkAdapter* buffer, DWORD capacity, DWORD* outCount)
{
    FORWARD_TO_PRIMARY(InterposerEnumNetworkAdapters, buffer, capacity, outCount);

    if (outCount)
        *outCount = 0;

    return 0;
}

} // extern "C"
