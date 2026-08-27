#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "config.h"
#include "dinput.h"

// ---------------------------------------------------------------------------
// Proxy exports.
//
// One binary stands in for every DLL the Interposer can be loaded as. Which one
// it is depends purely on what the file is named when it is deployed:
//
//   version.dll   most executables load it implicitly at startup
//   dinput8.dll   for games that import DirectInput 8
//   dinput.dll    for DirectInput 3/7 era games, which commonly import nothing
//                 else that can be proxied
//   *.asi         ASI loaders dispatch on the extension, not on exports
//
// The export table is the union of all three system DLLs. That is safe because
// a game resolves only the names it imports: a game loading this file as
// version.dll never looks up DirectInputCreateA, and never notices it is there.
//
// Every stub forwards to the system DLL that really owns the export, resolved
// lazily on first use from an explicit System32 path. The explicit path is what
// makes renaming safe — LoadLibraryW(L"version.dll") from a file named
// version.dll sitting in the game directory would find itself and recurse.
// ---------------------------------------------------------------------------

namespace {

// Cached system module handles, one per DLL we forward to. Deliberately never
// freed: the game may still be inside a forwarded call at process detach, and
// these are system DLLs that the process is keeping loaded anyway.
HMODULE g_systemModules[3] = {};

enum SystemModule { SysVersion = 0, SysDInput8 = 1, SysDInput = 2 };

const wchar_t* const kSystemModuleNames[] = { L"version.dll", L"dinput8.dll", L"dinput.dll" };

HMODULE SystemModuleHandle(SystemModule which)
{
    if (g_systemModules[which])
        return g_systemModules[which];

    wchar_t path[MAX_PATH]{};

    UINT length = GetSystemDirectoryW(path, MAX_PATH);

    if (length == 0 || length > MAX_PATH - 16)
        return nullptr;

    wcscat_s(path, L"\\");
    wcscat_s(path, kSystemModuleNames[which]);

    g_systemModules[which] = LoadLibraryW(path);

    return g_systemModules[which];
}

FARPROC SystemProc(SystemModule which, const char* name)
{
    HMODULE module = SystemModuleHandle(which);

    return module ? GetProcAddress(module, name) : nullptr;
}

// Resolve `name` from the owning system DLL once and cache it in `cache`.
// Every stub below is a one-liner on top of this.
template <typename Fn>
Fn Resolved(SystemModule which, const char* name, Fn& cache)
{
    if (!cache)
        cache = reinterpret_cast<Fn>(reinterpret_cast<void*>(SystemProc(which, name)));

    return cache;
}

// DllCanUnloadNow, DllGetClassObject, DllRegisterServer and DllUnregisterServer
// are exported by BOTH dinput.dll and dinput8.dll, and a single binary can only
// define each once. Which one a call belongs to is decided by what this file is
// named — the one place where the deployment name genuinely changes behaviour.
// Anything else (version.dll, an .asi) never receives these calls at all, so
// the DirectInput 8 DLL is a harmless default.
SystemModule ComObjectHost()
{
    static SystemModule cached = SysDInput8;
    static bool resolved = false;

    if (resolved)
        return cached;

    resolved = true;

    HMODULE self = nullptr;

    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ComObjectHost), &self) && self)
    {
        wchar_t path[MAX_PATH]{};

        if (GetModuleFileNameW(self, path, MAX_PATH))
        {
            const wchar_t* slash = wcsrchr(path, L'\\');
            const wchar_t* name  = slash ? slash + 1 : path;

            if (_wcsicmp(name, L"dinput.dll") == 0)
                cached = SysDInput;
        }
    }

    return cached;
}

} // namespace

// ---------------------------------------------------------------------------
// version.dll
// ---------------------------------------------------------------------------

#define FORWARD(module, ret, name, params, args, fail)                        \
    extern "C" ret WINAPI name params                                         \
    {                                                                         \
        using Fn = ret (WINAPI*) params;                                      \
        static Fn cache = nullptr;                                            \
        Fn fn = Resolved<Fn>(module, #name, cache);                           \
        return fn ? fn args : fail;                                           \
    }

FORWARD(SysVersion, BOOL, GetFileVersionInfoA,
    (LPCSTR f, DWORD h, DWORD len, LPVOID p), (f, h, len, p), FALSE)

FORWARD(SysVersion, BOOL, GetFileVersionInfoW,
    (LPCWSTR f, DWORD h, DWORD len, LPVOID p), (f, h, len, p), FALSE)

FORWARD(SysVersion, BOOL, GetFileVersionInfoExA,
    (DWORD flags, LPCSTR f, DWORD h, DWORD len, LPVOID p), (flags, f, h, len, p), FALSE)

FORWARD(SysVersion, BOOL, GetFileVersionInfoExW,
    (DWORD flags, LPCWSTR f, DWORD h, DWORD len, LPVOID p), (flags, f, h, len, p), FALSE)

FORWARD(SysVersion, DWORD, GetFileVersionInfoSizeA,
    (LPCSTR f, LPDWORD handle), (f, handle), 0)

FORWARD(SysVersion, DWORD, GetFileVersionInfoSizeW,
    (LPCWSTR f, LPDWORD handle), (f, handle), 0)

FORWARD(SysVersion, DWORD, GetFileVersionInfoSizeExA,
    (DWORD flags, LPCSTR f, LPDWORD handle), (flags, f, handle), 0)

FORWARD(SysVersion, DWORD, GetFileVersionInfoSizeExW,
    (DWORD flags, LPCWSTR f, LPDWORD handle), (flags, f, handle), 0)

// Undocumented, and not in any SDK header — the signature is taken from the
// export as the loader sees it. Present so that a game importing the full
// version.dll export set still binds.
FORWARD(SysVersion, BOOL, GetFileVersionInfoByHandle,
    (HANDLE h, LPCWSTR f, DWORD len, LPVOID p), (h, f, len, p), FALSE)

FORWARD(SysVersion, DWORD, VerFindFileA,
    (DWORD uFlags, LPCSTR szFileName, LPCSTR szWinDir, LPCSTR szAppDir,
     LPSTR szCurDir, PUINT lpuCurDirLen, LPSTR szDestDir, PUINT lpuDestDirLen),
    (uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen), 0)

FORWARD(SysVersion, DWORD, VerFindFileW,
    (DWORD uFlags, LPCWSTR szFileName, LPCWSTR szWinDir, LPCWSTR szAppDir,
     LPWSTR szCurDir, PUINT lpuCurDirLen, LPWSTR szDestDir, PUINT lpuDestDirLen),
    (uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen), 0)

FORWARD(SysVersion, DWORD, VerInstallFileA,
    (DWORD uFlags, LPCSTR szSrcFileName, LPCSTR szDestFileName, LPCSTR szSrcDir,
     LPCSTR szDestDir, LPCSTR szCurDir, LPSTR szTmpFile, PUINT lpuTmpFileLen),
    (uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen), 0)

FORWARD(SysVersion, DWORD, VerInstallFileW,
    (DWORD uFlags, LPCWSTR szSrcFileName, LPCWSTR szDestFileName, LPCWSTR szSrcDir,
     LPCWSTR szDestDir, LPCWSTR szCurDir, LPWSTR szTmpFile, PUINT lpuTmpFileLen),
    (uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen), 0)

FORWARD(SysVersion, DWORD, VerLanguageNameA,
    (DWORD wLang, LPSTR szLang, DWORD nSize), (wLang, szLang, nSize), 0)

FORWARD(SysVersion, DWORD, VerLanguageNameW,
    (DWORD wLang, LPWSTR szLang, DWORD nSize), (wLang, szLang, nSize), 0)

FORWARD(SysVersion, BOOL, VerQueryValueA,
    (LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen),
    (pBlock, lpSubBlock, lplpBuffer, puLen), FALSE)

FORWARD(SysVersion, BOOL, VerQueryValueW,
    (LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen),
    (pBlock, lpSubBlock, lplpBuffer, puLen), FALSE)

// ---------------------------------------------------------------------------
// dinput8.dll
// ---------------------------------------------------------------------------

// Routed through the DirectInput subsystem rather than forwarded blindly, so
// that DirectInput.DeviceFilter and the enumeration logging apply. It falls
// back to the real export when neither is configured.
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion,
    const void* riidltf, LPVOID* ppvOut, void* punkOuter)
{
    return DirectInput8CreateFiltered(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

// Returns the address of dinput8's c_dfDIJoystick data format. Games that link
// the DirectX SDK's dinput8.lib import this, so leaving it out stops them
// loading at all.
FORWARD(SysDInput8, LPVOID, GetdfDIJoystick, (void), (), nullptr)

// ---------------------------------------------------------------------------
// dinput.dll
// ---------------------------------------------------------------------------

extern "C" HRESULT WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD dwVersion,
    LPVOID* ppDI, void* punkOuter)
{
    if (g_diFixLegacyEnum)
        return DirectInputBridgeCreate(hinst, dwVersion, ppDI, punkOuter);

    using Fn = HRESULT (WINAPI*)(HINSTANCE, DWORD, LPVOID*, void*);
    static Fn cache = nullptr;

    Fn fn = Resolved<Fn>(SysDInput, "DirectInputCreateA", cache);

    return fn ? fn(hinst, dwVersion, ppDI, punkOuter) : E_FAIL;
}

// The Unicode entry point is always forwarded: the bridge only handles ANSI
// device enumeration, and no game seen on this path uses it.
FORWARD(SysDInput, HRESULT, DirectInputCreateW,
    (HINSTANCE hinst, DWORD dwVersion, LPVOID* ppDI, void* punkOuter),
    (hinst, dwVersion, ppDI, punkOuter), E_FAIL)

extern "C" HRESULT WINAPI DirectInputCreateEx(HINSTANCE hinst, DWORD dwVersion,
    const void* riid, LPVOID* ppvOut, void* punkOuter)
{
    if (g_diFixLegacyEnum && DirectInputBridgeWantsIid(riid))
        return DirectInputBridgeCreate(hinst, dwVersion, ppvOut, punkOuter);

    using Fn = HRESULT (WINAPI*)(HINSTANCE, DWORD, const void*, LPVOID*, void*);
    static Fn cache = nullptr;

    Fn fn = Resolved<Fn>(SysDInput, "DirectInputCreateEx", cache);

    return fn ? fn(hinst, dwVersion, riid, ppvOut, punkOuter) : E_FAIL;
}

// ---------------------------------------------------------------------------
// Shared between dinput.dll and dinput8.dll — see ComObjectHost above
// ---------------------------------------------------------------------------

extern "C" HRESULT WINAPI DllCanUnloadNow(void)
{
    using Fn = HRESULT (WINAPI*)(void);

    Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(SystemProc(ComObjectHost(), "DllCanUnloadNow")));

    // S_FALSE keeps the DLL resident, which is what we want either way: the
    // DirectInput bridge patches vtables the game is still holding.
    return fn ? fn() : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(const void* rclsid, const void* riid, LPVOID* ppv)
{
    using Fn = HRESULT (WINAPI*)(const void*, const void*, LPVOID*);

    Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(SystemProc(ComObjectHost(), "DllGetClassObject")));

    return fn ? fn(rclsid, riid, ppv) : 0x80040111L; // CLASS_E_CLASSNOTAVAILABLE
}

extern "C" HRESULT WINAPI DllRegisterServer(void)
{
    using Fn = HRESULT (WINAPI*)(void);

    Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(SystemProc(ComObjectHost(), "DllRegisterServer")));

    return fn ? fn() : E_FAIL;
}

extern "C" HRESULT WINAPI DllUnregisterServer(void)
{
    using Fn = HRESULT (WINAPI*)(void);

    Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(SystemProc(ComObjectHost(), "DllUnregisterServer")));

    return fn ? fn() : E_FAIL;
}

#undef FORWARD
