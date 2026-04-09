#ifdef INTERPOSER_PROXY_DINPUT8

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>

static HMODULE g_realDInput8 = nullptr;

// Function pointer types
typedef HRESULT (WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (WINAPI* PFN_DllCanUnloadNow)(void);
typedef HRESULT (WINAPI* PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (WINAPI* PFN_DllRegisterServer)(void);
typedef HRESULT (WINAPI* PFN_DllUnregisterServer)(void);

static PFN_DirectInput8Create  pfn_DirectInput8Create  = nullptr;
static PFN_DllCanUnloadNow     pfn_DllCanUnloadNow     = nullptr;
static PFN_DllGetClassObject   pfn_DllGetClassObject   = nullptr;
static PFN_DllRegisterServer   pfn_DllRegisterServer   = nullptr;
static PFN_DllUnregisterServer pfn_DllUnregisterServer = nullptr;

void InitProxy()
{
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\dinput8.dll");

    g_realDInput8 = LoadLibraryW(path);
    if (!g_realDInput8)
        return;

#define RESOLVE(name) pfn_##name = reinterpret_cast<PFN_##name>(GetProcAddress(g_realDInput8, #name))
    RESOLVE(DirectInput8Create);
    RESOLVE(DllCanUnloadNow);
    RESOLVE(DllGetClassObject);
    RESOLVE(DllRegisterServer);
    RESOLVE(DllUnregisterServer);
#undef RESOLVE
}

void UninitProxy()
{
    if (g_realDInput8)
    {
        FreeLibrary(g_realDInput8);
        g_realDInput8 = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Export stubs — forward every call to the real system dinput8.dll
// ---------------------------------------------------------------------------

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion,
    REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    return pfn_DirectInput8Create
        ? pfn_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter)
        : E_FAIL;
}

extern "C" HRESULT WINAPI DllCanUnloadNow(void)
    { return pfn_DllCanUnloadNow ? pfn_DllCanUnloadNow() : S_FALSE; }

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
    { return pfn_DllGetClassObject ? pfn_DllGetClassObject(rclsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE; }

extern "C" HRESULT WINAPI DllRegisterServer(void)
    { return pfn_DllRegisterServer ? pfn_DllRegisterServer() : E_FAIL; }

extern "C" HRESULT WINAPI DllUnregisterServer(void)
    { return pfn_DllUnregisterServer ? pfn_DllUnregisterServer() : E_FAIL; }

#endif // INTERPOSER_PROXY_DINPUT8
