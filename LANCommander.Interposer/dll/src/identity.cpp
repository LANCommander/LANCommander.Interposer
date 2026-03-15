#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include <string>

#include "identity.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Trampoline types & pointers
// ---------------------------------------------------------------------------
using FnGetUserNameW     = BOOL(WINAPI*)(LPWSTR, LPDWORD);
using FnGetUserNameA     = BOOL(WINAPI*)(LPSTR,  LPDWORD);
using FnGetComputerNameW = BOOL(WINAPI*)(LPWSTR, LPDWORD);
using FnGetComputerNameA = BOOL(WINAPI*)(LPSTR,  LPDWORD);

static FnGetUserNameW     g_origGetUserNameW     = nullptr;
static FnGetUserNameA     g_origGetUserNameA     = nullptr;
static FnGetComputerNameW g_origGetComputerNameW = nullptr;
static FnGetComputerNameA g_origGetComputerNameA = nullptr;

// ---------------------------------------------------------------------------
// Helper — read a named MMF written by the injector (UTF-8 encoded string)
// ---------------------------------------------------------------------------
static std::wstring ReadMmfString(const wchar_t* mmfName, DWORD maxBytes)
{
    HANDLE hMMF = OpenFileMappingW(FILE_MAP_READ, FALSE, mmfName);
    
    if (!hMMF)
        return {};

    std::wstring result;
    void* view = MapViewOfFile(hMMF, FILE_MAP_READ, 0, 0, maxBytes);
    if (view)
    {
        const char* utf8 = static_cast<const char*>(view);
        
        int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        
        if (wideLength > 1)
        {
            result.resize(static_cast<size_t>(wideLength - 1));
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result.data(), wideLength);
        }
        
        UnmapViewOfFile(view);
    }
    
    CloseHandle(hMMF);
    
    return result;
}

// ---------------------------------------------------------------------------
// Hook implementations — GetUserName (advapi32)
//
// Buffer contract (matches real API):
//   *pcbBuffer in/out: characters, including null terminator.
//   On failure: *pcbBuffer = needed, ERROR_INSUFFICIENT_BUFFER.
//   On success: *pcbBuffer = needed (chars incl. null).
// ---------------------------------------------------------------------------
static BOOL WINAPI HookGetUserNameW(LPWSTR lpBuffer, LPDWORD pcbBuffer)
{
    if (!pcbBuffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    DWORD needed = static_cast<DWORD>(g_username.size() + 1);

    if (!lpBuffer || *pcbBuffer < needed)
    {
        *pcbBuffer = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    wmemcpy(lpBuffer, g_username.c_str(), needed);
    *pcbBuffer = needed;
    
    return TRUE;
}

static BOOL WINAPI HookGetUserNameA(LPSTR lpBuffer, LPDWORD pcbBuffer)
{
    if (!pcbBuffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    int ansiLength = WideCharToMultiByte(CP_ACP, 0, g_username.c_str(), -1,
        nullptr, 0, nullptr, nullptr);

    if (ansiLength <= 0)
        return g_origGetUserNameA(lpBuffer, pcbBuffer);

    DWORD needed = static_cast<DWORD>(ansiLength);

    if (!lpBuffer || *pcbBuffer < needed)
    {
        *pcbBuffer = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        
        return FALSE;
    }

    WideCharToMultiByte(CP_ACP, 0, g_username.c_str(), -1,
        lpBuffer, static_cast<int>(*pcbBuffer), nullptr, nullptr);
    *pcbBuffer = needed;
    
    return TRUE;
}

// ---------------------------------------------------------------------------
// Hook implementations — GetComputerName (kernel32)
//
// Buffer contract (matches real API):
//   *nSize in: characters available (including null terminator).
//   On failure: *nSize = required chars including null, ERROR_BUFFER_OVERFLOW.
//   On success: *nSize = characters written, NOT including null terminator.
// ---------------------------------------------------------------------------
static BOOL WINAPI HookGetComputerNameW(LPWSTR lpBuffer, LPDWORD nSize)
{
    if (!nSize)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        
        return FALSE;
    }

    DWORD nameLength = static_cast<DWORD>(g_computername.size()); // without null
    DWORD needed  = nameLength + 1;                               // with null

    if (!lpBuffer || *nSize < needed)
    {
        *nSize = needed;
        SetLastError(ERROR_BUFFER_OVERFLOW);
        
        return FALSE;
    }

    wmemcpy(lpBuffer, g_computername.c_str(), needed);
    *nSize = nameLength; // success: count without null terminator
    
    return TRUE;
}

static BOOL WINAPI HookGetComputerNameA(LPSTR lpBuffer, LPDWORD nSize)
{
    if (!nSize)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    int ansiLength = WideCharToMultiByte(CP_ACP, 0, g_computername.c_str(), -1,
        nullptr, 0, nullptr, nullptr);

    if (ansiLength <= 0)
        return g_origGetComputerNameA(lpBuffer, nSize);

    DWORD nameLength = static_cast<DWORD>(ansiLength - 1); // without null
    DWORD needed  = static_cast<DWORD>(ansiLength);     // with null

    if (!lpBuffer || *nSize < needed)
    {
        *nSize = needed;
        SetLastError(ERROR_BUFFER_OVERFLOW);
        
        return FALSE;
    }

    WideCharToMultiByte(CP_ACP, 0, g_computername.c_str(), -1,
        lpBuffer, static_cast<int>(*nSize), nullptr, nullptr);
    *nSize = nameLength; // success: count without null terminator
    
    return TRUE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InstallIdentityHooks()
{
    DWORD pid = GetCurrentProcessId();
    wchar_t mmfName[64]{};

    // Username override from injector MMF (Local\InterposerUsername_<pid>)
    wsprintfW(mmfName, L"Local\\InterposerUsername_%lu", pid);
    {
        std::wstring override = ReadMmfString(mmfName, 512);
        
        if (!override.empty())
            g_username = std::move(override);
    }

    // Computer name override from injector MMF (Local\InterposerComputerName_<pid>)
    wsprintfW(mmfName, L"Local\\InterposerComputerName_%lu", pid);
    {
        std::wstring override = ReadMmfString(mmfName, 512);
        
        if (!override.empty())
            g_computername = std::move(override);
    }

    if (!g_username.empty())
    {
        MH_CreateHookApi(L"advapi32", "GetUserNameW",
            reinterpret_cast<LPVOID>(HookGetUserNameW),
            reinterpret_cast<LPVOID*>(&g_origGetUserNameW));

        MH_CreateHookApi(L"advapi32", "GetUserNameA",
            reinterpret_cast<LPVOID>(HookGetUserNameA),
            reinterpret_cast<LPVOID*>(&g_origGetUserNameA));
    }

    if (!g_computername.empty())
    {
        MH_CreateHookApi(L"kernel32", "GetComputerNameW",
            reinterpret_cast<LPVOID>(HookGetComputerNameW),
            reinterpret_cast<LPVOID*>(&g_origGetComputerNameW));

        MH_CreateHookApi(L"kernel32", "GetComputerNameA",
            reinterpret_cast<LPVOID>(HookGetComputerNameA),
            reinterpret_cast<LPVOID*>(&g_origGetComputerNameA));
    }
}
