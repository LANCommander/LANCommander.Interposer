// The audio device filter's export, for a build without it: always on Linux,
// and in an embedded build with INTERPOSER_FEATURE_AUDIO off. See
// core/interposer_features.h.

#include "instance.h"

extern "C" {

INTERPOSER_API DWORD InterposerEnumAudioDevices(InterposerAudioDevice* buffer, DWORD capacity, DWORD* outCount)
{
    FORWARD_TO_PRIMARY(InterposerEnumAudioDevices, buffer, capacity, outCount);

    if (outCount)
        *outCount = 0;

    return 0;
}

} // extern "C"
