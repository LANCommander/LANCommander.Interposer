#ifdef INTERPOSER_PROXY

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static HMODULE g_realVersion = nullptr;

// Function pointer types
typedef BOOL  (WINAPI* PFN_GetFileVersionInfoA)      (LPCSTR,  DWORD, DWORD, LPVOID);
typedef BOOL  (WINAPI* PFN_GetFileVersionInfoW)      (LPCWSTR, DWORD, DWORD, LPVOID);
typedef BOOL  (WINAPI* PFN_GetFileVersionInfoExA)    (DWORD, LPCSTR,  DWORD, DWORD, LPVOID);
typedef BOOL  (WINAPI* PFN_GetFileVersionInfoExW)    (DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
typedef DWORD (WINAPI* PFN_GetFileVersionInfoSizeA)  (LPCSTR,  LPDWORD);
typedef DWORD (WINAPI* PFN_GetFileVersionInfoSizeW)  (LPCWSTR, LPDWORD);
typedef DWORD (WINAPI* PFN_GetFileVersionInfoSizeExA)(DWORD, LPCSTR,  LPDWORD);
typedef DWORD (WINAPI* PFN_GetFileVersionInfoSizeExW)(DWORD, LPCWSTR, LPDWORD);
typedef DWORD (WINAPI* PFN_VerFindFileA)  (DWORD, LPCSTR,  LPCSTR,  LPCSTR,  LPSTR,  PUINT, LPSTR,  PUINT);
typedef DWORD (WINAPI* PFN_VerFindFileW)  (DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
typedef DWORD (WINAPI* PFN_VerInstallFileA)(DWORD, LPCSTR,  LPCSTR,  LPCSTR,  LPCSTR,  LPCSTR,  LPSTR,  PUINT);
typedef DWORD (WINAPI* PFN_VerInstallFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
typedef DWORD (WINAPI* PFN_VerLanguageNameA)(DWORD, LPSTR,  DWORD);
typedef DWORD (WINAPI* PFN_VerLanguageNameW)(DWORD, LPWSTR, DWORD);
typedef BOOL  (WINAPI* PFN_VerQueryValueA) (LPCVOID, LPCSTR,  LPVOID*, PUINT);
typedef BOOL  (WINAPI* PFN_VerQueryValueW) (LPCVOID, LPCWSTR, LPVOID*, PUINT);

static PFN_GetFileVersionInfoA       pfn_GetFileVersionInfoA       = nullptr;
static PFN_GetFileVersionInfoW       pfn_GetFileVersionInfoW       = nullptr;
static PFN_GetFileVersionInfoExA     pfn_GetFileVersionInfoExA     = nullptr;
static PFN_GetFileVersionInfoExW     pfn_GetFileVersionInfoExW     = nullptr;
static PFN_GetFileVersionInfoSizeA   pfn_GetFileVersionInfoSizeA   = nullptr;
static PFN_GetFileVersionInfoSizeW   pfn_GetFileVersionInfoSizeW   = nullptr;
static PFN_GetFileVersionInfoSizeExA pfn_GetFileVersionInfoSizeExA = nullptr;
static PFN_GetFileVersionInfoSizeExW pfn_GetFileVersionInfoSizeExW = nullptr;
static PFN_VerFindFileA              pfn_VerFindFileA              = nullptr;
static PFN_VerFindFileW              pfn_VerFindFileW              = nullptr;
static PFN_VerInstallFileA           pfn_VerInstallFileA           = nullptr;
static PFN_VerInstallFileW           pfn_VerInstallFileW           = nullptr;
static PFN_VerLanguageNameA          pfn_VerLanguageNameA          = nullptr;
static PFN_VerLanguageNameW          pfn_VerLanguageNameW          = nullptr;
static PFN_VerQueryValueA            pfn_VerQueryValueA            = nullptr;
static PFN_VerQueryValueW            pfn_VerQueryValueW            = nullptr;

void InitProxy()
{
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\version.dll");

    g_realVersion = LoadLibraryW(path);
    if (!g_realVersion)
        return;

#define RESOLVE(name) pfn_##name = reinterpret_cast<PFN_##name>(GetProcAddress(g_realVersion, #name))
    RESOLVE(GetFileVersionInfoA);
    RESOLVE(GetFileVersionInfoW);
    RESOLVE(GetFileVersionInfoExA);
    RESOLVE(GetFileVersionInfoExW);
    RESOLVE(GetFileVersionInfoSizeA);
    RESOLVE(GetFileVersionInfoSizeW);
    RESOLVE(GetFileVersionInfoSizeExA);
    RESOLVE(GetFileVersionInfoSizeExW);
    RESOLVE(VerFindFileA);
    RESOLVE(VerFindFileW);
    RESOLVE(VerInstallFileA);
    RESOLVE(VerInstallFileW);
    RESOLVE(VerLanguageNameA);
    RESOLVE(VerLanguageNameW);
    RESOLVE(VerQueryValueA);
    RESOLVE(VerQueryValueW);
#undef RESOLVE
}

void UninitProxy()
{
    if (g_realVersion)
    {
        FreeLibrary(g_realVersion);
        g_realVersion = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Export stubs — forward every call to the real system version.dll
// ---------------------------------------------------------------------------

extern "C" BOOL WINAPI GetFileVersionInfoA(LPCSTR f, DWORD h, DWORD len, LPVOID p)
    { return pfn_GetFileVersionInfoA ? pfn_GetFileVersionInfoA(f, h, len, p) : FALSE; }

extern "C" BOOL WINAPI GetFileVersionInfoW(LPCWSTR f, DWORD h, DWORD len, LPVOID p)
    { return pfn_GetFileVersionInfoW ? pfn_GetFileVersionInfoW(f, h, len, p) : FALSE; }

extern "C" BOOL WINAPI GetFileVersionInfoExA(DWORD flags, LPCSTR f, DWORD h, DWORD len, LPVOID p)
    { return pfn_GetFileVersionInfoExA ? pfn_GetFileVersionInfoExA(flags, f, h, len, p) : FALSE; }

extern "C" BOOL WINAPI GetFileVersionInfoExW(DWORD flags, LPCWSTR f, DWORD h, DWORD len, LPVOID p)
    { return pfn_GetFileVersionInfoExW ? pfn_GetFileVersionInfoExW(flags, f, h, len, p) : FALSE; }

extern "C" DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR f, LPDWORD handle)
    { return pfn_GetFileVersionInfoSizeA ? pfn_GetFileVersionInfoSizeA(f, handle) : 0; }

extern "C" DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR f, LPDWORD handle)
    { return pfn_GetFileVersionInfoSizeW ? pfn_GetFileVersionInfoSizeW(f, handle) : 0; }

extern "C" DWORD WINAPI GetFileVersionInfoSizeExA(DWORD flags, LPCSTR f, LPDWORD handle)
    { return pfn_GetFileVersionInfoSizeExA ? pfn_GetFileVersionInfoSizeExA(flags, f, handle) : 0; }

extern "C" DWORD WINAPI GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR f, LPDWORD handle)
    { return pfn_GetFileVersionInfoSizeExW ? pfn_GetFileVersionInfoSizeExW(flags, f, handle) : 0; }

extern "C" DWORD WINAPI VerFindFileA(DWORD uFlags,
    LPCSTR szFileName, LPCSTR szWinDir, LPCSTR szAppDir,
    LPSTR szCurDir, PUINT lpuCurDirLen, LPSTR szDestDir, PUINT lpuDestDirLen)
{
    return pfn_VerFindFileA
        ? pfn_VerFindFileA(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen)
        : 0;
}

extern "C" DWORD WINAPI VerFindFileW(DWORD uFlags,
    LPCWSTR szFileName, LPCWSTR szWinDir, LPCWSTR szAppDir,
    LPWSTR szCurDir, PUINT lpuCurDirLen, LPWSTR szDestDir, PUINT lpuDestDirLen)
{
    return pfn_VerFindFileW
        ? pfn_VerFindFileW(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen)
        : 0;
}

extern "C" DWORD WINAPI VerInstallFileA(DWORD uFlags,
    LPCSTR szSrcFileName, LPCSTR szDestFileName,
    LPCSTR szSrcDir, LPCSTR szDestDir, LPCSTR szCurDir,
    LPSTR szTmpFile, PUINT lpuTmpFileLen)
{
    return pfn_VerInstallFileA
        ? pfn_VerInstallFileA(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen)
        : 0;
}

extern "C" DWORD WINAPI VerInstallFileW(DWORD uFlags,
    LPCWSTR szSrcFileName, LPCWSTR szDestFileName,
    LPCWSTR szSrcDir, LPCWSTR szDestDir, LPCWSTR szCurDir,
    LPWSTR szTmpFile, PUINT lpuTmpFileLen)
{
    return pfn_VerInstallFileW
        ? pfn_VerInstallFileW(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen)
        : 0;
}

extern "C" DWORD WINAPI VerLanguageNameA(DWORD wLang, LPSTR szLang, DWORD nSize)
    { return pfn_VerLanguageNameA ? pfn_VerLanguageNameA(wLang, szLang, nSize) : 0; }

extern "C" DWORD WINAPI VerLanguageNameW(DWORD wLang, LPWSTR szLang, DWORD nSize)
    { return pfn_VerLanguageNameW ? pfn_VerLanguageNameW(wLang, szLang, nSize) : 0; }

extern "C" BOOL WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen)
    { return pfn_VerQueryValueA ? pfn_VerQueryValueA(pBlock, lpSubBlock, lplpBuffer, puLen) : FALSE; }

extern "C" BOOL WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen)
    { return pfn_VerQueryValueW ? pfn_VerQueryValueW(pBlock, lpSubBlock, lplpBuffer, puLen) : FALSE; }

#endif // INTERPOSER_PROXY
