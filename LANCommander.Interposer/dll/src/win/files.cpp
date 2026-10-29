#include "files.h"
#include "callbacks.h"
#include "config.h"
#include "fastdl.h"
#include "file_access.h"
#include "hooks.h"

#include <windows.h>
#include "minhook_support.h"
#include <string>

// Internal file access -- a FastDL download writing its temp file, reading a
// file back for its CRC -- passes through every hook here untouched; see
// InternalFileAccess in file_access.h.

// ---------------------------------------------------------------------------
// Trampoline types & pointers
// ---------------------------------------------------------------------------
using FnCreateFileW        = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using FnCreateFileA        = HANDLE(WINAPI*)(LPCSTR,  DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using FnCreateFile2        = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using FnGetFileAttributesW = DWORD(WINAPI*)(LPCWSTR);
using FnGetFileAttributesA = DWORD(WINAPI*)(LPCSTR);
using FnGetFileAttributesExW = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using FnGetFileAttributesExA = BOOL(WINAPI*)(LPCSTR,  GET_FILEEX_INFO_LEVELS, LPVOID);
using FnFindFirstFileW     = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FnFindFirstFileA     = HANDLE(WINAPI*)(LPCSTR,  LPWIN32_FIND_DATAA);
using FnFindFirstFileExW   = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FnFindFirstFileExA   = HANDLE(WINAPI*)(LPCSTR,  FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FnDeleteFileW        = BOOL(WINAPI*)(LPCWSTR);
using FnDeleteFileA        = BOOL(WINAPI*)(LPCSTR);
using FnMoveFileW          = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
using FnMoveFileA          = BOOL(WINAPI*)(LPCSTR,  LPCSTR);
using FnMoveFileExW        = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using FnMoveFileExA        = BOOL(WINAPI*)(LPCSTR,  LPCSTR,  DWORD);
using FnCopyFileW          = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
using FnCopyFileA          = BOOL(WINAPI*)(LPCSTR,  LPCSTR,  BOOL);
using FnCopyFileExW        = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
using FnCopyFileExA        = BOOL(WINAPI*)(LPCSTR,  LPCSTR,  LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
using FnLoadLibraryW       = HMODULE(WINAPI*)(LPCWSTR);
using FnLoadLibraryA       = HMODULE(WINAPI*)(LPCSTR);
using FnLoadLibraryExW     = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using FnLoadLibraryExA     = HMODULE(WINAPI*)(LPCSTR,  HANDLE, DWORD);

static FnCreateFileW        g_origCreateFileW        = nullptr;
static FnCreateFileA        g_origCreateFileA        = nullptr;
static FnCreateFile2        g_origCreateFile2        = nullptr;
static FnGetFileAttributesW g_origGetFileAttributesW = nullptr;
static FnGetFileAttributesA g_origGetFileAttributesA = nullptr;
static FnGetFileAttributesExW g_origGetFileAttributesExW = nullptr;
static FnGetFileAttributesExA g_origGetFileAttributesExA = nullptr;
static FnFindFirstFileW     g_origFindFirstFileW     = nullptr;
static FnFindFirstFileA     g_origFindFirstFileA     = nullptr;
static FnFindFirstFileExW   g_origFindFirstFileExW   = nullptr;
static FnFindFirstFileExA   g_origFindFirstFileExA   = nullptr;
static FnDeleteFileW        g_origDeleteFileW        = nullptr;
static FnDeleteFileA        g_origDeleteFileA        = nullptr;
static FnMoveFileW          g_origMoveFileW          = nullptr;
static FnMoveFileA          g_origMoveFileA          = nullptr;
static FnMoveFileExW        g_origMoveFileExW        = nullptr;
static FnMoveFileExA        g_origMoveFileExA        = nullptr;
static FnCopyFileW          g_origCopyFileW          = nullptr;
static FnCopyFileA          g_origCopyFileA          = nullptr;
static FnCopyFileExW        g_origCopyFileExW        = nullptr;
static FnCopyFileExA        g_origCopyFileExA        = nullptr;
static FnLoadLibraryW       g_origLoadLibraryW       = nullptr;
static FnLoadLibraryA       g_origLoadLibraryA       = nullptr;
static FnLoadLibraryExW     g_origLoadLibraryExW     = nullptr;
static FnLoadLibraryExA     g_origLoadLibraryExA     = nullptr;

// Reentrancy guard for file-management hooks. Two chains reach an already-hooked
// export: KernelBase's own CopyFileW calls the exported CopyFileExW, and the
// kernel32-only ANSI variants (CopyFileA, CopyFileExA) funnel into KernelBase's
// CopyFileExW as well. Move does not re-enter — MoveFileW/A and MoveFileExA all
// go through MoveFileWithProgress[Transacted]W, which we do not hook — but the
// guard covers the family uniformly.
static thread_local bool g_inFileOpHook = false;

// Reentrancy guard for the CreateFile family. CreateFile2 calls an internal
// KernelBase helper rather than the exported CreateFileW on every build checked,
// so this is insurance: should a future build funnel 2 -> W, the inner call
// degrades to a plain passthrough instead of logging and redirecting twice.
static thread_local bool g_inCreateFileHook = false;

// Reentrancy guard for the FindFirstFile family. Modern KernelBase routes all
// four through an internal helper, but XP and Vista have no KernelBase, so the
// hooks sit on kernel32's own implementations, and there FindFirstFileW/A call
// the exported FindFirstFileExW. The A variants must keep their own trampolines
// (the out-buffer is WIN32_FIND_DATAA), so the nesting cannot be avoided by
// routing, only by recognising it. Nothing caller-supplied runs inside a find,
// so a guard cannot hide a legitimate access.
static thread_local bool g_inFindHook = false;

// Sets a thread_local flag for the enclosing scope.
class ScopedFlag
{
public:
    explicit ScopedFlag(bool& flag) : m_flag(flag) { m_flag = true; }
    ~ScopedFlag() { m_flag = false; }
    ScopedFlag(const ScopedFlag&) = delete;
    ScopedFlag& operator=(const ScopedFlag&) = delete;
private:
    bool& m_flag;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::wstring AnsiToWide(LPCSTR str)
{
    if (!str || !str[0]) return {};
    int wlen = MultiByteToWideChar(CP_ACP, 0, str, -1, nullptr, 0);
    if (wlen <= 1) return {};
    std::wstring result(wlen - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, str, -1, result.data(), wlen);
    return result;
}

static FileAccessMode AccessMode(DWORD access)
{
    bool r = (access & (GENERIC_READ  | FILE_READ_DATA))                    != 0;
    bool w = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    if (w && r) return FileAccessMode::ReadWrite;
    if (w)      return FileAccessMode::Write;
    return             FileAccessMode::Read;
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
    if (InternalFileAccess::Active() || g_inCreateFileHook)
        return g_origCreateFileW(path.c_str(), dwDesiredAccess, dwShareMode,
            lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    ScopedFlag guard(g_inCreateFileHook);

    std::wstring target = ResolveOpenPath(path, AccessMode(dwDesiredAccess));

    return g_origCreateFileW(
        target.c_str(), dwDesiredAccess, dwShareMode,
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

    // CreateFileWImpl always calls g_origCreateFileW (the real KernelBase function),
    // which accepts wide paths, so this correctly handles ANSI → wide redirections.
    return CreateFileWImpl(wpath, dwDesiredAccess, dwShareMode,
        lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

static HANDLE WINAPI HookCreateFile2(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    DWORD dwCreationDisposition, LPCREATEFILE2_EXTENDED_PARAMETERS pCreateExParams)
{
    if (!lpFileName || InternalFileAccess::Active() || g_inCreateFileHook)
        return g_origCreateFile2(lpFileName, dwDesiredAccess, dwShareMode,
            dwCreationDisposition, pCreateExParams);

    ScopedFlag guard(g_inCreateFileHook);

    std::wstring target = ResolveOpenPath(lpFileName, AccessMode(dwDesiredAccess));

    return g_origCreateFile2(target.c_str(), dwDesiredAccess, dwShareMode,
        dwCreationDisposition, pCreateExParams);
}

// ---------------------------------------------------------------------------
// Hook implementations — GetFileAttributes
// ---------------------------------------------------------------------------
static DWORD WINAPI HookGetFileAttributesW(LPCWSTR lpFileName)
{
    if (!lpFileName)
        return g_origGetFileAttributesW(nullptr);

    // Suppress all hook logic during internal FastDL operations
    if (InternalFileAccess::Active())
        return g_origGetFileAttributesW(lpFileName);

    std::wstring path(lpFileName);
    std::wstring redirected = RedirectFilePath(path);

    if (redirected != path)
    {
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());
        return g_origGetFileAttributesW(redirected.c_str());
    }

    LogFileAccess(L"FILE ATTR", path.c_str());

    DWORD attrs = g_origGetFileAttributesW(lpFileName);

    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        // Check overlay cache first (cheaper than a network request)
        std::wstring overlayPath = AttributeFallbackPath(path);

        if (!overlayPath.empty())
            return g_origGetFileAttributesW(overlayPath.c_str());

        // File not in overlay — ask the FastDL server if it can supply it.
        // Report FILE_ATTRIBUTE_NORMAL so the game proceeds to open it;
        // the actual download happens in CreateFileW.
        bool exists = false;
        {
            InternalFileAccess internal;
            exists = FastDLFileExists(path);
        }

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
    if (InternalFileAccess::Active())
        return g_origGetFileAttributesA(lpFileName);

    int wlen = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, nullptr, 0);
    if (wlen <= 0)
        return g_origGetFileAttributesA(lpFileName);

    std::wstring wpath(wlen - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wpath.data(), wlen);

    std::wstring redirected = RedirectFilePath(wpath);

    if (redirected != wpath)
    {
        LogFileAccess(L"FILE REDIRECT", wpath.c_str(), redirected.c_str());
        return g_origGetFileAttributesW(redirected.c_str());
    }

    LogFileAccess(L"FILE ATTR", wpath.c_str());

    DWORD attrs = g_origGetFileAttributesW(wpath.c_str());

    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        // Check overlay cache first
        std::wstring overlayPath = AttributeFallbackPath(wpath);

        if (!overlayPath.empty())
            return g_origGetFileAttributesW(overlayPath.c_str());

        // FastDL server existence check
        bool exists = false;
        {
            InternalFileAccess internal;
            exists = FastDLFileExists(wpath);
        }

        if (exists)
            return FILE_ATTRIBUTE_NORMAL;
    }

    return attrs;
}

// ---------------------------------------------------------------------------
// Hook implementations — GetFileAttributesEx
//
// This is what the CRT's _wstat/_waccess and std::filesystem::exists/file_size
// actually call. Same [FILE ATTR] verb as the plain form — it answers the same
// question, and reusing the verb keeps the event vocabulary consumers depend on
// unchanged.
// ---------------------------------------------------------------------------
static BOOL WINAPI HookGetFileAttributesExW(
    LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation)
{
    if (!lpFileName || InternalFileAccess::Active())
        return g_origGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);

    std::wstring path(lpFileName);
    std::wstring redirected = RedirectFilePath(path);

    if (redirected != path)
    {
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());
        return g_origGetFileAttributesExW(redirected.c_str(), fInfoLevelId, lpFileInformation);
    }

    LogFileAccess(L"FILE ATTR", path.c_str());

    BOOL ok = g_origGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);

    if (!ok)
    {
        std::wstring overlayPath = AttributeFallbackPath(path);

        if (!overlayPath.empty())
            return g_origGetFileAttributesExW(overlayPath.c_str(), fInfoLevelId, lpFileInformation);
    }

    return ok;
}

static BOOL WINAPI HookGetFileAttributesExA(
    LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation)
{
    if (!lpFileName || InternalFileAccess::Active())
        return g_origGetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);

    std::wstring wpath = AnsiToWide(lpFileName);
    if (wpath.empty())
        return g_origGetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);

    return HookGetFileAttributesExW(wpath.c_str(), fInfoLevelId, lpFileInformation);
}

// ---------------------------------------------------------------------------
// Hook implementations — FindFirstFile
// ---------------------------------------------------------------------------
static HANDLE WINAPI HookFindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData)
{
    if (!lpFileName || g_inFindHook)
        return g_origFindFirstFileW(lpFileName, lpFindFileData);

    ScopedFlag guard(g_inFindHook);

    std::wstring path(lpFileName);
    std::wstring redirected = RedirectFilePath(path);

    if (redirected != path)
    {
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());
        return g_origFindFirstFileW(redirected.c_str(), lpFindFileData);
    }

    LogFileAccess(L"FILE FIND", path.c_str());
    return g_origFindFirstFileW(lpFileName, lpFindFileData);
}

static HANDLE WINAPI HookFindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    if (!lpFileName || g_inFindHook)
        return g_origFindFirstFileA(lpFileName, lpFindFileData);

    ScopedFlag guard(g_inFindHook);

    int wideLength = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, nullptr, 0);
    if (wideLength <= 0)
        return g_origFindFirstFileA(lpFileName, lpFindFileData);

    std::wstring widePath(wideLength - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, widePath.data(), wideLength);

    std::wstring redirected = RedirectFilePath(widePath);

    if (redirected != widePath)
    {
        LogFileAccess(L"FILE REDIRECT", widePath.c_str(), redirected.c_str());
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

    LogFileAccess(L"FILE FIND", widePath.c_str());
    return g_origFindFirstFileA(lpFileName, lpFindFileData);
}

// ---------------------------------------------------------------------------
// Hook implementations — FindFirstFileEx
// ---------------------------------------------------------------------------
static HANDLE WINAPI HookFindFirstFileExW(
    LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId,
    LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp,
    LPVOID lpSearchFilter, DWORD dwAdditionalFlags)
{
    if (!lpFileName || InternalFileAccess::Active() || g_inFindHook)
        return g_origFindFirstFileExW(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);

    ScopedFlag guard(g_inFindHook);

    std::wstring path(lpFileName);
    std::wstring redirected = RedirectFilePath(path);

    if (redirected != path)
    {
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());
        return g_origFindFirstFileExW(redirected.c_str(), fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
    }

    LogFileAccess(L"FILE FIND", path.c_str());
    return g_origFindFirstFileExW(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

static HANDLE WINAPI HookFindFirstFileExA(
    LPCSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId,
    LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp,
    LPVOID lpSearchFilter, DWORD dwAdditionalFlags)
{
    if (!lpFileName || InternalFileAccess::Active() || g_inFindHook)
        return g_origFindFirstFileExA(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);

    ScopedFlag guard(g_inFindHook);

    std::wstring wpath = AnsiToWide(lpFileName);
    if (wpath.empty())
        return g_origFindFirstFileExA(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);

    std::wstring redirected = RedirectFilePath(wpath);

    if (redirected != wpath)
    {
        LogFileAccess(L"FILE REDIRECT", wpath.c_str(), redirected.c_str());
        // Use the W trampoline since we have a wide redirected path; cast the find
        // data pointer — the caller passed an A-sized buffer but FindFirstFileExW
        // writes a W-sized structure.  However the caller expects ANSI data, so
        // we cannot safely call the W version here.  Convert redirected path back
        // to ANSI instead.
        int ansiLen = WideCharToMultiByte(CP_ACP, 0, redirected.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (ansiLen > 1)
        {
            std::string ansiPath(static_cast<size_t>(ansiLen - 1), '\0');
            WideCharToMultiByte(CP_ACP, 0, redirected.c_str(), -1, ansiPath.data(), ansiLen, nullptr, nullptr);
            return g_origFindFirstFileExA(ansiPath.c_str(), fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
        }
        return g_origFindFirstFileExA(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
    }

    LogFileAccess(L"FILE FIND", wpath.c_str());
    return g_origFindFirstFileExA(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

// ---------------------------------------------------------------------------
// Hook implementations — DeleteFile
// ---------------------------------------------------------------------------
static BOOL WINAPI HookDeleteFileW(LPCWSTR lpFileName)
{
    if (!lpFileName || InternalFileAccess::Active())
        return g_origDeleteFileW(lpFileName);

    std::wstring path(lpFileName);
    std::wstring redirected = RedirectFilePath(path);

    if (redirected != path)
    {
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());
        return g_origDeleteFileW(redirected.c_str());
    }

    LogFileAccess(L"FILE DELETE", path.c_str());
    return g_origDeleteFileW(lpFileName);
}

static BOOL WINAPI HookDeleteFileA(LPCSTR lpFileName)
{
    if (!lpFileName || InternalFileAccess::Active())
        return g_origDeleteFileA(lpFileName);

    std::wstring wpath = AnsiToWide(lpFileName);
    if (wpath.empty())
        return g_origDeleteFileA(lpFileName);

    std::wstring redirected = RedirectFilePath(wpath);

    if (redirected != wpath)
    {
        LogFileAccess(L"FILE REDIRECT", wpath.c_str(), redirected.c_str());
        return g_origDeleteFileW(redirected.c_str());
    }

    LogFileAccess(L"FILE DELETE", wpath.c_str());

    return g_origDeleteFileW(wpath.c_str());
}

// ---------------------------------------------------------------------------
// Hook implementations — MoveFile / MoveFileEx
// ---------------------------------------------------------------------------
static BOOL WINAPI HookMoveFileW(LPCWSTR lpExisting, LPCWSTR lpNew)
{
    if (g_inFileOpHook || InternalFileAccess::Active())
        return g_origMoveFileW(lpExisting, lpNew);

    g_inFileOpHook = true;

    std::wstring src(lpExisting ? lpExisting : L"");
    std::wstring dst(lpNew ? lpNew : L"");
    std::wstring rSrc = RedirectFilePath(src);
    std::wstring rDst = RedirectFilePath(dst);

    if (rSrc != src)
        LogFileAccess(L"FILE REDIRECT", src.c_str(), rSrc.c_str());
    if (rDst != dst)
    {
        LogFileAccess(L"FILE REDIRECT", dst.c_str(), rDst.c_str());
        EnsureParentDirectoryExists(rDst);
    }

    if (rSrc == src && rDst == dst)
        LogFileAccess(L"FILE MOVE", src.c_str(), dst.c_str());

    BOOL result = g_origMoveFileW(rSrc.c_str(), rDst.c_str());
    g_inFileOpHook = false;
    return result;
}

static BOOL WINAPI HookMoveFileA(LPCSTR lpExisting, LPCSTR lpNew)
{
    if (g_inFileOpHook || InternalFileAccess::Active())
        return g_origMoveFileA(lpExisting, lpNew);

    g_inFileOpHook = true;

    std::wstring src = AnsiToWide(lpExisting);
    std::wstring dst = AnsiToWide(lpNew);

    if (src.empty() && dst.empty())
    {
        g_inFileOpHook = false;
        return g_origMoveFileA(lpExisting, lpNew);
    }

    std::wstring rSrc = RedirectFilePath(src);
    std::wstring rDst = RedirectFilePath(dst);

    if (rSrc != src)
        LogFileAccess(L"FILE REDIRECT", src.c_str(), rSrc.c_str());
    if (rDst != dst)
    {
        LogFileAccess(L"FILE REDIRECT", dst.c_str(), rDst.c_str());
        EnsureParentDirectoryExists(rDst);
    }

    if (rSrc == src && rDst == dst)
    {
        LogFileAccess(L"FILE MOVE", src.c_str(), dst.c_str());
        g_inFileOpHook = false;
        return g_origMoveFileA(lpExisting, lpNew);
    }

    BOOL result = g_origMoveFileW(rSrc.c_str(), rDst.c_str());
    g_inFileOpHook = false;
    return result;
}

static BOOL WINAPI HookMoveFileExW(LPCWSTR lpExisting, LPCWSTR lpNew, DWORD dwFlags)
{
    if (g_inFileOpHook || InternalFileAccess::Active())
        return g_origMoveFileExW(lpExisting, lpNew, dwFlags);

    g_inFileOpHook = true;

    std::wstring src(lpExisting ? lpExisting : L"");
    std::wstring dst(lpNew ? lpNew : L"");
    std::wstring rSrc = RedirectFilePath(src);
    std::wstring rDst = RedirectFilePath(dst);

    if (rSrc != src)
        LogFileAccess(L"FILE REDIRECT", src.c_str(), rSrc.c_str());
    if (rDst != dst)
    {
        LogFileAccess(L"FILE REDIRECT", dst.c_str(), rDst.c_str());
        EnsureParentDirectoryExists(rDst);
    }

    if (rSrc == src && rDst == dst)
        LogFileAccess(L"FILE MOVE", src.c_str(), dst.c_str());

    BOOL result = g_origMoveFileExW(rSrc.c_str(), rDst.c_str(), dwFlags);
    g_inFileOpHook = false;
    return result;
}

static BOOL WINAPI HookMoveFileExA(LPCSTR lpExisting, LPCSTR lpNew, DWORD dwFlags)
{
    if (g_inFileOpHook || InternalFileAccess::Active())
        return g_origMoveFileExA(lpExisting, lpNew, dwFlags);

    g_inFileOpHook = true;

    std::wstring src = AnsiToWide(lpExisting);
    std::wstring dst = AnsiToWide(lpNew);

    if (src.empty() && dst.empty())
    {
        g_inFileOpHook = false;
        return g_origMoveFileExA(lpExisting, lpNew, dwFlags);
    }

    std::wstring rSrc = RedirectFilePath(src);
    std::wstring rDst = RedirectFilePath(dst);

    if (rSrc != src)
        LogFileAccess(L"FILE REDIRECT", src.c_str(), rSrc.c_str());
    if (rDst != dst)
    {
        LogFileAccess(L"FILE REDIRECT", dst.c_str(), rDst.c_str());
        EnsureParentDirectoryExists(rDst);
    }

    if (rSrc == src && rDst == dst)
    {
        LogFileAccess(L"FILE MOVE", src.c_str(), dst.c_str());
        g_inFileOpHook = false;
        return g_origMoveFileExA(lpExisting, lpNew, dwFlags);
    }

    BOOL result = g_origMoveFileExW(rSrc.c_str(), rDst.c_str(), dwFlags);
    g_inFileOpHook = false;
    return result;
}

// ---------------------------------------------------------------------------
// Hook implementations — CopyFile / CopyFileEx
// ---------------------------------------------------------------------------
static BOOL WINAPI HookCopyFileW(LPCWSTR lpExisting, LPCWSTR lpNew, BOOL bFailIfExists)
{
    if (g_inFileOpHook || InternalFileAccess::Active())
        return g_origCopyFileW(lpExisting, lpNew, bFailIfExists);

    g_inFileOpHook = true;

    std::wstring src(lpExisting ? lpExisting : L"");
    std::wstring dst(lpNew ? lpNew : L"");
    std::wstring rSrc = RedirectFilePath(src);
    std::wstring rDst = RedirectFilePath(dst);

    if (rSrc != src)
        LogFileAccess(L"FILE REDIRECT", src.c_str(), rSrc.c_str());
    if (rDst != dst)
    {
        LogFileAccess(L"FILE REDIRECT", dst.c_str(), rDst.c_str());
        EnsureParentDirectoryExists(rDst);
    }

    if (rSrc == src && rDst == dst)
        LogFileAccess(L"FILE COPY", src.c_str(), dst.c_str());

    BOOL result = g_origCopyFileW(rSrc.c_str(), rDst.c_str(), bFailIfExists);
    g_inFileOpHook = false;
    return result;
}

static BOOL WINAPI HookCopyFileA(LPCSTR lpExisting, LPCSTR lpNew, BOOL bFailIfExists)
{
    if (g_inFileOpHook || InternalFileAccess::Active())
        return g_origCopyFileA(lpExisting, lpNew, bFailIfExists);

    g_inFileOpHook = true;

    std::wstring src = AnsiToWide(lpExisting);
    std::wstring dst = AnsiToWide(lpNew);

    if (src.empty() && dst.empty())
    {
        g_inFileOpHook = false;
        return g_origCopyFileA(lpExisting, lpNew, bFailIfExists);
    }

    std::wstring rSrc = RedirectFilePath(src);
    std::wstring rDst = RedirectFilePath(dst);

    if (rSrc != src)
        LogFileAccess(L"FILE REDIRECT", src.c_str(), rSrc.c_str());
    if (rDst != dst)
    {
        LogFileAccess(L"FILE REDIRECT", dst.c_str(), rDst.c_str());
        EnsureParentDirectoryExists(rDst);
    }

    if (rSrc == src && rDst == dst)
    {
        LogFileAccess(L"FILE COPY", src.c_str(), dst.c_str());
        g_inFileOpHook = false;
        return g_origCopyFileA(lpExisting, lpNew, bFailIfExists);
    }

    BOOL result = g_origCopyFileW(rSrc.c_str(), rDst.c_str(), bFailIfExists);
    g_inFileOpHook = false;
    return result;
}

static BOOL WINAPI HookCopyFileExW(
    LPCWSTR lpExisting, LPCWSTR lpNew,
    LPPROGRESS_ROUTINE lpProgressRoutine, LPVOID lpData, LPBOOL pbCancel, DWORD dwCopyFlags)
{
    if (g_inFileOpHook || InternalFileAccess::Active())
        return g_origCopyFileExW(lpExisting, lpNew, lpProgressRoutine, lpData, pbCancel, dwCopyFlags);

    g_inFileOpHook = true;

    std::wstring src(lpExisting ? lpExisting : L"");
    std::wstring dst(lpNew ? lpNew : L"");
    std::wstring rSrc = RedirectFilePath(src);
    std::wstring rDst = RedirectFilePath(dst);

    if (rSrc != src)
        LogFileAccess(L"FILE REDIRECT", src.c_str(), rSrc.c_str());
    if (rDst != dst)
    {
        LogFileAccess(L"FILE REDIRECT", dst.c_str(), rDst.c_str());
        EnsureParentDirectoryExists(rDst);
    }

    if (rSrc == src && rDst == dst)
        LogFileAccess(L"FILE COPY", src.c_str(), dst.c_str());

    BOOL result = g_origCopyFileExW(rSrc.c_str(), rDst.c_str(), lpProgressRoutine, lpData, pbCancel, dwCopyFlags);
    g_inFileOpHook = false;
    return result;
}

static BOOL WINAPI HookCopyFileExA(
    LPCSTR lpExisting, LPCSTR lpNew,
    LPPROGRESS_ROUTINE lpProgressRoutine, LPVOID lpData, LPBOOL pbCancel, DWORD dwCopyFlags)
{
    if (g_inFileOpHook || InternalFileAccess::Active())
        return g_origCopyFileExA(lpExisting, lpNew, lpProgressRoutine, lpData, pbCancel, dwCopyFlags);

    g_inFileOpHook = true;

    std::wstring src = AnsiToWide(lpExisting);
    std::wstring dst = AnsiToWide(lpNew);

    if (src.empty() && dst.empty())
    {
        g_inFileOpHook = false;
        return g_origCopyFileExA(lpExisting, lpNew, lpProgressRoutine, lpData, pbCancel, dwCopyFlags);
    }

    std::wstring rSrc = RedirectFilePath(src);
    std::wstring rDst = RedirectFilePath(dst);

    if (rSrc != src)
        LogFileAccess(L"FILE REDIRECT", src.c_str(), rSrc.c_str());
    if (rDst != dst)
    {
        LogFileAccess(L"FILE REDIRECT", dst.c_str(), rDst.c_str());
        EnsureParentDirectoryExists(rDst);
    }

    if (rSrc == src && rDst == dst)
    {
        LogFileAccess(L"FILE COPY", src.c_str(), dst.c_str());
        g_inFileOpHook = false;
        return g_origCopyFileExA(lpExisting, lpNew, lpProgressRoutine, lpData, pbCancel, dwCopyFlags);
    }

    BOOL result = g_origCopyFileExW(rSrc.c_str(), rDst.c_str(), lpProgressRoutine, lpData, pbCancel, dwCopyFlags);
    g_inFileOpHook = false;
    return result;
}

// ---------------------------------------------------------------------------
// Hook implementations — LoadLibrary
// ---------------------------------------------------------------------------
// Every hook in the family finishes on the LoadLibraryExW trampoline, never on
// its own. LoadLibraryW(p) is LoadLibraryExW(p, NULL, 0) by definition, and on
// XP and Vista, which have no KernelBase, these hooks sit on kernel32's real
// implementations, which call each other through the exported (hooked) entry
// points: LoadLibraryW -> LoadLibraryExW, LoadLibraryA -> LoadLibraryExA ->
// LoadLibraryExW. Calling a hook's own trampoline there would log, redirect and
// fire OnLibraryLoaded once per layer. A reentrancy guard would not do instead:
// a DllMain may load further libraries on the same thread, and those must still
// be redirected.
static HMODULE LoadLibraryImpl(const std::wstring& path, HANDLE hFile, DWORD dwFlags)
{
    std::wstring redirected = RedirectFilePath(path);

    if (redirected != path)
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());
    else
        LogFileAccess(L"DLL LOAD", path.c_str());

    HMODULE hMod = g_origLoadLibraryExW(redirected.c_str(), hFile, dwFlags);
    constexpr DWORD dataFlags = LOAD_LIBRARY_AS_DATAFILE
                              | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE
                              | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if (!(dwFlags & dataFlags))
        OnLibraryLoaded(hMod);
    return hMod;
}

// Converts with the code page kernel32 itself would use for this ANSI path,
// which SetFileApisToOEM can switch away from CP_ACP. Empty on failure.
static std::wstring LibraryNameToWide(LPCSTR name)
{
    UINT codePage = AreFileApisANSI() ? CP_ACP : CP_OEMCP;
    int wideLength = MultiByteToWideChar(codePage, 0, name, -1, nullptr, 0);
    if (wideLength <= 0)
        return {};

    std::wstring wide(wideLength - 1, L'\0');
    MultiByteToWideChar(codePage, 0, name, -1, wide.data(), wideLength);
    return wide;
}

static HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpLibFileName)
{
    if (!lpLibFileName)
        return g_origLoadLibraryW(nullptr);

    return LoadLibraryImpl(lpLibFileName, nullptr, 0);
}

static HMODULE WINAPI HookLoadLibraryA(LPCSTR lpLibFileName)
{
    if (!lpLibFileName)
        return g_origLoadLibraryA(nullptr);

    std::wstring widePath = LibraryNameToWide(lpLibFileName);
    if (widePath.empty())
        return g_origLoadLibraryA(lpLibFileName);

    return LoadLibraryImpl(widePath, nullptr, 0);
}

static HMODULE WINAPI HookLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    if (!lpLibFileName)
        return g_origLoadLibraryExW(nullptr, hFile, dwFlags);

    return LoadLibraryImpl(lpLibFileName, hFile, dwFlags);
}

static HMODULE WINAPI HookLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    if (!lpLibFileName)
        return g_origLoadLibraryExA(nullptr, hFile, dwFlags);

    std::wstring widePath = LibraryNameToWide(lpLibFileName);
    if (widePath.empty())
        return g_origLoadLibraryExA(lpLibFileName, hFile, dwFlags);

    return LoadLibraryImpl(widePath, hFile, dwFlags);
}

// ---------------------------------------------------------------------------
// Hook installation
// ---------------------------------------------------------------------------

// Hooks a file API in KernelBase, falling back to kernel32.
template <typename Fn>
static void HookFileApi(const char* name, Fn detour, Fn* orig)
{
    MH_STATUS status = MH_CreateHookApi(L"kernelbase", name,
        reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(orig));

    if (status == MH_OK || status == MH_ERROR_ALREADY_CREATED)
    {
        LogHookInit(L"kernelbase", name, status);
        return;
    }

    if (status != MH_ERROR_MODULE_NOT_FOUND && status != MH_ERROR_FUNCTION_NOT_FOUND)
        LogHookInit(L"kernelbase", name, status);

    LogHookInit(L"kernel32", name,
        MH_CreateHookApi(L"kernel32", name,
            reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(orig)));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InstallFileHooks()
{
    HookFileApi("CreateFileW",          HookCreateFileW,          &g_origCreateFileW);
    HookFileApi("CreateFileA",          HookCreateFileA,          &g_origCreateFileA);
    HookFileApi("CreateFile2",          HookCreateFile2,          &g_origCreateFile2);
    HookFileApi("GetFileAttributesW",   HookGetFileAttributesW,   &g_origGetFileAttributesW);
    HookFileApi("GetFileAttributesA",   HookGetFileAttributesA,   &g_origGetFileAttributesA);
    HookFileApi("GetFileAttributesExW", HookGetFileAttributesExW, &g_origGetFileAttributesExW);
    HookFileApi("GetFileAttributesExA", HookGetFileAttributesExA, &g_origGetFileAttributesExA);
    HookFileApi("FindFirstFileW",       HookFindFirstFileW,       &g_origFindFirstFileW);
    HookFileApi("FindFirstFileA",       HookFindFirstFileA,       &g_origFindFirstFileA);
    HookFileApi("FindFirstFileExW",     HookFindFirstFileExW,     &g_origFindFirstFileExW);
    HookFileApi("FindFirstFileExA",     HookFindFirstFileExA,     &g_origFindFirstFileExA);
    HookFileApi("DeleteFileW",          HookDeleteFileW,          &g_origDeleteFileW);
    HookFileApi("DeleteFileA",          HookDeleteFileA,          &g_origDeleteFileA);
    HookFileApi("MoveFileW",            HookMoveFileW,            &g_origMoveFileW);
    HookFileApi("MoveFileA",            HookMoveFileA,            &g_origMoveFileA);
    HookFileApi("MoveFileExW",          HookMoveFileExW,          &g_origMoveFileExW);
    HookFileApi("MoveFileExA",          HookMoveFileExA,          &g_origMoveFileExA);
    HookFileApi("CopyFileW",            HookCopyFileW,            &g_origCopyFileW);
    HookFileApi("CopyFileA",            HookCopyFileA,            &g_origCopyFileA);
    HookFileApi("CopyFileExW",          HookCopyFileExW,          &g_origCopyFileExW);
    HookFileApi("CopyFileExA",          HookCopyFileExA,          &g_origCopyFileExA);

    LogHookInit(L"kernel32", "LoadLibraryW",
        MH_CreateHookApi(L"kernel32", "LoadLibraryW",
            reinterpret_cast<LPVOID>(HookLoadLibraryW),
            reinterpret_cast<LPVOID*>(&g_origLoadLibraryW)));

    LogHookInit(L"kernel32", "LoadLibraryA",
        MH_CreateHookApi(L"kernel32", "LoadLibraryA",
            reinterpret_cast<LPVOID>(HookLoadLibraryA),
            reinterpret_cast<LPVOID*>(&g_origLoadLibraryA)));

    LogHookInit(L"kernel32", "LoadLibraryExW",
        MH_CreateHookApi(L"kernel32", "LoadLibraryExW",
            reinterpret_cast<LPVOID>(HookLoadLibraryExW),
            reinterpret_cast<LPVOID*>(&g_origLoadLibraryExW)));

    LogHookInit(L"kernel32", "LoadLibraryExA",
        MH_CreateHookApi(L"kernel32", "LoadLibraryExA",
            reinterpret_cast<LPVOID>(HookLoadLibraryExA),
            reinterpret_cast<LPVOID*>(&g_origLoadLibraryExA)));
}
