#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include <string>
#include <vector>
#include <regex>

// A compiled file-redirect rule loaded from fileRedirects in interposer.yaml.
struct FileRedirect {
    std::wregex  pattern;     // ECMAScript regex, case-insensitive
    std::wstring replacement; // ECMAScript format string; %ENVVAR% expanded after substitution
};

// A FastDL path mapping loaded from fastDLPaths in interposer.yaml.
struct FastDLPath {
    std::wstring localPrefix;   // local directory prefix, trailing backslash included
    std::wstring remoteSubPath; // URL sub-path under BaseUrl, e.g. baseq3
};

// Populated by LoadConfig(). Read-only after that.
extern bool         g_logFiles;         // true = log file I/O operations
extern bool         g_logRegistry;      // true = log registry operations
extern bool         g_borderlessEnabled; // true = apply borderless fullscreen to game windows
extern std::wstring g_username;          // non-empty = override GetUserNameW/A return value
extern std::wstring g_computername;      // non-empty = override GetComputerNameW/A return value

extern bool                      g_fastdlEnabled;
extern bool                      g_logFastDL;
extern std::wstring              g_fastdlBaseUrl;
extern std::vector<std::wstring> g_fastdlAllowedExtensions; // empty = allow all
extern std::vector<FastDLPath>   g_fastdlPaths;
extern bool                      g_fastdlUseDownloadDir;     // true = write to overlay dir (default)
extern std::wstring              g_fastdlDownloadDir;        // empty = <dlldir>\downloads
extern bool                      g_fastdlBlockSensitiveFiles; // true = block overwriting sensitive files (default)
extern bool                      g_fastdlProbeConnections;   // true = probe discovered server addresses for FastDL
extern int                       g_fastdlProbePort;          // HTTP port to probe (default 80)
extern std::wstring              g_fastdlProbePath;          // HTTP path to probe (default "/")

extern bool         g_logNetwork;       // true = log connection/DNS events

// Parse <dlldir>\.interposer\Config.yml and open <dlldir>\.interposer\Logs\<timestamp>.log. Call before MH_EnableHook.
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
void LogNetworkAccess(const wchar_t* verb, const wchar_t* address, const wchar_t* info = nullptr);

// Log a MinHook hook installation result. Always written regardless of other logging flags.
// Pass the MH_STATUS value returned by MH_CreateHookApi.
void LogHookInit(const wchar_t* module, const char* fn, MH_STATUS status);

// ---------------------------------------------------------------------------
// Plugin API — exported by name, resolved by plugins via GetProcAddress.
// ---------------------------------------------------------------------------

// Return the effective username (configured override or real Windows account name).
// bufferSize is in wchar_t units including the null terminator.
extern "C" __declspec(dllexport) BOOL InterposerGetUsername(wchar_t* buffer, DWORD bufferSize);

// Write a line to the session log regardless of logging flags.
// verb should be padded to ~15 chars for alignment, e.g. "[MYPLUGIN]     "
extern "C" __declspec(dllexport) void InterposerLog(const wchar_t* verb, const wchar_t* message);

// Read a scalar value from Config.yml by dot-separated YAML path.
// Returns TRUE on success; FALSE if the key is missing, not scalar, or buffer too small.
// Example: InterposerGetConfigString(L"Plugins.MyPlugin.Setting", buf, ARRAYSIZE(buf))
extern "C" __declspec(dllexport) BOOL InterposerGetConfigString(const wchar_t* dotPath, wchar_t* buffer, DWORD bufferSize);
