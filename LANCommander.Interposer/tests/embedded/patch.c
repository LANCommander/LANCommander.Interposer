/*
 * A stand-in game patch that embeds the Interposer, for the EM- tests.
 *
 * Built the way such a patch is: C, against the public headers, linking
 * LANCommander::Interposer::Embedded, with a DllMain of its own that attaches
 * the Interposer before doing anything else. It is staged as dinput.dll and
 * serves DirectInputCreateA itself, as a DirectInput patch does, so the tests
 * can tell whether another Interposer left that export alone.
 */
#include "LANCommander.Interposer.Embedded.h"

static BOOL g_primary;
static LONG g_directInputCalls;

/* What InterposerEmbeddedAttach returned. */
__declspec(dllexport) BOOL __cdecl PatchProbeIsPrimary(void)
{
    return g_primary;
}

/* A direct call into the embedded Interposer, as a patch makes one. */
__declspec(dllexport) BOOL __cdecl PatchProbeGetConfig(const wchar_t* path, wchar_t* buffer, DWORD size)
{
    return InterposerGetConfigString(path, buffer, size);
}

/* How often the game reached this module's DirectInputCreateA. */
__declspec(dllexport) LONG __cdecl PatchProbeDirectInputCalls(void)
{
    return g_directInputCalls;
}

/* The export a DirectInput 3/7 game imports. A real patch wraps the object
 * that comes back; this one only counts the call. Exported through patch.def,
 * which keeps the x86 name undecorated, as dinput.dll's is. */
HRESULT WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD version, void** ppDI, void* punkOuter)
{
    InterlockedIncrement(&g_directInputCalls);
    return InterposerDirectInputCreateA(hinst, version, ppDI, punkOuter);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        g_primary = InterposerEmbeddedAttach(instance);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        InterposerEmbeddedDetach(reserved != NULL);
    }

    return TRUE;
}
