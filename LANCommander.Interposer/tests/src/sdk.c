/*
 * Calls into the Interposer the way a game patch does: through the public
 * header and the import library, not GetProcAddress.
 *
 * Compiled as C, and with /Gz (stdcall by default) -- the two things a
 * consumer of the header is most likely to differ from the DLL in. C proves the
 * header has no C++ in it. /Gz proves every declaration states __cdecl rather
 * than inheriting the default: an import declared stdcall would link against
 * a decorated name (_InterposerLog@8) the import library does not have on
 * x86, and fail to build.
 *
 * The import is delay-loaded (see the vcxproj), so nothing here binds until
 * main.cpp has written the fixtures and loaded the DLL itself; the first call
 * then resolves to that same module, by name.
 *
 * These wrappers are __cdecl explicitly so main.cpp, compiled /Gd, calls them
 * correctly.
 */
#include "LANCommander.Interposer.h"

BOOL __cdecl SdkGetConfigString(const wchar_t* dotPath, wchar_t* buffer, DWORD bufferSize)
{
    return InterposerGetConfigString(dotPath, buffer, bufferSize);
}

BOOL __cdecl SdkGetUsername(wchar_t* buffer, DWORD bufferSize)
{
    return InterposerGetUsername(buffer, bufferSize);
}

void __cdecl SdkLog(const wchar_t* verb, const wchar_t* message)
{
    InterposerLog(verb, message);
}

/* A function with arguments whose stack cleanup would go wrong first if the
 * convention were mismatched: three pointers, one returned count. */
DWORD __cdecl SdkSetRegistryValueBySuffix(const wchar_t* keySuffix, const wchar_t* valueName, const wchar_t* value)
{
    return InterposerSetRegistryValueBySuffix(keySuffix, valueName, value);
}

DWORD __cdecl SdkEnumNetworkAdapterCount(void)
{
    DWORD written = 0;
    return InterposerEnumNetworkAdapters(NULL, 0, &written);
}

/* The import's address slot, which the first call filled in when it resolved
 * the delay-load thunk. Read directly rather than as &InterposerGetConfigString:
 * on x86, C takes that as the import library's jump stub inside this image. */
#ifdef _M_IX86
extern void* _imp__InterposerGetConfigString;
#define SDK_IMPORT_SLOT _imp__InterposerGetConfigString
#else
extern void* __imp_InterposerGetConfigString;
#define SDK_IMPORT_SLOT __imp_InterposerGetConfigString
#endif

void* __cdecl SdkAddressOfGetConfigString(void)
{
    return SDK_IMPORT_SLOT;
}
