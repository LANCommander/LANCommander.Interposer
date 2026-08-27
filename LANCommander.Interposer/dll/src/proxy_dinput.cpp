#ifdef INTERPOSER_PROXY_DINPUT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "config.h"
#include "dinput.h"

// ---------------------------------------------------------------------------
// dinput.dll proxy.
//
// Games from the DirectInput 3/7 era import dinput.dll directly and very often
// import neither version.dll nor dinput8.dll, so this is the only load method
// that reaches them.
//
// When DirectInput.FixLegacyDeviceEnumeration is enabled, DirectInputCreateA/Ex
// are served from the DX7 -> DX8 bridge and the system dinput.dll is never loaded
// at all — which is the point, since the bug being worked around is inside it.
// When the fix is off this is a plain passthrough and the rest of the Interposer
// still applies.
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI* PFN_DirectInputCreateA)(HINSTANCE, DWORD, LPVOID*, void*);
typedef HRESULT (WINAPI* PFN_DirectInputCreateW)(HINSTANCE, DWORD, LPVOID*, void*);
typedef HRESULT (WINAPI* PFN_DirectInputCreateEx)(HINSTANCE, DWORD, const void*, LPVOID*, void*);
typedef HRESULT (WINAPI* PFN_DllCanUnloadNow)(void);
typedef HRESULT (WINAPI* PFN_DllGetClassObject)(const void*, const void*, LPVOID*);
typedef HRESULT (WINAPI* PFN_DllRegisterServer)(void);
typedef HRESULT (WINAPI* PFN_DllUnregisterServer)(void);

static HMODULE g_realDInput = nullptr;

static PFN_DirectInputCreateA  pfn_DirectInputCreateA  = nullptr;
static PFN_DirectInputCreateW  pfn_DirectInputCreateW  = nullptr;
static PFN_DirectInputCreateEx pfn_DirectInputCreateEx = nullptr;
static PFN_DllCanUnloadNow     pfn_DllCanUnloadNow     = nullptr;
static PFN_DllGetClassObject   pfn_DllGetClassObject   = nullptr;
static PFN_DllRegisterServer   pfn_DllRegisterServer   = nullptr;
static PFN_DllUnregisterServer pfn_DllUnregisterServer = nullptr;

// Resolved on the first forwarded call rather than from InitProxy(). InitProxy
// runs before LoadConfig(), so loading eagerly would pull the broken system
// dinput.dll into every process even when the bridge is enabled and nothing
// will ever call into it.
static bool EnsureRealDInput()
{
    if (g_realDInput)
        return true;

    wchar_t path[MAX_PATH];

    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\dinput.dll");

    g_realDInput = LoadLibraryW(path);

    if (!g_realDInput)
        return false;

#define RESOLVE(name) pfn_##name = reinterpret_cast<PFN_##name>( \
    reinterpret_cast<void*>(GetProcAddress(g_realDInput, #name)))
    RESOLVE(DirectInputCreateA);
    RESOLVE(DirectInputCreateW);
    RESOLVE(DirectInputCreateEx);
    RESOLVE(DllCanUnloadNow);
    RESOLVE(DllGetClassObject);
    RESOLVE(DllRegisterServer);
    RESOLVE(DllUnregisterServer);
#undef RESOLVE

    return true;
}

void InitProxy()
{
    // Nothing to do — see EnsureRealDInput above.
}

void UninitProxy()
{
    if (g_realDInput)
    {
        FreeLibrary(g_realDInput);
        g_realDInput = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Export stubs
// ---------------------------------------------------------------------------

extern "C" HRESULT WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD dwVersion,
    LPVOID* ppDI, void* punkOuter)
{
    if (g_diFixLegacyEnum)
        return DirectInputBridgeCreate(hinst, dwVersion, ppDI, punkOuter);

    if (!EnsureRealDInput() || !pfn_DirectInputCreateA)
        return E_FAIL;

    return pfn_DirectInputCreateA(hinst, dwVersion, ppDI, punkOuter);
}

// The Unicode entry point is always forwarded: the bridge only synthesizes ANSI
// device enumeration, and no game seen on this path uses it.
extern "C" HRESULT WINAPI DirectInputCreateW(HINSTANCE hinst, DWORD dwVersion,
    LPVOID* ppDI, void* punkOuter)
{
    if (!EnsureRealDInput() || !pfn_DirectInputCreateW)
        return E_FAIL;

    return pfn_DirectInputCreateW(hinst, dwVersion, ppDI, punkOuter);
}

extern "C" HRESULT WINAPI DirectInputCreateEx(HINSTANCE hinst, DWORD dwVersion,
    const void* riid, LPVOID* ppvOut, void* punkOuter)
{
    if (g_diFixLegacyEnum && DirectInputBridgeWantsIid(riid))
        return DirectInputBridgeCreate(hinst, dwVersion, ppvOut, punkOuter);

    if (!EnsureRealDInput() || !pfn_DirectInputCreateEx)
        return E_FAIL;

    return pfn_DirectInputCreateEx(hinst, dwVersion, riid, ppvOut, punkOuter);
}

extern "C" HRESULT WINAPI DllCanUnloadNow(void)
{
    // S_FALSE keeps the DLL resident, which is what we want either way: the
    // bridge patches vtables the game is still holding.
    if (!EnsureRealDInput() || !pfn_DllCanUnloadNow)
        return S_FALSE;

    return pfn_DllCanUnloadNow();
}

extern "C" HRESULT WINAPI DllGetClassObject(const void* rclsid, const void* riid, LPVOID* ppv)
{
    if (!EnsureRealDInput() || !pfn_DllGetClassObject)
        return 0x80040111L; // CLASS_E_CLASSNOTAVAILABLE

    return pfn_DllGetClassObject(rclsid, riid, ppv);
}

extern "C" HRESULT WINAPI DllRegisterServer(void)
{
    if (!EnsureRealDInput() || !pfn_DllRegisterServer)
        return E_FAIL;

    return pfn_DllRegisterServer();
}

extern "C" HRESULT WINAPI DllUnregisterServer(void)
{
    if (!EnsureRealDInput() || !pfn_DllUnregisterServer)
        return E_FAIL;

    return pfn_DllUnregisterServer();
}

#endif // INTERPOSER_PROXY_DINPUT
