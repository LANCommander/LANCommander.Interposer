#include "files.h"
#include "config.h"
#include "fastdl.h"

#include <windows.h>
#include <MinHook.h>
#include <string>

// Reentrancy guard: FastDL downloads call CreateFileW internally (to write
// temp files, read local files for CRC, etc.). We must not re-enter the hook
// chain when we are already handling a FastDL operation on this thread.
static thread_local bool g_inFastDLHook = false;

// ---------------------------------------------------------------------------
// Trampoline types & pointers
// ---------------------------------------------------------------------------
using FnCreateFileW        = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using FnCreateFileA        = HANDLE(WINAPI*)(LPCSTR,  DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using FnGetFileAttributesW = DWORD(WINAPI*)(LPCWSTR);
using FnGetFileAttributesA = DWORD(WINAPI*)(LPCSTR);

static FnCreateFileW        g_origCreateFileW        = nullptr;
static FnCreateFileA        g_origCreateFileA        = nullptr;
static FnGetFileAttributesW g_origGetFileAttributesW = nullptr;
static FnGetFileAttributesA g_origGetFileAttributesA = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static const wchar_t* AccessVerb(DWORD access)
{
    bool r = (access & (GENERIC_READ  | FILE_READ_DATA))                    != 0;
    bool w = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    if (w && r) return L"[FILE R/W]     ";
    if (w)      return L"[FILE WRITE]   ";
    return             L"[FILE READ]    ";
}

// Core wide-path implementation shared by both W and A CreateFile hooks.
static HANDLE CreateFileWImpl(
    const std::wstring& path,
    DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSA,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    std::wstring redirected = ApplyFileRedirects(path);

    if (redirected != path)
        LogFileAccess(L"[FILE REDIRECT]", path.c_str(), redirected.c_str());
    else
        LogFileAccess(AccessVerb(dwDesiredAccess), path.c_str());

    // FastDL: for read-only opens, ensure the file is downloaded if eligible.
    // Guard against reentrancy caused by CreateFileW calls inside TryFastDLDownload.
    if (!g_inFastDLHook &&
        !(dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)))
    {
        g_inFastDLHook = true;
        TryFastDLDownload(redirected);
        g_inFastDLHook = false;
    }

    return g_origCreateFileW(
        redirected.c_str(), dwDesiredAccess, dwShareMode,
        lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

// ---------------------------------------------------------------------------
// Hook implementations
// ---------------------------------------------------------------------------
static HANDLE WINAPI HookCreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSA, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (!lpFileName)
        return g_origCreateFileW(nullptr, dwDesiredAccess, dwShareMode,
            lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    return CreateFileWImpl(lpFileName, dwDesiredAccess, dwShareMode,
        lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

static HANDLE WINAPI HookCreateFileA(
    LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSA, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (!lpFileName)
        return g_origCreateFileA(nullptr, dwDesiredAccess, dwShareMode,
            lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    int wlen = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, nullptr, 0);
    if (wlen <= 0)
        return g_origCreateFileA(lpFileName, dwDesiredAccess, dwShareMode,
            lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    std::wstring wpath(wlen - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wpath.data(), wlen);

    // CreateFileWImpl always calls g_origCreateFileW (the real kernel32 function),
    // which accepts wide paths, so this correctly handles ANSI → wide redirections.
    return CreateFileWImpl(wpath, dwDesiredAccess, dwShareMode,
        lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

static DWORD WINAPI HookGetFileAttributesW(LPCWSTR lpFileName)
{
    if (!lpFileName)
        return g_origGetFileAttributesW(nullptr);

    std::wstring path(lpFileName);
    std::wstring redirected = ApplyFileRedirects(path);

    if (redirected != path)
    {
        LogFileAccess(L"[FILE REDIRECT]", path.c_str(), redirected.c_str());
        return g_origGetFileAttributesW(redirected.c_str());
    }

    LogFileAccess(L"[FILE ATTR]    ", path.c_str());

    DWORD attrs = g_origGetFileAttributesW(lpFileName);

    // FastDL: if the file doesn't exist locally but is available on the server,
    // report FILE_ATTRIBUTE_NORMAL so the game proceeds to open it (which will
    // trigger the actual download in CreateFileW).
    if (attrs == INVALID_FILE_ATTRIBUTES && !g_inFastDLHook)
    {
        g_inFastDLHook = true;
        bool exists = FastDLFileExists(path);
        g_inFastDLHook = false;

        if (exists)
            return FILE_ATTRIBUTE_NORMAL;
    }

    return attrs;
}

static DWORD WINAPI HookGetFileAttributesA(LPCSTR lpFileName)
{
    if (!lpFileName)
        return g_origGetFileAttributesA(nullptr);

    int wlen = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, nullptr, 0);
    if (wlen <= 0)
        return g_origGetFileAttributesA(lpFileName);

    std::wstring wpath(wlen - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wpath.data(), wlen);

    std::wstring redirected = ApplyFileRedirects(wpath);

    if (redirected != wpath)
    {
        LogFileAccess(L"[FILE REDIRECT]", wpath.c_str(), redirected.c_str());
        return g_origGetFileAttributesW(redirected.c_str());
    }

    LogFileAccess(L"[FILE ATTR]    ", wpath.c_str());

    DWORD attrs = g_origGetFileAttributesA(lpFileName);

    // FastDL: same existence check as the W variant
    if (attrs == INVALID_FILE_ATTRIBUTES && !g_inFastDLHook)
    {
        g_inFastDLHook = true;
        bool exists = FastDLFileExists(wpath);
        g_inFastDLHook = false;

        if (exists)
            return FILE_ATTRIBUTE_NORMAL;
    }

    return attrs;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InstallFileHooks()
{
    MH_CreateHookApi(L"kernel32", "CreateFileW",
        reinterpret_cast<LPVOID>(HookCreateFileW),
        reinterpret_cast<LPVOID*>(&g_origCreateFileW));

    MH_CreateHookApi(L"kernel32", "CreateFileA",
        reinterpret_cast<LPVOID>(HookCreateFileA),
        reinterpret_cast<LPVOID*>(&g_origCreateFileA));

    MH_CreateHookApi(L"kernel32", "GetFileAttributesW",
        reinterpret_cast<LPVOID>(HookGetFileAttributesW),
        reinterpret_cast<LPVOID*>(&g_origGetFileAttributesW));

    MH_CreateHookApi(L"kernel32", "GetFileAttributesA",
        reinterpret_cast<LPVOID>(HookGetFileAttributesA),
        reinterpret_cast<LPVOID*>(&g_origGetFileAttributesA));
}
