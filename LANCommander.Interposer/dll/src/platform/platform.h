#pragma once

// The operating-system services the portable core (dll\src\core) is written
// against. Everything under core\ reaches the OS through this header and
// win32_types.h and nothing else, so it compiles unchanged on every platform;
// each platform supplies the implementation (platform\win\platform.cpp).
//
// Deliberately small: only what the core needs. Hook subsystems talk to their
// own OS directly.

#include "win32_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace platform
{
    // ── Paths ─────────────────────────────────────────────────────────────────

    // The native directory separator: L'\\' on Windows, L'/' elsewhere.
    extern const wchar_t kPathSeparator;

    // True for a path that names its own root. On Windows that is a drive-
    // qualified path ("C:\..."), a UNC share or a rooted path ("\..."); either
    // separator counts.
    bool IsAbsolutePath(const std::wstring& path);

    // Drop trailing separators, keeping the one a root needs ("C:\", "/").
    void TrimTrailingSeparators(std::wstring& path);

    // Absolute form of `path`, relative to the current directory. False when
    // the path is not usable.
    bool GetFullPath(const std::wstring& path, std::wstring& out);

    // The directory this module (the Interposer itself) was loaded from, with a
    // trailing separator.
    std::wstring ModuleDirectory();

    // The directory of the process's executable, without a trailing separator.
    // Empty if it cannot be determined.
    std::wstring ExecutableDirectory();

    // Create every missing directory along `path`.
    void CreateDirectoryTree(const std::wstring& path);

    // One entry of a directory listing.
    struct DirectoryEntry
    {
        std::wstring name;
        bool         isDirectory;
    };

    // The entries of `directory` in the order the file system returns them,
    // without "." and "..". Empty when the directory does not exist.
    std::vector<DirectoryEntry> ListDirectory(const std::wstring& directory);

    // ── Text ──────────────────────────────────────────────────────────────────

    // UTF-8 <-> wchar_t (UTF-16 on Windows, UTF-32 elsewhere). Conversion stops
    // at an embedded NUL. Invalid input is replaced, never rejected.
    std::string  WideToUtf8(const std::wstring& text);
    std::wstring Utf8ToWide(const std::string& text);

    // UTF-16 LE bytes (no BOM) to UTF-8. Independent of sizeof(wchar_t).
    std::string Utf16LeToUtf8(const unsigned char* data, size_t bytes);

    // ── Files ─────────────────────────────────────────────────────────────────

    using FileHandle = intptr_t;
    inline constexpr FileHandle kInvalidFile = -1;

    // Open for appending, creating the file if needed; others may read it while
    // it is open. kInvalidFile on failure.
    FileHandle OpenForAppend(const std::wstring& path);
    bool       WriteToFile(FileHandle file, const void* data, size_t bytes);
    void       CloseFile(FileHandle file);

    // The whole file. False when it cannot be opened, or is empty or larger
    // than maxBytes.
    bool ReadWholeFile(const std::wstring& path, size_t maxBytes, std::vector<unsigned char>& out);

    // Replace the file's content with `content`.
    bool WriteWholeFile(const std::wstring& path, const std::string& content);

    // ── Environment ───────────────────────────────────────────────────────────

    // An environment variable. False when it is not set.
    bool GetEnvironmentValue(const std::wstring& name, std::wstring& out);

    // Expand %NAME% references to environment variables; unknown names are left
    // as written.
    std::wstring ExpandEnvironment(const std::wstring& text);

    // Look up the platform's well-known folders once (Saved Games, Documents,
    // ...). Call before any file hook is enabled: the lookup may read files or
    // the registry, and later reads would re-enter the hooks.
    void ResolveKnownFolders();

    // A well-known folder by its path-token name in upper case (L"SAVEDGAMES",
    // L"DOCUMENTS", ...), without a trailing separator. False when the name is
    // not one, or ResolveKnownFolders found nothing for it.
    bool KnownFolder(const std::wstring& upperName, std::wstring& out);

    // The account name of the user the process runs as.
    bool CurrentUserName(std::wstring& out);

    // ── Time ──────────────────────────────────────────────────────────────────

    struct LocalTime
    {
        int year, month, day, hour, minute, second;
    };

    LocalTime Now();

    // ── Process and launcher ──────────────────────────────────────────────────

    unsigned long ProcessId();

    // A UTF-8 value the launcher handed this process before the Interposer
    // loaded, by name (L"InterposerConfig", L"InterposerBaseDirectory", ...).
    // On Windows that is the named mapping Local\<name>_<pid>. Empty when the
    // launcher did not provide it.
    std::string ReadLauncherValue(const wchar_t* name);

    // ── Modules ───────────────────────────────────────────────────────────────

    using Module = void*;

    // This module: what plugins receive, and what a secondary copy forwards to.
    Module SelfModule();

    // Load a library. On failure returns null and describes why in `error`, as
    // it is written into the log (e.g. L"error 126").
    Module LoadModule(const std::wstring& path, std::wstring& error);

    void* FindSymbol(Module module, const char* name);
    void  UnloadModule(Module module);

    // The file extensions a plugin may have, with the dot, lower case.
    const std::vector<std::wstring>& PluginExtensions();
}
