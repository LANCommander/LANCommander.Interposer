#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include <string>
#include <vector>
#include <regex>
#include <array>
#include <cstdint>

// A compiled file-redirect rule loaded from FileRedirects in Config.yml.
struct FileRedirect {
    std::wregex  pattern;     // ECMAScript regex, case-insensitive
    std::wstring patternText; // original pattern source; std::wregex does not retain it
    std::wstring replacement; // ECMAScript format string; %ENVVAR% expanded after substitution
};

// Verbosity of the session log. Info reproduces the historical output exactly;
// Debug and Trace only ever add lines. Set from Logging.Level in Config.yml.
enum class LogLevel {
    Info  = 0,  // default — access lines only
    Debug = 1,  // + redirect hit/miss diagnostics
    Trace = 2   // + one line per redirect rule evaluated and rejected
};

// Reported by ApplyFileRedirects so callers can tell a genuine miss from a rule
// that fired but produced an unchanged string, and can name the rule responsible.
struct FileRedirectMatch {
    bool         matched   = false; // a rule fired, regardless of the resulting string
    size_t       ruleIndex = 0;     // 0-based index of the rule that fired
    size_t       ruleCount = 0;     // rules configured
    std::wstring pattern;           // pattern of the rule that fired; populated at Debug+
    std::vector<std::wstring> tried; // patterns evaluated and rejected; populated at Trace
};

// A FastDL path mapping loaded from fastDLPaths in interposer.yaml.
struct FastDLPath {
    std::wstring localPrefix;   // local directory prefix, trailing backslash included
    std::wstring remoteSubPath; // URL sub-path under BaseUrl, e.g. baseq3
};

// An inclusive port range used to filter out server browser / non-game-server ports.
struct PortRange { int min; int max; };

// A DNS redirect rule: when the game asks the resolver for a hostname matching
// `pattern`, the hook substitutes the regex result (with $1..$9 capture groups)
// before calling the real getaddrinfo / gethostbyname / GetAddrInfoEx.
struct DnsRedirect {
    std::wregex  pattern;     // ECMAScript regex, case-insensitive
    std::wstring replacement; // ECMAScript format string (supports $1..$9)
};

// A parsed IPv4 CIDR filter from NetworkAdapters.Subnets. `network` and `mask`
// are stored in HOST byte order with `network` pre-masked, so a match is simply
// (ntohl(addr) & mask) == network.
struct SubnetFilter {
    uint32_t network;
    uint32_t mask;
};

// Populated by LoadConfig(). Read-only after that.
extern bool         g_logFiles;          // true = log file I/O operations
extern bool         g_logRegistry;       // true = log registry operations
extern LogLevel     g_logLevel;          // Logging.Level; Info reproduces pre-existing output
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
extern bool                      g_fastdlProbeConnections;   // true = collect server addresses and probe at download time
extern int                       g_fastdlProbePort;          // HTTP port to probe (default 80)
extern std::wstring              g_fastdlProbePath;          // HTTP path to probe (default "/")
extern int                       g_fastdlProbeTimeout;       // probe request timeout in ms (default 2000)
extern std::vector<PortRange>    g_fastdlFilteredPorts;      // port ranges to skip when collecting addresses

extern bool         g_logPlugins;       // true = log plugin load/unload/config events
extern bool         g_logIdentity;     // true = log identity override operations
extern bool         g_logRichPresence;  // true = log rich presence updates
extern bool         g_logDnsRedirects;  // true = log DNS redirect matches
extern bool         g_logNetwork;       // true = log connection/DNS events

extern std::vector<DnsRedirect> g_dnsRedirects; // DNS hostname redirects (case-insensitive)

// NetworkAdapters — allow-list controlling which adapters games may enumerate.
// An adapter is KEPT if it matches ANY populated list. g_naAnyFilters is the
// fast-path gate (false when all three lists are empty → feature is a no-op).
extern bool                            g_naEnabled;    // NetworkAdapters.Enabled
extern bool                            g_naAnyFilters; // any of the three lists populated
extern std::vector<SubnetFilter>       g_naSubnets;    // IPv4 CIDR filters
extern std::vector<std::wregex>        g_naNames;      // vs FriendlyName + Description
extern std::vector<std::array<BYTE,6>> g_naMacs;       // physical addresses

// Rich presence config — read by richpresence.cpp after LoadConfig().
extern bool        g_rpDiscordEnabled;
extern std::string g_rpDiscordAppId;
extern int         g_rpDefaultType;
extern std::string g_rpDefaultName;
extern std::string g_rpDefaultDetails;
extern std::string g_rpDefaultDetailsUrl;
extern std::string g_rpDefaultState;
extern std::string g_rpDefaultStateUrl;
extern std::string g_rpDefaultLargeImageKey;
extern std::string g_rpDefaultLargeImageText;
extern std::string g_rpDefaultSmallImageKey;
extern std::string g_rpDefaultSmallImageText;
extern std::string g_rpDefaultButton1Text;
extern std::string g_rpDefaultButton1Url;
extern std::string g_rpDefaultButton2Text;
extern std::string g_rpDefaultButton2Url;

// Parse <dlldir>\.interposer\Config.yml and open <dlldir>\.interposer\Logs\<timestamp>.log. Call before MH_EnableHook.
void LoadConfig();

// Flush and close the log file. Call from RemoveHooks().
void CloseLog();

// Expand %VARNAME% tokens using Windows environment variables.
std::wstring ExpandEnvVars(const std::wstring& input);

// Return the redirected path if any rule matches, otherwise return path unchanged.
// Pass outMatch to learn whether a rule actually fired and which one — the return
// value alone cannot distinguish a miss from a rule whose replacement equals the input.
std::wstring ApplyFileRedirects(const std::wstring& path, FileRedirectMatch* outMatch = nullptr);

// Return the redirected hostname if the first DnsRedirects rule matches `host`
// (ECMAScript regex, case-insensitive, partial match — anchor with ^ $ for
// exact match). Otherwise return `host` unchanged.
std::wstring ApplyDnsRedirect(const std::wstring& host);

// Thread-safe log writers. No-op when the respective flag is false or no log file is open.
void LogFileAccess(const wchar_t* verb, const wchar_t* sourcePath, const wchar_t* redirectionPath = nullptr);
void LogRegistryAccess(const wchar_t* verb, const wchar_t* keyPath, const wchar_t* valueName = nullptr);
void LogFastDLAccess(const wchar_t* verb, const wchar_t* url, const wchar_t* localPath);
void LogPluginEvent(const wchar_t* verb, const wchar_t* info);
void LogIdentityAccess(const wchar_t* verb, const wchar_t* info);
void LogRichPresence(const wchar_t* verb, const wchar_t* info);
void LogNetworkAccess(const wchar_t* verb, const wchar_t* address, const wchar_t* info = nullptr);

// Redirect diagnostics. No-op below Debug level or when the subsystem flag is false.
// Unlike LogFileAccess/LogRegistryAccess these deliberately do NOT fire plugin or
// named-pipe callbacks — the diagnostic verbs are log-only and must not widen the
// event vocabulary consumers depend on.
void LogFileDiag(const wchar_t* verb, const wchar_t* path, const wchar_t* detail = nullptr);
void LogRegistryDiag(const wchar_t* verb, const wchar_t* keyPath, const wchar_t* detail = nullptr);

// Log a DNS redirect. Gated by the Logging.DnsRedirects flag (default true).
void LogDnsRedirect(const wchar_t* fromHost, const wchar_t* toHost);

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

// Register default configuration for a plugin. pluginName is the key under
// Plugins: in Config.yml (e.g. L"CDKey"). yamlDefaults is a YAML map body
// defining the default keys and values (e.g. L"Mask: '****'\nKeyPath: ''").
//
// If a Plugins.<pluginName> section already exists in Config.yml, the defaults
// are NOT written — the user's existing configuration is preserved. Otherwise
// the defaults are merged into the in-memory config (immediately available via
// InterposerGetConfigString) and appended to Config.yml on disk.
//
// Returns TRUE if the section already existed or defaults were written
// successfully. Returns FALSE on error (bad YAML, file write failure).
extern "C" __declspec(dllexport) BOOL InterposerRegisterPluginConfig(const wchar_t* pluginName, const wchar_t* yamlDefaults);
