/* The SDK probe: C, against the public header, linked to the Interposer by its
 * soname the way a game patch would be. The harness dlopens it after loading
 * the Interposer itself, so the dynamic linker has to bind it to that copy
 * rather than load another (SDK-02). */

#include "LANCommander.Interposer.h"

#define PROBE_EXPORT __attribute__((visibility("default")))

PROBE_EXPORT BOOL SdkGetConfigString(const wchar_t* path, wchar_t* buffer, DWORD size)
{
    return InterposerGetConfigString(path, buffer, size);
}

PROBE_EXPORT void* SdkAddressOfGetConfigString(void)
{
    return (void*)&InterposerGetConfigString;
}

PROBE_EXPORT BOOL SdkGetUsername(wchar_t* buffer, DWORD size)
{
    return InterposerGetUsername(buffer, size);
}

PROBE_EXPORT void SdkLog(const wchar_t* verb, const wchar_t* message)
{
    InterposerLog(verb, message);
}

/* A Windows-only API: links, and reports nothing to do. */
PROBE_EXPORT DWORD SdkSetRegistryValueBySuffix(const wchar_t* suffix, const wchar_t* name, const wchar_t* value)
{
    return InterposerSetRegistryValueBySuffix(suffix, name, value);
}
