#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <regex>

// A compiled file-redirect rule loaded from [FileRedirects] in interposer.ini.
struct FileRedirect {
    std::wregex  pattern;     // ECMAScript regex, case-insensitive
    std::wstring replacement; // ECMAScript format string; %ENVVAR% expanded after substitution
};

// A FastDL path mapping loaded from [FastDLPaths] in interposer.ini.
struct FastDLPath {
    std::wstring localPrefix;   // local directory prefix, trailing backslash included
    std::wstring remoteSubPath; // URL sub-path under BaseUrl, e.g. baseq3
};

// Populated by LoadConfig(). Read-only after that.
extern bool g_logFiles;     // true = log file I/O operations
extern bool g_logRegistry;  // true = log registry operations

extern bool                      g_fastdlEnabled;
extern bool                      g_logFastDL;
extern std::wstring              g_fastdlBaseUrl;
extern std::vector<std::wstring> g_fastdlAllowedExtensions; // empty = allow all
extern std::vector<FastDLPath>   g_fastdlPaths;

// Parse <dlldir>\interposer.ini and open the log file. Call before MH_EnableHook.
void LoadConfig();

// Flush and close the log file. Call from RemoveHooks().
void CloseLog();

// Expand %VARNAME% tokens using Windows environment variables.
std::wstring ExpandEnvVars(const std::wstring& input);

// Return the redirected path if any rule matches, otherwise return path unchanged.
std::wstring ApplyFileRedirects(const std::wstring& path);

// Thread-safe log writers. No-op when the respective flag is false or no log file is open.
void LogFileAccess(const wchar_t* verb, const wchar_t* sourcePath, const wchar_t* redirectionPath = nullptr);
void LogRegistryAccess(const wchar_t* verb, const wchar_t* keyPath, const wchar_t* valueName = nullptr);
void LogFastDLAccess(const wchar_t* verb, const wchar_t* url, const wchar_t* localPath);
