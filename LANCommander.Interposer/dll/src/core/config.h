#pragma once
#include "platform.h"
#include <string>
#include <vector>
#include <regex>
#include <array>
#include <cstdint>

#include "relay_settings.h"

// A compiled file-redirect rule loaded from FileRedirects in Config.yml.
struct FileRedirect {
    std::wregex  pattern;       // ECMAScript regex, case-insensitive
    std::wstring patternText;   // pattern as compiled, %TOKEN% already substituted
    std::wstring patternSource; // pattern as written in Config.yml, before substitution
    std::wstring replacement;   // ECMAScript format string; %TOKEN% expanded after substitution
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
    std::wstring source;            // that rule as written, when %TOKEN% substitution changed it
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

// OsVersion — the version of Windows reported to the game. OsVersion.Version
// names a preset in kOsVersions (config.cpp); the individual Major/Minor/Build/
// ServicePack/ProductType keys then override single fields of it. Nothing is
// hooked unless g_osVersionEnabled, so the feature is inert by default.
extern bool         g_osVersionEnabled;    // OsVersion.Version selected something other than None
extern std::wstring g_osVersionName;       // resolved preset name, for logging
extern DWORD        g_osMajorVersion;
extern DWORD        g_osMinorVersion;
extern DWORD        g_osBuildNumber;
extern std::wstring g_osServicePack;       // szCSDVersion, e.g. "Service Pack 2"
extern WORD         g_osServicePackMajor;  // derived from the preset or parsed off ServicePack
extern WORD         g_osSuiteMask;         // wSuiteMask, derived from the product type
extern BYTE         g_osProductType;       // VER_NT_WORKSTATION or VER_NT_SERVER

extern bool         g_logPlugins;       // true = log plugin load/unload/config events
extern bool         g_logIdentity;     // true = log identity override operations
extern bool         g_logOsVersion;     // true = log the reported OS version and which APIs asked
extern bool         g_logRichPresence;  // true = log rich presence updates
extern bool         g_logDnsRedirects;  // true = log DNS redirect matches
extern bool         g_logNetwork;       // true = log connection/DNS events
extern bool         g_logDirectInput;   // true = log DirectInput bridge activity
extern bool         g_logAudio;         // true = log audio device enumeration/filtering
extern bool         g_logRelay;         // true = log network relay connection, peers and streams

extern std::vector<DnsRedirect> g_dnsRedirects; // DNS hostname redirects (case-insensitive)

// Registry — the .reg files that back the virtual registry, and whether that
// store is authoritative for every key rather than only the ones it contains.
//
// g_registryFiles is the overlay stack in Config.yml order: each file is loaded
// on top of the previous one, so a later file wins for any key/value the two
// share. The LAST entry is the writable layer — every runtime write lands
// there, and the earlier files are never modified. Paths are absolute, with
// %TOKEN% already expanded and relative entries resolved against the DLL's own
// directory. Empty means the historical default, <dlldir>\.interposer\Registry.reg.
extern std::vector<std::wstring> g_registryFiles;

// Registry.Isolated — when true every key is treated as virtual, so a game
// never reads or writes the real registry even for keys absent from the files.
extern bool                      g_registryIsolated;

// A DirectInput device class, taken from the low byte of DIDEVICEINSTANCE::dwDevType.
// Covers both encodings: DirectInput 3/7 numbers its types 1..4 and DirectInput 8
// numbers its own 0x11..0x1C, and the two ranges do not overlap, so one mapping
// serves both eras.
enum class DiDeviceClass {
    Unknown = 0,
    Device,
    Mouse,
    Keyboard,
    Joystick,
    Gamepad,
    Driving,
    Flight,
    FirstPerson,
    DeviceControl,
    ScreenPointer,
    Remote,
    Supplemental
};

// A DirectInput.DeviceFilter.Names rule. patternText is retained for Trace
// logging, because std::wregex does not keep its source.
struct DiNameFilter {
    std::wregex  pattern;
    std::wstring patternText;
};

// DirectInput — see dinput.cpp. The fix bridges games that ask for DirectInput
// 3/7 onto DirectInput 8, working around the record-array overrun in the legacy
// Windows dinput.dll. The filter is an allow-list applied to device enumeration
// for bridged and native DirectInput 8 games alike: a device is KEPT if it
// matches ANY populated list, and both lists empty means the filter is a no-op.
extern bool                       g_diFixLegacyEnum;  // DirectInput.FixLegacyDeviceEnumeration
extern bool                       g_diFilterEnabled;  // DirectInput.DeviceFilter.Enabled
extern std::vector<DiDeviceClass> g_diFilterClasses;  // DirectInput.DeviceFilter.Classes
extern std::vector<DiNameFilter>  g_diFilterNames;    // DirectInput.DeviceFilter.Names

// Classify a device from its dwDevType, and name a class for logging.
DiDeviceClass  DiClassFromDevType(DWORD devType);
const wchar_t* DiClassName(DiDeviceClass deviceClass);

// NetworkAdapters — allow-list controlling which adapters games may enumerate.
// An adapter is KEPT if it matches ANY populated list. g_naAnyFilters is the
// fast-path gate (false when all three lists are empty → feature is a no-op).
extern bool                            g_naEnabled;    // NetworkAdapters.Enabled
extern bool                            g_naAnyFilters; // any of the three lists populated
extern std::vector<SubnetFilter>       g_naSubnets;    // IPv4 CIDR filters
extern std::vector<std::wregex>        g_naNames;      // vs FriendlyName + Description
extern std::vector<std::array<BYTE,6>> g_naMacs;       // physical addresses

// The direction an audio endpoint carries data in. Output is a render endpoint
// (speakers, headphones), Input a capture one (microphone, line-in). Every
// audio API this filter covers splits its enumeration along the same line, so
// one value classifies a waveOut device, a DirectSound device and a Core Audio
// endpoint alike.
enum class AudioDirection {
    Unknown = 0,
    Output,
    Input
};

// AudioDevices - a per-direction allow-list over the audio devices a game may
// enumerate. Each direction has its own list, and the key's PRESENCE is what
// arms the filter for that direction:
//
//   key absent            that direction is not filtered at all
//   key present, empty    every device of that direction is hidden
//   key present, entries  only devices matching an entry are shown
//
// Entries are literal, matched case-insensitively as a substring against every
// string a device is known by -- friendly name, description, Core Audio
// endpoint ID, device instance path, DirectSound GUID and the hardware IDs of
// the physical device behind it. They are deliberately NOT regular
// expressions: the strings a user pastes in here ("Speakers (NVIDIA
// Broadcast)", USB\VID_0965&PID_9001) are full of characters a regex would
// read as syntax.
extern bool                      g_adEnabled;      // AudioDevices.Enabled
extern bool                      g_adFilterOutput; // the Output key was present
extern bool                      g_adFilterInput;  // the Input key was present
extern std::vector<std::wstring> g_adOutput;       // AudioDevices.Output entries
extern std::vector<std::wstring> g_adInput;        // AudioDevices.Input entries

// Name a direction for logging.
const wchar_t* AudioDirectionName(AudioDirection direction);

// NetworkRelay — the settings in Config.yml. relay.cpp overlays the launcher's
// override mapping on a copy of these; this global itself is never changed
// after LoadConfig.
extern RelaySettings g_relayConfig;

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

// MatchStatistics -- where the InterposerMatch* reports go (matches.cpp). With
// a ServerUrl and Token the Interposer sends them to LANCommander itself; the
// token is never logged.
extern bool         g_msEnabled;        // MatchStatistics.Enabled (default true)
extern std::wstring g_msServerUrl;      // no trailing slash; empty = not configured
extern std::string  g_msToken;          // bearer token, matches:report scope
extern std::string  g_msGameId;         // LANCommander game id (GUID); required to report over HTTP
extern std::string  g_msServerId;       // LANCommander server id (GUID), or empty
extern std::string  g_msReporterKind;   // "Server" | "Host" | "Client"
extern std::string  g_msReporterName;   // empty = the computer name
extern int          g_msFlushInterval;  // ms between batches
extern bool         g_logMatches;       // Logging.Matches

// Parse <base>\.interposer\Config.yml, merge the launcher's config overlay over it,
// and open <base>\.interposer\Logs\<timestamp>.log. Call before MH_EnableHook.
//
// <base> is the DLL's own directory unless the launcher named another one in
// Local\InterposerBaseDirectory_<pid>, which is how one injected copy of the DLL
// serves many games without them sharing a config, a registry store or a log.
// The overlay, Local\InterposerConfig_<pid>, is a YAML document applied over
// Config.yml before anything is parsed: maps merge key by key, any other value
// replaces the file's, and a null removes the key.
void LoadConfig();

// The directory the Interposer treats as its own: the DLL's directory, or the
// launcher's base directory override. .interposer\ is looked up under it, relative
// Registry.Files and FastDL.DownloadDirectory entries resolve against it, and
// %INTERPOSERDIR% expands to it. Ends with a separator. Valid after LoadConfig.
const std::wstring& GetInterposerBaseDirectory();

// GetInterposerBaseDirectory() + L".interposer" and a separator.
std::wstring GetInterposerDataDirectory();

// Flush and close the log file. Call from RemoveHooks().
void CloseLog();

// Expand %VARNAME% tokens using environment variables.
std::wstring ExpandEnvVars(const std::wstring& input);

// Expand %TOKEN% references: environment variables first, then the known-folder
// tokens (%SAVEDGAMES%, %DOCUMENTS%, %MYGAMES%, ...) and %GAMEDIR% /
// %INTERPOSERDIR%. Unresolved tokens are left verbatim, delimiters included, and
// their names appended to `unresolved` when it is non-null.
//
// Set escapeForRegex when the result is spliced into a regular expression: it
// escapes the substituted *value* only, leaving the surrounding pattern intact.
std::wstring ExpandPathTokens(const std::wstring& input,
                              bool escapeForRegex = false,
                              std::vector<std::wstring>* unresolved = nullptr);

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

// Log DirectInput bridge activity. Gated by the Logging.DirectInput flag (default false).
// Log-only: deliberately does not fire plugin or named-pipe callbacks.
void LogDirectInput(const wchar_t* verb, const wchar_t* info, const wchar_t* detail = nullptr);

// Log the OS version reported to the game. Gated by the Logging.OsVersion flag
// (default false). Log-only, for the same reason as LogDirectInput.
void LogOsVersion(const wchar_t* verb, const wchar_t* info, const wchar_t* detail = nullptr);

// Per-device enumeration diagnostics. Additionally requires Logging.Level Debug
// (or Trace), because a busy HID stack produces a line per device per call.
void LogDirectInputDiag(const wchar_t* verb, const wchar_t* info, const wchar_t* detail = nullptr);

// Log audio device enumeration and filtering. Gated by the Logging.Audio flag
// (default false). Log-only, for the same reason as LogDirectInput.
void LogAudio(const wchar_t* verb, const wchar_t* info, const wchar_t* detail = nullptr);

// Per-device audio enumeration diagnostics. Additionally requires Logging.Level
// Debug (or Trace) -- a machine with many endpoints produces a line per device
// per enumeration call.
void LogAudioDiag(const wchar_t* verb, const wchar_t* info, const wchar_t* detail = nullptr);

// Log network relay activity. Gated by the Logging.Relay flag (default false).
// Log-only, for the same reason as LogDirectInput: relay state reaches .NET and
// plugin consumers through their own events, not through these lines.
void LogRelay(const wchar_t* verb, const wchar_t* info, const wchar_t* detail = nullptr);

// Per-packet and per-portal relay diagnostics. Additionally requires
// Logging.Level Debug; the per-datagram lines require Trace.
void LogRelayDiag(const wchar_t* verb, const wchar_t* info, const wchar_t* detail = nullptr);

// Write a line of the voice library's own output. Not gated here: the patch
// sets the threshold with interposer_comms_set_session_logging, and
// voice/common/log.cpp applies it before calling. Log-only.
void LogVoice(const wchar_t* verb, const wchar_t* message);

// Log match statistics: matches started and ended, batches sent. Gated by the
// Logging.Matches flag (default false). Log-only: the steps themselves reach
// .NET and plugins through the match callback. The Diag form needs Debug and
// logs every event.
void LogMatch(const wchar_t* verb, const wchar_t* info, const wchar_t* detail = nullptr);
void LogMatchDiag(const wchar_t* verb, const wchar_t* info, const wchar_t* detail = nullptr);

// Redirect diagnostics. No-op below Debug level or when the subsystem flag is false.
// Unlike LogFileAccess/LogRegistryAccess these deliberately do NOT fire plugin or
// named-pipe callbacks — the diagnostic verbs are log-only and must not widen the
// event vocabulary consumers depend on.
void LogFileDiag(const wchar_t* verb, const wchar_t* path, const wchar_t* detail = nullptr);
void LogRegistryDiag(const wchar_t* verb, const wchar_t* keyPath, const wchar_t* detail = nullptr);

// Log a DNS redirect. Gated by the Logging.DnsRedirects flag (default true).
void LogDnsRedirect(const wchar_t* fromHost, const wchar_t* toHost);

// The plugin API (InterposerGetUsername, InterposerLog, InterposerGetConfigString,
// InterposerRegisterPluginConfig, InterposerSetConfigString) is declared in the
// public header.
#include "LANCommander.Interposer.h"
