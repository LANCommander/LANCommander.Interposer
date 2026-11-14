// The DirectInput mouse transform's export, for a build without DirectInput:
// always on Linux, and in an embedded build with INTERPOSER_FEATURE_DIRECTINPUT
// off. See core/interposer_features.h.

#include "instance.h"

extern "C" {

INTERPOSER_API BOOL InterposerRegisterMouseTransform(InterposerMouseTransform callback, void* userData)
{
    FORWARD_TO_PRIMARY(InterposerRegisterMouseTransform, callback, userData);
    return FALSE;
}

} // extern "C"
