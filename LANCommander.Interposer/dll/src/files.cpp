#include "files.h"
#include "callbacks.h"
#include "config.h"
#include "fastdl.h"
#include "hooks.h"

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

// Recursively create all directories along a file path's parent chain.
static void EnsureParentDirectoryExists(const std::wstring& filePath)
{
    size_t pos = filePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos == 0)
        return;

    std::wstring dir = filePath.substr(0, pos);

    // Already exists? Use trampoline to bypass our own hook.
    auto getAttrs = g_origGetFileAttributesW ? g_origGetFileAttributesW : GetFileAttributesW;
    DWORD attrs = getAttrs(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return;

    // Walk forward creating each segment
    for (size_t i = 0; i < dir.size(); ++i)
    {
        if (dir[i] == L'\\' || dir[i] == L'/')
        {
            // Skip drive letter root (e.g. "C:\") and UNC prefixes
            if (i == 0) continue;
            if (i == 2 && dir[1] == L':') continue;

            std::wstring segment = dir.substr(0, i);
            CreateDirectoryW(segment.c_str(), nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

static std::wstring AnsiToWide(LPCSTR str)
{
    if (!str || !str[0]) return {};
    int wlen = MultiByteToWideChar(CP_ACP, 0, str, -1, nullptr, 0);
    if (wlen <= 1) return {};
    std::wstring result(wlen - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, str, -1, result.data(), wlen);
    return result;
}

static const wchar_t* AccessVerb(DWORD access)
{
    bool r = (access & (GENERIC_READ  | FILE_READ_DATA))                    != 0;
    bool w = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    if (w && r) return L"FILE R/W";
    if (w)      return L"FILE WRITE";
    return             L"FILE READ";
}

// Applies file redirects and, at Debug level or above, records whether a rule
// actually fired. Drop-in replacement for ApplyFileRedirects at every hook call
// site — the existing [FILE REDIRECT] / plain-verb lines are unaffected.
static std::wstring RedirectWithDiag(const std::wstring& path)
{
    // Below Debug there is nothing to report, so skip the bookkeeping entirely.
    if (!g_logFiles || g_logLevel < LogLevel::Debug)
        return ApplyFileRedirects(path);

    FileRedirectMatch match;
    std::wstring result = ApplyFileRedirects(path, &match);

    if (match.matched)
    {
        std::wstring detail = L"rule #" + std::to_wstring(match.ruleIndex + 1) + L"  " + match.pattern;

        // Only worth showing when %TOKEN% substitution rewrote the rule: the
        // author needs to see both what they wrote and what the regex compiled.
        if (!match.source.empty() && match.source != match.pattern)
            detail += L"  (as written: " + match.source + L")";

        LogFileDiag(L"REDIRECT HIT", path.c_str(), detail.c_str());
    }
    else
    {
        std::wstring detail = match.ruleCount == 0
            ? std::wstring(L"no rules configured")
            : std::to_wstring(match.ruleCount)
                + (match.ruleCount == 1 ? L" rule, none matched" : L" rules, none matched");

        LogFileDiag(L"REDIRECT MISS", path.c_str(), detail.c_str());

        // Trace only — `tried` is left empty at Debug.
        for (const auto& pattern : match.tried)
            LogFileDiag(L"REDIRECT RULE", path.c_str(), pattern.c_str());
    }

    return result;
}

static std::wstring ResolveOpenPath(const std::wstring& path, DWORD dwDesiredAccess)
{
    std::wstring redirected = RedirectWithDiag(path);

    if (redirected != path)
    {
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());

        // Ensure the destination directory tree exists for write operations
        if (dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA))
            EnsureParentDirectoryExists(redirected);
    }
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
            LogFileAccess(L"FILE OVERLAY", redirected.c_str(), overlayPath.c_str());
            return overlayPath;
        }
    }

    return redirected;
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
    if (g_inFastDLHook || g_inCreateFileHook)
        return g_origCreateFileW(path.c_str(), dwDesiredAccess, dwShareMode,
            lpSA, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    ScopedFlag guard(g_inCreateFileHook);

    std::wstring target = ResolveOpenPath(path, dwDesiredAccess);

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
    if (!lpFileName || g_inFastDLHook || g_inCreateFileHook)
        return g_origCreateFile2(lpFileName, dwDesiredAccess, dwShareMode,
            dwCreationDisposition, pCreateExParams);

    ScopedFlag guard(g_inCreateFileHook);

    std::wstring target = ResolveOpenPath(lpFileName, dwDesiredAccess);

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
    if (g_inFastDLHook)
        return g_origGetFileAttributesW(lpFileName);

    std::wstring path(lpFileName);
    std::wstring redirected = RedirectWithDiag(path);

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

    std::wstring redirected = RedirectWithDiag(wpath);

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
    if (!lpFileName || g_inFastDLHook)
        return g_origGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);

    std::wstring path(lpFileName);
    std::wstring redirected = RedirectWithDiag(path);

    if (redirected != path)
    {
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());
        return g_origGetFileAttributesExW(redirected.c_str(), fInfoLevelId, lpFileInformation);
    }

    LogFileAccess(L"FILE ATTR", path.c_str());

    BOOL ok = g_origGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);

    if (!ok)
    {
        g_inFastDLHook = true;
        std::wstring overlayPath = GetExistingOverlayPath(path);
        g_inFastDLHook = false;

        if (!overlayPath.empty())
            return g_origGetFileAttributesExW(overlayPath.c_str(), fInfoLevelId, lpFileInformation);
    }

    return ok;
}

static BOOL WINAPI HookGetFileAttributesExA(
    LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation)
{
    if (!lpFileName || g_inFastDLHook)
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
    if (!lpFileName)
        return g_origFindFirstFileW(lpFileName, lpFindFileData);

    std::wstring path(lpFileName);
    std::wstring redirected = RedirectWithDiag(path);

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
    if (!lpFileName)
        return g_origFindFirstFileA(lpFileName, lpFindFileData);

    int wideLength = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, nullptr, 0);
    if (wideLength <= 0)
        return g_origFindFirstFileA(lpFileName, lpFindFileData);

    std::wstring widePath(wideLength - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, widePath.data(), wideLength);

    std::wstring redirected = RedirectWithDiag(widePath);

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
    if (!lpFileName || g_inFastDLHook)
        return g_origFindFirstFileExW(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);

    std::wstring path(lpFileName);
    std::wstring redirected = RedirectWithDiag(path);

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
    if (!lpFileName || g_inFastDLHook)
        return g_origFindFirstFileExA(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);

    std::wstring wpath = AnsiToWide(lpFileName);
    if (wpath.empty())
        return g_origFindFirstFileExA(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);

    std::wstring redirected = RedirectWithDiag(wpath);

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
    if (!lpFileName || g_inFastDLHook)
        return g_origDeleteFileW(lpFileName);

    std::wstring path(lpFileName);
    std::wstring redirected = RedirectWithDiag(path);

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
    if (!lpFileName || g_inFastDLHook)
        return g_origDeleteFileA(lpFileName);

    std::wstring wpath = AnsiToWide(lpFileName);
    if (wpath.empty())
        return g_origDeleteFileA(lpFileName);

    std::wstring redirected = RedirectWithDiag(wpath);

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
    if (g_inFileOpHook)
        return g_origMoveFileW(lpExisting, lpNew);

    g_inFileOpHook = true;

    std::wstring src(lpExisting ? lpExisting : L"");
    std::wstring dst(lpNew ? lpNew : L"");
    std::wstring rSrc = RedirectWithDiag(src);
    std::wstring rDst = RedirectWithDiag(dst);

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
    if (g_inFileOpHook)
        return g_origMoveFileA(lpExisting, lpNew);

    g_inFileOpHook = true;

    std::wstring src = AnsiToWide(lpExisting);
    std::wstring dst = AnsiToWide(lpNew);

    if (src.empty() && dst.empty())
    {
        g_inFileOpHook = false;
        return g_origMoveFileA(lpExisting, lpNew);
    }

    std::wstring rSrc = RedirectWithDiag(src);
    std::wstring rDst = RedirectWithDiag(dst);

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
    if (g_inFileOpHook)
        return g_origMoveFileExW(lpExisting, lpNew, dwFlags);

    g_inFileOpHook = true;

    std::wstring src(lpExisting ? lpExisting : L"");
    std::wstring dst(lpNew ? lpNew : L"");
    std::wstring rSrc = RedirectWithDiag(src);
    std::wstring rDst = RedirectWithDiag(dst);

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
    if (g_inFileOpHook)
        return g_origMoveFileExA(lpExisting, lpNew, dwFlags);

    g_inFileOpHook = true;

    std::wstring src = AnsiToWide(lpExisting);
    std::wstring dst = AnsiToWide(lpNew);

    if (src.empty() && dst.empty())
    {
        g_inFileOpHook = false;
        return g_origMoveFileExA(lpExisting, lpNew, dwFlags);
    }

    std::wstring rSrc = RedirectWithDiag(src);
    std::wstring rDst = RedirectWithDiag(dst);

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
    if (g_inFileOpHook)
        return g_origCopyFileW(lpExisting, lpNew, bFailIfExists);

    g_inFileOpHook = true;

    std::wstring src(lpExisting ? lpExisting : L"");
    std::wstring dst(lpNew ? lpNew : L"");
    std::wstring rSrc = RedirectWithDiag(src);
    std::wstring rDst = RedirectWithDiag(dst);

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
    if (g_inFileOpHook)
        return g_origCopyFileA(lpExisting, lpNew, bFailIfExists);

    g_inFileOpHook = true;

    std::wstring src = AnsiToWide(lpExisting);
    std::wstring dst = AnsiToWide(lpNew);

    if (src.empty() && dst.empty())
    {
        g_inFileOpHook = false;
        return g_origCopyFileA(lpExisting, lpNew, bFailIfExists);
    }

    std::wstring rSrc = RedirectWithDiag(src);
    std::wstring rDst = RedirectWithDiag(dst);

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
    if (g_inFileOpHook)
        return g_origCopyFileExW(lpExisting, lpNew, lpProgressRoutine, lpData, pbCancel, dwCopyFlags);

    g_inFileOpHook = true;

    std::wstring src(lpExisting ? lpExisting : L"");
    std::wstring dst(lpNew ? lpNew : L"");
    std::wstring rSrc = RedirectWithDiag(src);
    std::wstring rDst = RedirectWithDiag(dst);

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
    if (g_inFileOpHook)
        return g_origCopyFileExA(lpExisting, lpNew, lpProgressRoutine, lpData, pbCancel, dwCopyFlags);

    g_inFileOpHook = true;

    std::wstring src = AnsiToWide(lpExisting);
    std::wstring dst = AnsiToWide(lpNew);

    if (src.empty() && dst.empty())
    {
        g_inFileOpHook = false;
        return g_origCopyFileExA(lpExisting, lpNew, lpProgressRoutine, lpData, pbCancel, dwCopyFlags);
    }

    std::wstring rSrc = RedirectWithDiag(src);
    std::wstring rDst = RedirectWithDiag(dst);

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
static HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpLibFileName)
{
    if (!lpLibFileName)
        return g_origLoadLibraryW(nullptr);

    std::wstring path(lpLibFileName);
    std::wstring redirected = RedirectWithDiag(path);

    if (redirected != path)
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());
    else
        LogFileAccess(L"DLL LOAD", path.c_str());

    HMODULE hMod = g_origLoadLibraryW(redirected.c_str());
    OnLibraryLoaded(hMod);
    return hMod;
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

    std::wstring redirected = RedirectWithDiag(widePath);

    HMODULE hMod;
    if (redirected != widePath)
    {
        LogFileAccess(L"FILE REDIRECT", widePath.c_str(), redirected.c_str());
        // Use the W trampoline since we already have a wide redirected path
        hMod = g_origLoadLibraryW(redirected.c_str());
    }
    else
    {
        LogFileAccess(L"DLL LOAD", widePath.c_str());
        hMod = g_origLoadLibraryA(lpLibFileName);
    }
    OnLibraryLoaded(hMod);
    return hMod;
}

static HMODULE WINAPI HookLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    if (!lpLibFileName)
        return g_origLoadLibraryExW(nullptr, hFile, dwFlags);

    std::wstring path(lpLibFileName);
    std::wstring redirected = RedirectWithDiag(path);

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

static HMODULE WINAPI HookLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    if (!lpLibFileName)
        return g_origLoadLibraryExA(nullptr, hFile, dwFlags);

    int wideLength = MultiByteToWideChar(CP_ACP, 0, lpLibFileName, -1, nullptr, 0);
    if (wideLength <= 0)
        return g_origLoadLibraryExA(lpLibFileName, hFile, dwFlags);

    std::wstring widePath(wideLength - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, lpLibFileName, -1, widePath.data(), wideLength);

    std::wstring redirected = RedirectWithDiag(widePath);

    HMODULE hMod;
    if (redirected != widePath)
    {
        LogFileAccess(L"FILE REDIRECT", widePath.c_str(), redirected.c_str());
        hMod = g_origLoadLibraryExW(redirected.c_str(), hFile, dwFlags);
    }
    else
    {
        LogFileAccess(L"DLL LOAD", widePath.c_str());
        hMod = g_origLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    }
    constexpr DWORD dataFlags = LOAD_LIBRARY_AS_DATAFILE
                              | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE
                              | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if (!(dwFlags & dataFlags))
        OnLibraryLoaded(hMod);
    return hMod;
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
