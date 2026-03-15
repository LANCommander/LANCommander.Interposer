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
using FnFindFirstFileW     = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FnFindFirstFileA     = HANDLE(WINAPI*)(LPCSTR,  LPWIN32_FIND_DATAA);
using FnLoadLibraryW       = HMODULE(WINAPI*)(LPCWSTR);
using FnLoadLibraryA       = HMODULE(WINAPI*)(LPCSTR);
using FnLoadLibraryExW     = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using FnLoadLibraryExA     = HMODULE(WINAPI*)(LPCSTR,  HANDLE, DWORD);

static FnCreateFileW        g_origCreateFileW        = nullptr;
static FnCreateFileA        g_origCreateFileA        = nullptr;
static FnGetFileAttributesW g_origGetFileAttributesW = nullptr;
static FnGetFileAttributesA g_origGetFileAttributesA = nullptr;
static FnFindFirstFileW     g_origFindFirstFileW     = nullptr;
static FnFindFirstFileA     g_origFindFirstFileA     = nullptr;
static FnLoadLibraryW       g_origLoadLibraryW       = nullptr;
static FnLoadLibraryA       g_origLoadLibraryA       = nullptr;
static FnLoadLibraryExW     g_origLoadLibraryExW     = nullptr;
static FnLoadLibraryExA     g_origLoadLibraryExA     = nullptr;

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
    // Suppress all hook logic during internal FastDL operations to avoid
    // reentrancy, redundant logging, and spurious redirect checks.
    if (g_inFastDLHook)
        return g_origCreateFileW(path.c_str(), dwDesiredAccess, dwShareMode,
            lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    std::wstring redirected = ApplyFileRedirects(path);

    if (redirected != path)
        LogFileAccess(L"[FILE REDIRECT]", path.c_str(), redirected.c_str());
    else
        LogFileAccess(AccessVerb(dwDesiredAccess), path.c_str());

    // FastDL: for read-only opens, ensure the file is downloaded (or cached) if eligible.
    if (!(dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)))
    {
        g_inFastDLHook = true;
        TryFastDLDownload(redirected);

        // Serve from the overlay directory if a cached copy exists there
        std::wstring overlayPath = GetExistingOverlayPath(redirected);
        g_inFastDLHook = false;

        if (!overlayPath.empty())
        {
            LogFileAccess(L"[FILE OVERLAY] ", redirected.c_str(), overlayPath.c_str());
            return g_origCreateFileW(overlayPath.c_str(), dwDesiredAccess, dwShareMode,
                lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
        }
    }

    return g_origCreateFileW(
        redirected.c_str(), dwDesiredAccess, dwShareMode,
        lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

// ---------------------------------------------------------------------------
// Hook implementations — CreateFile
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

// ---------------------------------------------------------------------------
// Hook implementations — GetFileAttributes
// ---------------------------------------------------------------------------
static DWORD WINAPI HookGetFileAttributesW(LPCWSTR lpFileName)
{
    if (!lpFileName)
        return g_origGetFileAttributesW(nullptr);

    // Suppress all hook logic during internal FastDL operations
    if (g_inFastDLHook)
        return g_origGetFileAttributesW(lpFileName);

    std::wstring path(lpFileName);
    std::wstring redirected = ApplyFileRedirects(path);

    if (redirected != path)
    {
        LogFileAccess(L"[FILE REDIRECT]", path.c_str(), redirected.c_str());
        return g_origGetFileAttributesW(redirected.c_str());
    }

    LogFileAccess(L"[FILE ATTR]    ", path.c_str());

    DWORD attrs = g_origGetFileAttributesW(lpFileName);

    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        // Check overlay cache first (cheaper than a network request)
        g_inFastDLHook = true;
        std::wstring overlayPath = GetExistingOverlayPath(path);
        g_inFastDLHook = false;

        if (!overlayPath.empty())
            return g_origGetFileAttributesW(overlayPath.c_str());

        // File not in overlay — ask the FastDL server if it can supply it.
        // Report FILE_ATTRIBUTE_NORMAL so the game proceeds to open it;
        // the actual download happens in CreateFileW.
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

    // Suppress all hook logic during internal FastDL operations
    if (g_inFastDLHook)
        return g_origGetFileAttributesA(lpFileName);

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

    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        // Check overlay cache first
        g_inFastDLHook = true;
        std::wstring overlayPath = GetExistingOverlayPath(wpath);
        g_inFastDLHook = false;

        if (!overlayPath.empty())
            return g_origGetFileAttributesW(overlayPath.c_str());

        // FastDL server existence check
        g_inFastDLHook = true;
        bool exists = FastDLFileExists(wpath);
        g_inFastDLHook = false;

        if (exists)
            return FILE_ATTRIBUTE_NORMAL;
    }

    return attrs;
}

// ---------------------------------------------------------------------------
// Hook implementations — FindFirstFile
// ---------------------------------------------------------------------------
static HANDLE WINAPI HookFindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData)
{
    if (!lpFileName)
        return g_origFindFirstFileW(lpFileName, lpFindFileData);

    std::wstring path(lpFileName);
    std::wstring redirected = ApplyFileRedirects(path);

    if (redirected != path)
    {
        LogFileAccess(L"[FILE REDIRECT]", path.c_str(), redirected.c_str());
        return g_origFindFirstFileW(redirected.c_str(), lpFindFileData);
    }

    LogFileAccess(L"[FILE FIND]    ", path.c_str());
    return g_origFindFirstFileW(lpFileName, lpFindFileData);
}

static HANDLE WINAPI HookFindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    if (!lpFileName)
        return g_origFindFirstFileA(lpFileName, lpFindFileData);

    int wideLength = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, nullptr, 0);
    if (wideLength <= 0)
        return g_origFindFirstFileA(lpFileName, lpFindFileData);

    std::wstring widePath(wideLength - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, widePath.data(), wideLength);

    std::wstring redirected = ApplyFileRedirects(widePath);

    if (redirected != widePath)
    {
        LogFileAccess(L"[FILE REDIRECT]", widePath.c_str(), redirected.c_str());
        // Convert redirected wide path back to ANSI for the A trampoline
        int ansiLength = WideCharToMultiByte(CP_ACP, 0, redirected.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (ansiLength > 1)
        {
            std::string ansiPath(static_cast<size_t>(ansiLength - 1), '\0');
            WideCharToMultiByte(CP_ACP, 0, redirected.c_str(), -1, ansiPath.data(), ansiLength, nullptr, nullptr);
            return g_origFindFirstFileA(ansiPath.c_str(), lpFindFileData);
        }
        // Fallback if the redirected path can't be represented in ANSI
        return g_origFindFirstFileA(lpFileName, lpFindFileData);
    }

    LogFileAccess(L"[FILE FIND]    ", widePath.c_str());
    return g_origFindFirstFileA(lpFileName, lpFindFileData);
}

// ---------------------------------------------------------------------------
// Hook implementations — LoadLibrary
// ---------------------------------------------------------------------------
static HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpLibFileName)
{
    if (!lpLibFileName)
        return g_origLoadLibraryW(nullptr);

    std::wstring path(lpLibFileName);
    std::wstring redirected = ApplyFileRedirects(path);

    if (redirected != path)
        LogFileAccess(L"[FILE REDIRECT]", path.c_str(), redirected.c_str());
    else
        LogFileAccess(L"[DLL LOAD]     ", path.c_str());

    return g_origLoadLibraryW(redirected.c_str());
}

static HMODULE WINAPI HookLoadLibraryA(LPCSTR lpLibFileName)
{
    if (!lpLibFileName)
        return g_origLoadLibraryA(nullptr);

    int wideLength = MultiByteToWideChar(CP_ACP, 0, lpLibFileName, -1, nullptr, 0);
    if (wideLength <= 0)
        return g_origLoadLibraryA(lpLibFileName);

    std::wstring widePath(wideLength - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, lpLibFileName, -1, widePath.data(), wideLength);

    std::wstring redirected = ApplyFileRedirects(widePath);

    if (redirected != widePath)
    {
        LogFileAccess(L"[FILE REDIRECT]", widePath.c_str(), redirected.c_str());
        // Use the W trampoline since we already have a wide redirected path
        return g_origLoadLibraryW(redirected.c_str());
    }

    LogFileAccess(L"[DLL LOAD]     ", widePath.c_str());
    return g_origLoadLibraryA(lpLibFileName);
}

static HMODULE WINAPI HookLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    if (!lpLibFileName)
        return g_origLoadLibraryExW(nullptr, hFile, dwFlags);

    std::wstring path(lpLibFileName);
    std::wstring redirected = ApplyFileRedirects(path);

    if (redirected != path)
        LogFileAccess(L"[FILE REDIRECT]", path.c_str(), redirected.c_str());
    else
        LogFileAccess(L"[DLL LOAD]     ", path.c_str());

    return g_origLoadLibraryExW(redirected.c_str(), hFile, dwFlags);
}

static HMODULE WINAPI HookLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    if (!lpLibFileName)
        return g_origLoadLibraryExA(nullptr, hFile, dwFlags);

    int wideLength = MultiByteToWideChar(CP_ACP, 0, lpLibFileName, -1, nullptr, 0);
    if (wideLength <= 0)
        return g_origLoadLibraryExA(lpLibFileName, hFile, dwFlags);

    std::wstring widePath(wideLength - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, lpLibFileName, -1, widePath.data(), wideLength);

    std::wstring redirected = ApplyFileRedirects(widePath);

    if (redirected != widePath)
    {
        LogFileAccess(L"[FILE REDIRECT]", widePath.c_str(), redirected.c_str());
        return g_origLoadLibraryExW(redirected.c_str(), hFile, dwFlags);
    }

    LogFileAccess(L"[DLL LOAD]     ", widePath.c_str());
    return g_origLoadLibraryExA(lpLibFileName, hFile, dwFlags);
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

    MH_CreateHookApi(L"kernel32", "FindFirstFileW",
        reinterpret_cast<LPVOID>(HookFindFirstFileW),
        reinterpret_cast<LPVOID*>(&g_origFindFirstFileW));

    MH_CreateHookApi(L"kernel32", "FindFirstFileA",
        reinterpret_cast<LPVOID>(HookFindFirstFileA),
        reinterpret_cast<LPVOID*>(&g_origFindFirstFileA));

    MH_CreateHookApi(L"kernel32", "LoadLibraryW",
        reinterpret_cast<LPVOID>(HookLoadLibraryW),
        reinterpret_cast<LPVOID*>(&g_origLoadLibraryW));

    MH_CreateHookApi(L"kernel32", "LoadLibraryA",
        reinterpret_cast<LPVOID>(HookLoadLibraryA),
        reinterpret_cast<LPVOID*>(&g_origLoadLibraryA));

    MH_CreateHookApi(L"kernel32", "LoadLibraryExW",
        reinterpret_cast<LPVOID>(HookLoadLibraryExW),
        reinterpret_cast<LPVOID*>(&g_origLoadLibraryExW));

    MH_CreateHookApi(L"kernel32", "LoadLibraryExA",
        reinterpret_cast<LPVOID>(HookLoadLibraryExA),
        reinterpret_cast<LPVOID*>(&g_origLoadLibraryExA));
}
