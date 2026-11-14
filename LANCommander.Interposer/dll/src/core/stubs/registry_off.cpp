// The virtual registry's exports, for a build without it: always on Linux,
// where the registry is a Windows feature, and in an embedded build with
// INTERPOSER_FEATURE_REGISTRY off. See core/interposer_features.h.

#include "instance.h"

extern "C" {

INTERPOSER_API void InterposerSetRegistryValue(const wchar_t* keyPath, const wchar_t* valueName, const wchar_t* value)
{
    FORWARD_TO_PRIMARY(InterposerSetRegistryValue, keyPath, valueName, value);
}

INTERPOSER_API DWORD InterposerSetRegistryValueBySuffix(const wchar_t* keySuffix, const wchar_t* valueName, const wchar_t* value)
{
    FORWARD_TO_PRIMARY(InterposerSetRegistryValueBySuffix, keySuffix, valueName, value);
    return 0;
}

} // extern "C"
