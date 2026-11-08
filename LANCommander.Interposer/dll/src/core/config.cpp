#include "config.h"
#include "instance.h"
#include "callbacks.h"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <array>
#include <cctype>
#include <cwctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <yaml-cpp/yaml.h>

// ---------------------------------------------------------------------------
// Globals (definitions)
// ---------------------------------------------------------------------------
bool         g_logFiles         = false;
bool         g_logRegistry      = false;
LogLevel     g_logLevel         = LogLevel::Info;
std::wstring g_username;
std::wstring g_computername;

bool                      g_fastdlEnabled             = false;
bool                      g_logFastDL                 = true;
std::wstring              g_fastdlBaseUrl;
std::vector<std::wstring> g_fastdlAllowedExtensions;
std::vector<FastDLPath>   g_fastdlPaths;
bool                      g_fastdlUseDownloadDir      = true;
std::wstring              g_fastdlDownloadDir;
bool                      g_fastdlBlockSensitiveFiles = true;
bool                      g_fastdlProbeConnections    = false;
int                       g_fastdlProbePort           = 80;
std::wstring              g_fastdlProbePath           = L"/";
int                       g_fastdlProbeTimeout        = 2000;
std::vector<PortRange>    g_fastdlFilteredPorts       = {{ 23000, 23009 }};

bool         g_osVersionEnabled   = false;
std::wstring g_osVersionName;
DWORD        g_osMajorVersion     = 0;
DWORD        g_osMinorVersion     = 0;
DWORD        g_osBuildNumber      = 0;
std::wstring g_osServicePack;
WORD         g_osServicePackMajor = 0;
WORD         g_osSuiteMask        = 0;
BYTE         g_osProductType      = VER_NT_WORKSTATION;

bool         g_logPlugins       = false;
bool         g_logIdentity     = false;
bool         g_logOsVersion    = false;
bool         g_logRichPresence  = false;
bool         g_logDnsRedirects = true;
bool         g_logNetwork      = false;
bool         g_logDirectInput  = false;
bool         g_logAudio        = false;
bool         g_logRelay        = false;

std::vector<DnsRedirect> g_dnsRedirects;

std::vector<std::wstring> g_registryFiles;
bool                      g_registryIsolated = false;

bool                       g_diFixLegacyEnum = false;
bool                       g_diFilterEnabled = false;
std::vector<DiDeviceClass> g_diFilterClasses;
std::vector<DiNameFilter>  g_diFilterNames;

RelaySettings                   g_relayConfig;

bool                            g_naEnabled    = false;
bool                            g_naAnyFilters = false;
std::vector<SubnetFilter>       g_naSubnets;
std::vector<std::wregex>        g_naNames;
std::vector<std::array<BYTE,6>> g_naMacs;

bool                      g_adEnabled      = false;
bool                      g_adFilterOutput = false;
bool                      g_adFilterInput  = false;
std::vector<std::wstring> g_adOutput;
std::vector<std::wstring> g_adInput;

// Rich presence
bool        g_rpDiscordEnabled         = false;
std::string g_rpDiscordAppId;
int         g_rpDefaultType            = 0;
std::string g_rpDefaultName;
std::string g_rpDefaultDetails;
std::string g_rpDefaultDetailsUrl;
std::string g_rpDefaultState;
std::string g_rpDefaultStateUrl;
std::string g_rpDefaultLargeImageKey;
std::string g_rpDefaultLargeImageText;
std::string g_rpDefaultSmallImageKey;
std::string g_rpDefaultSmallImageText;
std::string g_rpDefaultButton1Text;
std::string g_rpDefaultButton1Url;
std::string g_rpDefaultButton2Text;
std::string g_rpDefaultButton2Url;

bool         g_msEnabled        = true;
std::wstring g_msServerUrl;
std::string  g_msToken;
std::string  g_msGameId;
std::string  g_msServerId;
std::string  g_msReporterKind   = "Server";
std::string  g_msReporterName;
int          g_msFlushInterval  = 2000;
bool         g_logMatches       = false;

static std::vector<FileRedirect> g_redirects;
static platform::FileHandle      g_logHandle = platform::kInvalidFile;
static std::mutex                g_logMutex;
static std::wstring              g_configFilePath; // set by LoadConfig(), used by InterposerRegisterPluginConfig

// Persisted YAML root for plugin config queries (read-only after LoadConfig).
// Config.yml with the launcher's overlay merged over it: what the session runs on.
static YAML::Node        g_configRoot;
static std::shared_mutex g_configMutex;

// Config.yml alone, without the overlay. Writes back to the file are rendered
// from this, so a fallback re-render never bakes an overlay value into a file
// that the next launch may give a different overlay. Guarded by g_configMutex.
static YAML::Node        g_fileRoot;

// ---------------------------------------------------------------------------
// ExpandEnvVars
// ---------------------------------------------------------------------------
std::wstring ExpandEnvVars(const std::wstring& input)
{
    return platform::ExpandEnvironment(input);
}

// ---------------------------------------------------------------------------
// Path tokens
// ---------------------------------------------------------------------------
// Captured by LoadConfig() so %GAMEDIR% / %INTERPOSERDIR% resolve without
// walking the module list again on every rule.
static std::wstring g_gameDirectory;
static std::wstring g_interposerDirectory;

// What GetInterposerBaseDirectory returns: g_interposerDirectory with its
// trailing backslash, which is how every caller wants to append to it.
static std::wstring g_baseDirectory;

// The known-folder tokens (%SAVEDGAMES%, %DOCUMENTS%, ...) are the platform's
// to resolve: platform::ResolveKnownFolders looks them up once, from LoadConfig,
// and platform::KnownFolder answers from that cache.

using platform::Utf8ToWide;
using platform::WideToUtf8;

static std::wstring UpperCase(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());

    for (wchar_t c : s)
        out += static_cast<wchar_t>(std::towupper(c));

    return out;
}

// Resolve one %NAME%. Environment first, so the feature can never change the
// meaning of a token that already worked. Returns false if nothing matches, in
// which case the caller leaves the token text alone.
static bool ResolvePathToken(const std::wstring& name, std::wstring& out)
{
    if (name.empty())
        return false;

    if (std::wstring value; platform::GetEnvironmentValue(name, value))
    {
        platform::TrimTrailingSeparators(value);
        out = value;
        return true;
    }

    const std::wstring wanted = UpperCase(name);

    if (wanted == L"GAMEDIR" && !g_gameDirectory.empty())
    {
        out = g_gameDirectory;
        return true;
    }

    if (wanted == L"INTERPOSERDIR" && !g_interposerDirectory.empty())
    {
        out = g_interposerDirectory;
        return true;
    }

    return platform::KnownFolder(wanted, out);
}

// Escape every ECMAScript metacharacter so a substituted path is matched
// literally. Backslash is the one that actually bites: an unescaped
// C:\Users\Pat\AppData would read as the escapes \U, \P and \A.
static std::wstring RegexEscape(const std::wstring& s)
{
    static const std::wstring kMeta = L"\\^$.|?*+()[]{}";

    std::wstring out;
    out.reserve(s.size() * 2);

    for (wchar_t c : s)
    {
        if (kMeta.find(c) != std::wstring::npos)
            out += L'\\';

        out += c;
    }

    return out;
}

// Substitute %NAME% tokens. Unresolved tokens are copied through verbatim,
// delimiters included, so a typo degrades to "rule never matches" rather than
// silently deleting part of the path; the names are reported via `unresolved`
// so LoadConfig can warn about them once the log exists.
//
// `escapeForRegex` escapes the substituted *value* only — the surrounding text
// is the author's regex and must stay untouched.
std::wstring ExpandPathTokens(const std::wstring& input,
                              bool escapeForRegex,
                              std::vector<std::wstring>* unresolved)
{
    std::wstring out;
    out.reserve(input.size());

    size_t i = 0;

    while (i < input.size())
    {
        if (input[i] != L'%')
        {
            out += input[i++];
            continue;
        }

        size_t close = input.find(L'%', i + 1);

        // A lone % with no partner is just a character.
        if (close == std::wstring::npos)
        {
            out += input.substr(i);
            break;
        }

        std::wstring name = input.substr(i + 1, close - i - 1);
        std::wstring value;

        if (ResolvePathToken(name, value))
        {
            out += escapeForRegex ? RegexEscape(value) : value;
        }
        else
        {
            out += input.substr(i, close - i + 1);

            if (unresolved && !name.empty())
                unresolved->push_back(name);
        }

        i = close + 1;
    }

    return out;
}

// ---------------------------------------------------------------------------
// ParseCidr — parse "A.B.C.D/N" (or bare "A.B.C.D", implying /32) into a
// host-order network+mask. Parsing the dotted quad by hand yields host order
// directly, so no winsock dependency or ntohl is needed here. Returns false on
// malformed input.
// ---------------------------------------------------------------------------
static bool ParseCidr(const std::string& cidr, SubnetFilter& out)
{
    std::string ipPart = cidr;
    int prefix = 32;

    size_t slash = cidr.find('/');
    if (slash != std::string::npos)
    {
        ipPart = cidr.substr(0, slash);
        try { prefix = std::stoi(cidr.substr(slash + 1)); }
        catch (...) { return false; }
    }
    if (prefix < 0 || prefix > 32)
        return false;

    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(ipPart.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;

    const uint32_t host = (a << 24) | (b << 16) | (c << 8) | d;
    const uint32_t mask = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
    out.network = host & mask;
    out.mask    = mask;
    return true;
}

// ---------------------------------------------------------------------------
// ParseMac — parse a 6-octet physical address. Accepts colon- or dash-separated
// (or any non-hex separators); case-insensitive. Returns false unless exactly 6
// octets are read.
// ---------------------------------------------------------------------------
static bool ParseMac(const std::string& s, std::array<BYTE, 6>& out)
{
    int n = 0;
    const char* p = s.c_str();
    while (*p && n < 6)
    {
        if (!isxdigit(static_cast<unsigned char>(*p))) { ++p; continue; }

        unsigned value = 0;
        int digits = 0;
        while (*p && isxdigit(static_cast<unsigned char>(*p)) && digits < 2)
        {
            const char ch = *p++;
            const unsigned hv = (ch <= '9')
                ? static_cast<unsigned>(ch - '0')
                : static_cast<unsigned>(tolower(static_cast<unsigned char>(ch)) - 'a' + 10);
            value = (value << 4) | hv;
            ++digits;
        }
        out[n++] = static_cast<BYTE>(value);
    }
    return n == 6;
}

// ---------------------------------------------------------------------------
// ApplyFileRedirects
// ---------------------------------------------------------------------------
// Collapse runs of backslashes to a single backslash, preserving
// a leading \\ for UNC paths and \\?\ extended-length prefixes.
static std::wstring NormalizeBackslashes(const std::wstring& path)
{
    if (path.size() < 2)
        return path;

    std::wstring out;
    out.reserve(path.size());

    // Preserve leading \\ (UNC / extended-length prefix)
    size_t start = 0;
    if (path[0] == L'\\' && path[1] == L'\\')
    {
        out += L"\\\\";
        start = 2;
    }

    for (size_t i = start; i < path.size(); ++i)
    {
        if (path[i] == L'\\' && !out.empty() && out.back() == L'\\')
            continue;
        out += path[i];
    }

    return out;
}

std::wstring ApplyFileRedirects(const std::wstring& path, FileRedirectMatch* outMatch)
{
    if (outMatch)
        outMatch->ruleCount = g_redirects.size();

    if (g_redirects.empty())
        return path;

    for (size_t i = 0; i < g_redirects.size(); ++i)
    {
        const auto& redirect = g_redirects[i];

        std::wstring result = std::regex_replace(
            path, redirect.pattern, redirect.replacement,
            std::regex_constants::format_first_only);

        if (result != path)
        {
            // Record the match here rather than by comparing the return value:
            // ExpandEnvVars/NormalizeBackslashes can collapse the result back to
            // `path`, which would otherwise look identical to a miss.
            if (outMatch)
            {
                outMatch->matched   = true;
                outMatch->ruleIndex = i;

                if (g_logLevel >= LogLevel::Debug)
                {
                    outMatch->pattern = redirect.patternText;
                    outMatch->source  = redirect.patternSource;
                }
            }

            // ExpandPathTokens handles the known-folder tokens and leaves
            // anything it does not recognize alone; ExpandEnvVars then gets its
            // usual shot at the remainder, so existing configs are unaffected.
            return NormalizeBackslashes(
                ExpandEnvVars(ExpandPathTokens(result, /*escapeForRegex=*/false, nullptr)));
        }

        if (outMatch && g_logLevel >= LogLevel::Trace)
            outMatch->tried.push_back(redirect.patternText);
    }

    return path;
}

// ---------------------------------------------------------------------------
// ApplyDnsRedirect
// ---------------------------------------------------------------------------
std::wstring ApplyDnsRedirect(const std::wstring& host)
{
    if (g_dnsRedirects.empty() || host.empty())
        return host;

    for (const auto& rule : g_dnsRedirects)
    {
        std::wstring result = std::regex_replace(
            host, rule.pattern, rule.replacement,
            std::regex_constants::format_first_only);

        if (result != host)
            return result;
    }

    return host;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

// Strip any existing [] wrapper and whitespace, truncate to 16 chars,
// re-wrap as [content] right-padded to 18 total characters.
static std::wstring NormalizeVerb(const wchar_t* raw)
{
    std::wstring s(raw ? raw : L"");

    // Strip leading '['
    if (!s.empty() && s.front() == L'[') s.erase(s.begin());
    // Strip trailing whitespace then trailing ']'
    while (!s.empty() && s.back() == L' ') s.pop_back();
    if (!s.empty() && s.back() == L']') s.pop_back();
    while (!s.empty() && s.back() == L' ') s.pop_back();

    // Truncate content to 16 characters
    if (s.size() > 16) s.resize(16);

    // Re-wrap and right-pad: [<=16 chars] padded to 18 total
    std::wstring result;
    result.reserve(18);
    result += L'[';
    result += s;
    result += L']';
    while (result.size() < 18) result += L' ';
    return result;
}

static void WriteLogLine(const wchar_t* verb, const wchar_t* a, const wchar_t* b)
{
    std::lock_guard<std::mutex> lk(g_logMutex);

    if (g_logHandle == platform::kInvalidFile)
        return;

    const platform::LocalTime now = platform::Now();

    wchar_t timestamp[24];
    swprintf(timestamp, std::size(timestamp), L"%04d-%02d-%02d %02d:%02d:%02d",
        now.year, now.month, now.day, now.hour, now.minute, now.second);

    // Build wide line
    std::wstring line;

    line.reserve(512);

    line += timestamp;
    line += L"  ";
    line += NormalizeVerb(verb);
    line += L"  ";
    line += a;

    if (b && b[0] != L'\0')
    {
        line += L"  ->  ";
        line += b;
    }

    line += L"\r\n";

    const std::string utf8 = WideToUtf8(line);

    if (utf8.empty())
        return;

    platform::WriteToFile(g_logHandle, utf8.data(), utf8.size());
}

void LogFileAccess(const wchar_t* verb, const wchar_t* sourcePath, const wchar_t* redirectionPath)
{
    FireFileCallback(verb, sourcePath, redirectionPath);

    if (!g_logFiles)
        return;

    WriteLogLine(verb, sourcePath, redirectionPath);
}

void LogRegistryAccess(const wchar_t* verb, const wchar_t* keyPath, const wchar_t* valueName)
{
    FireRegistryCallback(verb, keyPath, valueName);

    if (!g_logRegistry)
        return;

    if (valueName && valueName[0] != L'\0')
    {
        std::wstring fullKeyPath(keyPath);

        fullKeyPath += L"\\";
        fullKeyPath += valueName;

        WriteLogLine(verb, fullKeyPath.c_str(), nullptr);
    }
    else
        WriteLogLine(verb, keyPath, nullptr);
}

void LogFileDiag(const wchar_t* verb, const wchar_t* path, const wchar_t* detail)
{
    if (!g_logFiles || g_logLevel < LogLevel::Debug)
        return;

    WriteLogLine(verb, path, detail);
}

void LogRegistryDiag(const wchar_t* verb, const wchar_t* keyPath, const wchar_t* detail)
{
    if (!g_logRegistry || g_logLevel < LogLevel::Debug)
        return;

    WriteLogLine(verb, keyPath, detail);
}

void LogFastDLAccess(const wchar_t* verb, const wchar_t* url, const wchar_t* localPath)
{
    if (!g_logFastDL)
        return;

    WriteLogLine(verb, url, localPath);
}

void LogPluginEvent(const wchar_t* verb, const wchar_t* info)
{
    if (!g_logPlugins)
        return;

    WriteLogLine(verb, info, nullptr);
}

void LogIdentityAccess(const wchar_t* verb, const wchar_t* info)
{
    if (!g_logIdentity)
        return;

    WriteLogLine(verb, info, nullptr);
}

void LogRichPresence(const wchar_t* verb, const wchar_t* info)
{
    if (!g_logRichPresence)
        return;

    WriteLogLine(verb, info, nullptr);
}

void LogNetworkAccess(const wchar_t* verb, const wchar_t* address, const wchar_t* info)
{
    if (!g_logNetwork)
        return;

    WriteLogLine(verb, address, info);
}

// Log-only, like LogFileDiag/LogRegistryDiag: the DirectInput bridge deliberately
// does NOT fire the plugin or named-pipe callbacks, so its verbs stay out of the
// event vocabulary in LANCommander.Interposer.NET\Events\.
// ---------------------------------------------------------------------------
// DirectInput device classes
// ---------------------------------------------------------------------------

namespace {

struct DiClassEntry {
    const char*   name;      // as written in DirectInput.DeviceFilter.Classes
    DiDeviceClass value;
    DWORD         dx7Type;   // DIDEVTYPE_*,   0 = no DirectInput 3/7 equivalent
    DWORD         dx8Type;   // DI8DEVTYPE_*
};

// The DirectInput 3/7 type numbers (1..4) and the DirectInput 8 ones
// (0x11..0x1C) occupy disjoint ranges, so a single table classifies both.
constexpr DiClassEntry kDiClasses[] = {
    { "Device",        DiDeviceClass::Device,        0x01, 0x11 },
    { "Mouse",         DiDeviceClass::Mouse,         0x02, 0x12 },
    { "Keyboard",      DiDeviceClass::Keyboard,      0x03, 0x13 },
    { "Joystick",      DiDeviceClass::Joystick,      0x04, 0x14 },
    { "Gamepad",       DiDeviceClass::Gamepad,       0,    0x15 },
    { "Driving",       DiDeviceClass::Driving,       0,    0x16 },
    { "Flight",        DiDeviceClass::Flight,        0,    0x17 },
    { "FirstPerson",   DiDeviceClass::FirstPerson,   0,    0x18 },
    { "DeviceControl", DiDeviceClass::DeviceControl, 0,    0x19 },
    { "ScreenPointer", DiDeviceClass::ScreenPointer, 0,    0x1A },
    { "Remote",        DiDeviceClass::Remote,        0,    0x1B },
    { "Supplemental",  DiDeviceClass::Supplemental,  0,    0x1C },
};

// ASCII case-insensitive equality.
bool EqualsNoCase(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b)
    {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }

    return *a == *b;
}

// Case-insensitive, so 'mouse' and 'Mouse' both work in Config.yml.
bool DiClassFromName(const std::string& name, DiDeviceClass& out)
{
    for (const DiClassEntry& entry : kDiClasses)
    {
        if (EqualsNoCase(entry.name, name.c_str()))
        {
            out = entry.value;
            return true;
        }
    }

    return false;
}

} // namespace

DiDeviceClass DiClassFromDevType(DWORD devType)
{
    // Only the low byte identifies the class; the next byte is the subtype and
    // the high bits carry flags such as DIDEVTYPE_HID.
    const DWORD type = devType & 0xFF;

    for (const DiClassEntry& entry : kDiClasses)
    {
        if ((entry.dx7Type && type == entry.dx7Type) || type == entry.dx8Type)
            return entry.value;
    }

    return DiDeviceClass::Unknown;
}

const wchar_t* DiClassName(DiDeviceClass deviceClass)
{
    switch (deviceClass)
    {
    case DiDeviceClass::Device:        return L"Device";
    case DiDeviceClass::Mouse:         return L"Mouse";
    case DiDeviceClass::Keyboard:      return L"Keyboard";
    case DiDeviceClass::Joystick:      return L"Joystick";
    case DiDeviceClass::Gamepad:       return L"Gamepad";
    case DiDeviceClass::Driving:       return L"Driving";
    case DiDeviceClass::Flight:        return L"Flight";
    case DiDeviceClass::FirstPerson:   return L"FirstPerson";
    case DiDeviceClass::DeviceControl: return L"DeviceControl";
    case DiDeviceClass::ScreenPointer: return L"ScreenPointer";
    case DiDeviceClass::Remote:        return L"Remote";
    case DiDeviceClass::Supplemental:  return L"Supplemental";
    default:                           return L"Unknown";
    }
}

// ---------------------------------------------------------------------------
// Audio device lists
// ---------------------------------------------------------------------------

namespace {

// Read one AudioDevices direction list. The key's presence is what arms the
// filter, so a key with nothing under it is an empty allow-list -- "hide every
// device of this direction" -- and is meaningfully different from the key not
// being there at all.
void ParseAudioDeviceList(const YAML::Node& node, std::vector<std::wstring>& out, bool& present)
{
    if (!node.IsDefined())
        return;

    present = true;

    if (!node.IsSequence())
        return;

    for (const auto& item : node)
    {
        std::string entry = item.as<std::string>("");

        if (!entry.empty())
            out.push_back(Utf8ToWide(entry));
    }
}

} // namespace

const wchar_t* AudioDirectionName(AudioDirection direction)
{
    switch (direction)
    {
    case AudioDirection::Output: return L"Output";
    case AudioDirection::Input:  return L"Input";
    default:                     return L"Unknown";
    }
}

// ---------------------------------------------------------------------------
// OS version presets
// ---------------------------------------------------------------------------

namespace {

struct OsVersionEntry {
    const char*    name;             // as written in OsVersion.Version
    DWORD          major;
    DWORD          minor;
    DWORD          build;
    const wchar_t* servicePack;      // szCSDVersion
    WORD           servicePackMajor;
    BYTE           productType;
};

// Build numbers are the last shipped build of each version, which is what a
// fully patched machine of that era would have reported.
//
// Windows XP x64 and Server 2003 are both 5.2.3790 and differ only in product
// type; both are listed because a game that special-cases servers needs the
// distinction, and a game that only reads the version number does not care.
constexpr OsVersionEntry kOsVersions[] = {
    { "Windows2000",       5,  0,  2195, L"Service Pack 4", 4, VER_NT_WORKSTATION },
    { "WindowsXP",         5,  1,  2600, L"Service Pack 3", 3, VER_NT_WORKSTATION },
    { "WindowsXPx64",      5,  2,  3790, L"Service Pack 2", 2, VER_NT_WORKSTATION },
    { "WindowsServer2003", 5,  2,  3790, L"Service Pack 2", 2, VER_NT_SERVER      },
    { "WindowsVista",      6,  0,  6002, L"Service Pack 2", 2, VER_NT_WORKSTATION },
    { "Windows7",          6,  1,  7601, L"Service Pack 1", 1, VER_NT_WORKSTATION },
    { "Windows8",          6,  2,  9200, L"",               0, VER_NT_WORKSTATION },
    { "Windows81",         6,  3,  9600, L"",               0, VER_NT_WORKSTATION },
    { "Windows10",        10,  0, 19045, L"",               0, VER_NT_WORKSTATION },
    { "Windows11",        10,  0, 22631, L"",               0, VER_NT_WORKSTATION },
    // Zeroes on purpose: Custom means "take everything from the explicit keys".
    { "Custom",            0,  0,     0, L"",               0, VER_NT_WORKSTATION },
};

// Lowercase and drop everything that is not a letter or digit, so that
// "Windows 8.1", "windows8.1" and "Windows81" all name the same preset. Also
// used for the ProductType value, which tolerates the same sloppiness.
std::string NormalizeOsToken(const std::string& name)
{
    std::string out;
    out.reserve(name.size());

    for (unsigned char c : name)
    {
        if (std::isalnum(c))
            out += static_cast<char>(std::tolower(c));
    }

    return out;
}

const OsVersionEntry* OsVersionFromName(const std::string& name)
{
    const std::string wanted = NormalizeOsToken(name);

    for (const OsVersionEntry& entry : kOsVersions)
    {
        if (NormalizeOsToken(entry.name) == wanted)
            return &entry;
    }

    return nullptr;
}

// "Service Pack 2" -> 2. Anything without a trailing number yields 0, which is
// also the correct answer for the versions that shipped without one.
WORD ServicePackMajorFromName(const std::wstring& servicePack)
{
    size_t end = servicePack.size();

    while (end > 0 && !iswdigit(servicePack[end - 1]))
        --end;

    size_t start = end;

    while (start > 0 && iswdigit(servicePack[start - 1]))
        --start;

    if (start == end)
        return 0;

    return static_cast<WORD>(std::wcstol(servicePack.substr(start, end - start).c_str(), nullptr, 10));
}

} // namespace

void LogDirectInputDiag(const wchar_t* verb, const wchar_t* info, const wchar_t* detail)
{
    if (!g_logDirectInput || g_logLevel < LogLevel::Debug)
        return;

    WriteLogLine(verb, info, detail);
}

void LogDirectInput(const wchar_t* verb, const wchar_t* info, const wchar_t* detail)
{
    if (!g_logDirectInput)
        return;

    WriteLogLine(verb, info, detail);
}

void LogAudioDiag(const wchar_t* verb, const wchar_t* info, const wchar_t* detail)
{
    if (!g_logAudio || g_logLevel < LogLevel::Debug)
        return;

    WriteLogLine(verb, info, detail);
}

void LogAudio(const wchar_t* verb, const wchar_t* info, const wchar_t* detail)
{
    if (!g_logAudio)
        return;

    WriteLogLine(verb, info, detail);
}

void LogRelay(const wchar_t* verb, const wchar_t* info, const wchar_t* detail)
{
    if (!g_logRelay)
        return;

    WriteLogLine(verb, info, detail);
}

void LogRelayDiag(const wchar_t* verb, const wchar_t* info, const wchar_t* detail)
{
    if (!g_logRelay || g_logLevel < LogLevel::Debug)
        return;

    WriteLogLine(verb, info, detail);
}

void LogMatch(const wchar_t* verb, const wchar_t* info, const wchar_t* detail)
{
    if (!g_logMatches)
        return;

    WriteLogLine(verb, info, detail);
}

void LogMatchDiag(const wchar_t* verb, const wchar_t* info, const wchar_t* detail)
{
    if (!g_logMatches || g_logLevel < LogLevel::Debug)
        return;

    WriteLogLine(verb, info, detail);
}

void LogVoice(const wchar_t* verb, const wchar_t* message)
{
    WriteLogLine(verb, message, nullptr);
}

void LogOsVersion(const wchar_t* verb, const wchar_t* info, const wchar_t* detail)
{
    if (!g_logOsVersion)
        return;

    WriteLogLine(verb, info, detail);
}

void LogDnsRedirect(const wchar_t* fromHost, const wchar_t* toHost)
{
    FireDnsCallback(fromHost, toHost);

    if (!g_logDnsRedirects)
        return;

    WriteLogLine(L"DNS REDIRECT", fromHost, toHost);
}

void CloseLog()
{
    std::lock_guard<std::mutex> lk(g_logMutex);

    if (g_logHandle != platform::kInvalidFile)
    {
        platform::CloseFile(g_logHandle);
        g_logHandle = platform::kInvalidFile;
    }
}

// ---------------------------------------------------------------------------
// Plugin API exports
// ---------------------------------------------------------------------------

// Return the effective username: the configured/injected override if set, otherwise
// the real account name.
// bufferSize is in wchar_t units and must include room for the null terminator.
// Returns TRUE on success, FALSE if the buffer is too small or the name cannot be read.
extern "C" INTERPOSER_API
BOOL InterposerGetUsername(wchar_t* buffer, DWORD bufferSize)
{
    FORWARD_TO_PRIMARY(InterposerGetUsername, buffer, bufferSize);

    if (!buffer || bufferSize == 0) return FALSE;

    std::wstring name = g_username;

    // No override configured — ask the OS (hooks not yet involved here).
    if (name.empty() && !platform::CurrentUserName(name))
        return FALSE;

    DWORD needed = static_cast<DWORD>(name.size() + 1);
    if (bufferSize < needed) return FALSE;
    wmemcpy(buffer, name.c_str(), needed);
    return TRUE;
}

// Write a line to the session log. Always writes regardless of logging flags.
// verb    — label shown in the log (e.g. L"MYPLUGIN" or L"[MYPLUGIN]"); automatically
//           stripped of existing brackets/whitespace, truncated to 16 chars, and
//           re-wrapped as [verb] right-padded to 18 characters.
// message — path or free-form text
extern "C" INTERPOSER_API
void InterposerLog(const wchar_t* verb, const wchar_t* message)
{
    FORWARD_TO_PRIMARY(InterposerLog, verb, message);

    if (!verb || !message) return;
    WriteLogLine(verb, message, nullptr);
}

// Split a dot-separated config path into UTF-8 segments. Returns an empty
// vector for a malformed path (empty, or an empty segment such as "A..B").
static std::vector<std::string> SplitDotPath(const wchar_t* dotPath)
{
    std::vector<std::string> segs;
    std::wstring wpath(dotPath);
    size_t start = 0;

    while (true)
    {
        size_t dot = wpath.find(L'.', start);
        std::wstring wseg = (dot == std::wstring::npos)
            ? wpath.substr(start)
            : wpath.substr(start, dot - start);

        if (wseg.empty()) return {};

        segs.push_back(WideToUtf8(wseg));

        if (dot == std::wstring::npos) break;
        start = dot + 1;
    }

    return segs;
}

// Walk `node` down `count` segments of `segs`, rebinding it to each child in turn.
// Returns false if any segment is missing.
//
// Rebinding has to go through reset(). yaml-cpp's Node::operator= does not repoint
// the handle -- it overwrites the node the handle refers to -- so the obvious
// `node = node[seg]` writes each child over its parent and leaves the root holding
// whatever the walk ended on. The lookup also goes through a const reference,
// because the non-const operator[] inserts the key it is asked for.
static bool WalkConfigPath(YAML::Node& node, const std::vector<std::string>& segs, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        const YAML::Node& parent = node;
        if (!parent.IsMap()) return false;

        YAML::Node child = parent[segs[i]];
        if (!child) return false;

        node.reset(child);
    }

    return true;
}

// Read a scalar value from Config.yml by dot-separated YAML path.
// dotPath    — e.g. L"Plugins.MyPlugin.Setting"
// buffer     — receives the null-terminated wide string value
// bufferSize — capacity of buffer in wchar_t units
// Returns TRUE on success, FALSE if the key does not exist, is not a scalar,
// or the buffer is too small.
extern "C" INTERPOSER_API
BOOL InterposerGetConfigString(const wchar_t* dotPath, wchar_t* buffer, DWORD bufferSize)
{
    FORWARD_TO_PRIMARY(InterposerGetConfigString, dotPath, buffer, bufferSize);

    if (!dotPath || !buffer || bufferSize == 0) return FALSE;

    std::vector<std::string> segs = SplitDotPath(dotPath);
    if (segs.empty()) return FALSE;

    std::shared_lock lk(g_configMutex);

    YAML::Node node = g_configRoot;
    if (!WalkConfigPath(node, segs, segs.size())) return FALSE;

    if (!node.IsScalar()) return FALSE;

    std::string value;
    try { value = node.as<std::string>(); }
    catch (...) { return FALSE; }

    const std::wstring wide = Utf8ToWide(value);

    if (wide.size() + 1 > bufferSize) return FALSE;

    wmemcpy(buffer, wide.c_str(), wide.size() + 1);
    return TRUE;
}

// ---------------------------------------------------------------------------
// InterposerRegisterPluginConfig
// ---------------------------------------------------------------------------

// Read the entire file at g_configFilePath into a UTF-8 string.
// Returns empty string on failure.
static std::string ReadConfigFileUtf8()
{
    std::vector<unsigned char> raw;

    if (!platform::ReadWholeFile(g_configFilePath, 4 * 1024 * 1024, raw))
        return {};

    // Decode to UTF-8 (handle BOM variants)
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
        return platform::Utf16LeToUtf8(raw.data() + 2, raw.size() - 2);

    int offset = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
    return std::string(reinterpret_cast<const char*>(raw.data()) + offset, raw.size() - offset);
}

// Write a UTF-8 string to g_configFilePath (overwrites existing content).
static bool WriteConfigFileUtf8(const std::string& content)
{
    return platform::WriteWholeFile(g_configFilePath, content);
}

// Emit a YAML::Node map as block-style lines, each indented by `indent` spaces.
static std::string EmitPluginYaml(const YAML::Node& node, int indent)
{
    std::string result;
    std::string pad(indent, ' ');

    for (auto it = node.begin(); it != node.end(); ++it)
    {
        std::string key = it->first.as<std::string>();
        YAML::Node  val = it->second;

        if (val.IsScalar())
        {
            YAML::Emitter em;
            em << val;
            result += pad + key + ": " + em.c_str() + "\n";
        }
        else if (val.IsSequence())
        {
            YAML::Emitter em;
            em << YAML::Flow << val;
            result += pad + key + ": " + em.c_str() + "\n";
        }
        else if (val.IsMap() && val.size() == 0)
        {
            result += pad + key + ": {}\n";
        }
        else if (val.IsMap())
        {
            result += pad + key + ":\n";
            result += EmitPluginYaml(val, indent + 2);
        }
        else if (val.IsNull())
        {
            result += pad + key + ":\n";
        }
    }

    return result;
}

extern "C" INTERPOSER_API
BOOL InterposerRegisterPluginConfig(const wchar_t* pluginName, const wchar_t* yamlDefaults)
{
    FORWARD_TO_PRIMARY(InterposerRegisterPluginConfig, pluginName, yamlDefaults);

    if (!pluginName || !yamlDefaults || pluginName[0] == L'\0')
        return FALSE;

    std::string nameUtf8     = WideToUtf8(std::wstring(pluginName));
    std::string defaultsUtf8 = WideToUtf8(std::wstring(yamlDefaults));

    if (nameUtf8.empty())
        return FALSE;

    // Parse the defaults YAML
    YAML::Node defaults;
    try { defaults = YAML::Load(defaultsUtf8); }
    catch (const YAML::Exception&)
    {
        LogPluginEvent(L"PLUGIN CONFIG", Utf8ToWide("Failed to parse defaults for " + nameUtf8).c_str());
        return FALSE;
    }

    if (!defaults.IsMap())
    {
        LogPluginEvent(L"PLUGIN CONFIG", Utf8ToWide("Defaults for " + nameUtf8 + " must be a YAML map").c_str());
        return FALSE;
    }

    std::unique_lock lk(g_configMutex);

    // If the plugin section already exists, do nothing — user config takes priority.
    YAML::Node plugins = g_configRoot["Plugins"];
    if (plugins && plugins.IsMap() && plugins[nameUtf8] && plugins[nameUtf8].IsDefined())
    {
        LogPluginEvent(L"PLUGIN CONFIG", Utf8ToWide(nameUtf8 + " already configured").c_str());
        return TRUE;
    }

    // Merge defaults into in-memory config
    if (!plugins || !plugins.IsMap())
        g_configRoot["Plugins"] = YAML::Node(YAML::NodeType::Map);

    g_configRoot["Plugins"][nameUtf8] = defaults;

    // And into the file's own tree, which a later fallback re-render writes from
    {
        if (!g_fileRoot.IsMap())
            g_fileRoot = YAML::Node(YAML::NodeType::Map);

        const YAML::Node& fileConst = g_fileRoot;
        YAML::Node filePlugins = fileConst["Plugins"];

        if (!filePlugins || !filePlugins.IsMap())
            g_fileRoot["Plugins"] = YAML::Node(YAML::NodeType::Map);

        g_fileRoot["Plugins"][nameUtf8] = YAML::Clone(defaults);
    }

    // Build the text block to append/insert into Config.yml on disk
    std::string pluginBlock;
    pluginBlock += "  " + nameUtf8 + ":\n";
    pluginBlock += EmitPluginYaml(defaults, 4);

    // Read, patch, and write back the config file
    std::string fileContent = ReadConfigFileUtf8();
    if (fileContent.empty())
    {
        // No config file exists or is empty — create one with just the Plugins section
        fileContent = "Plugins:\n" + pluginBlock;
        WriteConfigFileUtf8(fileContent);
        LogPluginEvent(L"PLUGIN CONFIG", Utf8ToWide("Created config with defaults for " + nameUtf8).c_str());
        return TRUE;
    }

    // Ensure file ends with a newline for clean appending
    if (!fileContent.empty() && fileContent.back() != '\n')
        fileContent += '\n';

    // Find the Plugins: line and determine what form it takes
    // Look for "Plugins:" at the start of a line (possibly with trailing whitespace or {})
    size_t pluginsLineStart = std::string::npos;
    bool   isEmptyMap = false;

    size_t searchPos = 0;
    while (searchPos < fileContent.size())
    {
        size_t pos = fileContent.find("Plugins:", searchPos);
        if (pos == std::string::npos)
            break;

        // Must be at start of line (pos==0 or preceded by \n)
        if (pos == 0 || fileContent[pos - 1] == '\n')
        {
            pluginsLineStart = pos;

            // Check if this is "Plugins: {}" or "Plugins:{}"
            size_t afterColon = pos + 8; // length of "Plugins:"
            size_t lineEnd = fileContent.find('\n', afterColon);
            if (lineEnd == std::string::npos) lineEnd = fileContent.size();
            std::string rest = fileContent.substr(afterColon, lineEnd - afterColon);

            // Trim whitespace
            size_t first = rest.find_first_not_of(" \t\r");
            if (first != std::string::npos)
            {
                std::string trimmed = rest.substr(first);
                // Remove trailing whitespace
                size_t last = trimmed.find_last_not_of(" \t\r");
                if (last != std::string::npos)
                    trimmed.resize(last + 1);
                isEmptyMap = (trimmed == "{}" || trimmed == "{ }");
            }
            else
            {
                isEmptyMap = false; // "Plugins:" with nothing after = map with sub-keys below
            }
            break;
        }
        searchPos = pos + 1;
    }

    if (pluginsLineStart != std::string::npos && isEmptyMap)
    {
        // Replace "Plugins: {}" line with "Plugins:\n" + plugin block
        size_t lineEnd = fileContent.find('\n', pluginsLineStart);
        if (lineEnd == std::string::npos) lineEnd = fileContent.size();
        else lineEnd += 1; // include the \n

        fileContent.replace(pluginsLineStart, lineEnd - pluginsLineStart,
            "Plugins:\n" + pluginBlock);
    }
    else if (pluginsLineStart != std::string::npos)
    {
        // "Plugins:" exists with content — find the end of its block
        // (next line with indent 0 that isn't blank/comment, or EOF)
        size_t lineEnd = fileContent.find('\n', pluginsLineStart);
        if (lineEnd == std::string::npos) lineEnd = fileContent.size();
        else lineEnd += 1;

        size_t insertPos = lineEnd;
        while (insertPos < fileContent.size())
        {
            size_t nextLineEnd = fileContent.find('\n', insertPos);
            if (nextLineEnd == std::string::npos) nextLineEnd = fileContent.size();

            std::string line = fileContent.substr(insertPos, nextLineEnd - insertPos);

            // Skip blank lines and comment lines within the block
            size_t firstNonSpace = line.find_first_not_of(" \t\r");
            if (firstNonSpace == std::string::npos || line[firstNonSpace] == '#')
            {
                insertPos = nextLineEnd + 1;
                continue;
            }

            // If the line has indent >= 1 space, it belongs to the Plugins block
            if (firstNonSpace >= 1)
            {
                insertPos = nextLineEnd + 1;
                continue;
            }

            // Line at indent 0 = new top-level key; stop here
            break;
        }

        fileContent.insert(insertPos, pluginBlock);
    }
    else
    {
        // No Plugins: key found — append to end
        fileContent += "\nPlugins:\n" + pluginBlock;
    }

    if (!WriteConfigFileUtf8(fileContent))
    {
        LogPluginEvent(L"PLUGIN CONFIG", Utf8ToWide("Failed to write defaults for " + nameUtf8).c_str());
        return FALSE;
    }

    LogPluginEvent(L"PLUGIN CONFIG", Utf8ToWide("Registered defaults for " + nameUtf8).c_str());
    return TRUE;
}

// ---------------------------------------------------------------------------
// InterposerSetConfigString
// ---------------------------------------------------------------------------
//
// Config.yml is hand-edited and comment-heavy, so a write patches the one line
// it changes rather than re-emitting the document through yaml-cpp, which would
// drop every comment in the file. The patcher understands block-style maps and
// single-line scalars, which is what the sample config and
// InterposerRegisterPluginConfig produce. Anything else -- a flow map, a block
// scalar, a quoted key -- it declines, and the Plugins section alone is
// re-emitted from memory instead. Whichever text results is parsed back before
// it is written, so a patch that misread the file never reaches disk.

// One physical line of Config.yml, as the patcher sees it.
struct ConfigLine
{
    size_t start;   // first byte of the line
    size_t end;     // one past the last byte, excluding \r\n
    size_t next;    // first byte of the following line
    int    indent;  // column of the first character; -1 for a blank or comment-only line
};

static std::vector<ConfigLine> SplitConfigLines(const std::string& text)
{
    std::vector<ConfigLine> lines;
    size_t pos = 0;

    while (pos < text.size())
    {
        size_t nl   = text.find('\n', pos);
        size_t end  = (nl == std::string::npos) ? text.size() : nl;
        size_t next = (nl == std::string::npos) ? text.size() : nl + 1;

        if (end > pos && text[end - 1] == '\r')
            --end;

        size_t first  = text.find_first_not_of(" \t", pos);
        int    indent = -1;

        if (first != std::string::npos && first < end && text[first] != '#')
            indent = static_cast<int>(first - pos);

        lines.push_back({ pos, end, next, indent });
        pos = next;
    }

    return lines;
}

// If `line` declares the plain map key `key`, return the offset just past its
// colon; otherwise npos.
static size_t MatchConfigKey(const std::string& text, const ConfigLine& line, const std::string& key)
{
    size_t p = line.start + line.indent;

    if (line.end - p < key.size() || text.compare(p, key.size(), key) != 0)
        return std::string::npos;

    p += key.size();
    while (p < line.end && text[p] == ' ') ++p;

    if (p >= line.end || text[p] != ':')
        return std::string::npos;

    ++p;
    if (p < line.end && text[p] != ' ' && text[p] != '\t')
        return std::string::npos;

    return p;
}

// Locate the inline scalar that follows a key's colon, excluding any trailing
// comment. Returns false when there is no inline value, or when it is something
// the patcher does not rewrite (block scalar, flow collection, anchor, alias,
// tag, or a quoted string that continues onto the next line).
static bool FindScalarSpan(const std::string& text, size_t from, size_t lineEnd,
                           size_t& valStart, size_t& valEnd)
{
    size_t p = text.find_first_not_of(" \t", from);
    if (p == std::string::npos || p >= lineEnd || text[p] == '#')
        return false;

    char c = text[p];
    if (c == '|' || c == '>' || c == '{' || c == '[' || c == '&' || c == '*' || c == '!')
        return false;

    valStart = p;

    if (c == '\'' || c == '"')
    {
        size_t q = p + 1;

        for (; q < lineEnd; ++q)
        {
            if (c == '"' && text[q] == '\\') { ++q; continue; }

            if (text[q] == c)
            {
                // '' is an escaped quote inside a single-quoted scalar
                if (c == '\'' && q + 1 < lineEnd && text[q + 1] == '\'') { ++q; continue; }
                break;
            }
        }

        if (q >= lineEnd) return false;
        valEnd = q + 1;
        return true;
    }

    // A plain scalar runs to a " #" comment or the end of the line
    size_t q = p;
    while (q < lineEnd && !(text[q] == '#' && (text[q - 1] == ' ' || text[q - 1] == '\t')))
        ++q;
    while (q > p && (text[q - 1] == ' ' || text[q - 1] == '\t'))
        --q;

    valEnd = q;
    return true;
}

// True if any content line between line `index` and the end of its block is
// indented deeper than `indent` -- i.e. the key on line `index` opens a nested
// block rather than holding a null value.
static bool HasChildBlock(const std::vector<ConfigLine>& lines, size_t index, int indent)
{
    for (size_t l = index + 1; l < lines.size(); ++l)
    {
        if (lines[l].indent < 0) continue;
        return lines[l].indent > indent;
    }

    return false;
}

// True if the key on `line` has an inline value after its colon (as opposed to
// nothing, or only a comment).
static bool HasInlineValue(const std::string& text, const ConfigLine& line, size_t afterColon)
{
    size_t p = text.find_first_not_of(" \t", afterColon);
    return p != std::string::npos && p < line.end && text[p] != '#';
}

// Scan the block that starts at line `first`, opened by a key at `parentIndent`
// (-1 for the document itself), for a line declaring `key` at the block's own
// indentation. Returns its line index or npos. Either way `childIndent` is the
// block's indentation (-1 if it has no content yet) and `tail` is the offset
// just past the last content line scanned -- where a new sibling would go.
static size_t FindKeyInBlock(const std::string& text, const std::vector<ConfigLine>& lines,
                             size_t first, int parentIndent, const std::string& key,
                             int& childIndent, size_t& tail)
{
    childIndent = -1;
    tail        = (first < lines.size()) ? lines[first].start : text.size();

    for (size_t l = first; l < lines.size(); ++l)
    {
        const ConfigLine& line = lines[l];

        if (line.indent < 0)             continue;
        if (line.indent <= parentIndent) break;   // end of the block
        if (childIndent < 0)             childIndent = line.indent;

        tail = line.next;

        if (line.indent == childIndent && MatchConfigKey(text, line, key) != std::string::npos)
            return l;
    }

    return std::string::npos;
}

// Convert the \n line breaks in `s` to `eol`.
static std::string WithLineEndings(std::string s, const std::string& eol)
{
    if (eol == "\n") return s;

    for (size_t p = 0; (p = s.find('\n', p)) != std::string::npos; p += eol.size())
        s.replace(p, 1, eol);

    return s;
}

// Patch `scalar` into `text` at `segs`, preserving everything else byte for byte.
// On success, `insertedFrom` is the index of the first segment that had to be
// created (segs.size() if the key already existed and only its value changed).
// Returns false when the text takes a form this patcher does not rewrite.
static bool PatchConfigScalar(std::string& text, const std::vector<std::string>& segs,
                              const std::string& scalar, size_t& insertedFrom)
{
    std::vector<ConfigLine> lines = SplitConfigLines(text);
    std::string eol = (text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";

    size_t first        = 0;   // first line of the current block
    int    parentIndent = -1;  // indent of the key that opened it; -1 for the document

    for (size_t i = 0; i < segs.size(); ++i)
    {
        int    childIndent = -1;
        size_t tail        = 0;
        size_t found       = FindKeyInBlock(text, lines, first, parentIndent, segs[i], childIndent, tail);

        if (found == std::string::npos)
        {
            // Create the rest of the path at the end of the block, matching the
            // indentation its siblings already use.
            int indent = (childIndent >= 0) ? childIndent
                       : (parentIndent < 0) ? 0
                       : parentIndent + 2;

            std::string block;
            if (tail > 0 && text[tail - 1] != '\n')
                block += eol;

            for (size_t j = i; j < segs.size(); ++j, indent += 2)
            {
                block += std::string(indent, ' ') + segs[j] + ":";
                if (j + 1 == segs.size()) block += " " + scalar;
                block += eol;
            }

            text.insert(tail, block);
            insertedFrom = i;
            return true;
        }

        const ConfigLine& line = lines[found];
        size_t afterColon = MatchConfigKey(text, line, segs[i]);

        if (i + 1 == segs.size())
        {
            size_t vs = 0, ve = 0;

            if (FindScalarSpan(text, afterColon, line.end, vs, ve))
                text.replace(vs, ve - vs, scalar);
            else if (!HasInlineValue(text, line, afterColon))
            {
                // `Key:` with nothing after it is a null value -- unless it opens a block
                if (HasChildBlock(lines, found, line.indent)) return false;
                text.insert(afterColon, " " + scalar);
            }
            else
                return false;

            insertedFrom = segs.size();
            return true;
        }

        // An intermediate key has to open an indented block. An inline value --
        // `Plugins: {}`, a flow map, a scalar -- is left to the fallback.
        if (HasInlineValue(text, line, afterColon))
            return false;

        parentIndent = line.indent;
        first        = found + 1;
    }

    return false;
}

// Fallback for PatchConfigScalar: replace the block for the first `depth`
// segments of `segs` with a fresh rendering of the file's in-memory tree,
// g_fileRoot -- never the merged one, which carries the overlay. Comments
// inside that block are lost; everything outside it is untouched. A missing
// top-level key (depth 1) is appended. Returns false if the block cannot be found.
static bool ReplaceConfigBlock(std::string& text, const std::vector<std::string>& segs, size_t depth)
{
    std::vector<ConfigLine> lines = SplitConfigLines(text);
    std::string eol = (text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";

    YAML::Node node = g_fileRoot;
    if (!WalkConfigPath(node, segs, depth))
        return false;

    size_t first        = 0;
    int    parentIndent = -1;
    size_t found        = std::string::npos;

    for (size_t i = 0; i < depth; ++i)
    {
        int    childIndent = -1;
        size_t tail        = 0;
        found = FindKeyInBlock(text, lines, first, parentIndent, segs[i], childIndent, tail);

        if (found == std::string::npos)
        {
            if (depth != 1) return false;

            if (!text.empty() && text.back() != '\n') text += eol;
            text += eol + WithLineEndings(segs[0] + ":\n" + EmitPluginYaml(node, 2), eol);
            return true;
        }

        if (i + 1 < depth)
        {
            if (HasInlineValue(text, lines[found], MatchConfigKey(text, lines[found], segs[i])))
                return false;

            parentIndent = lines[found].indent;
            first        = found + 1;
        }
    }

    // Re-render at the indentation the key already has
    int indent = lines[found].indent;
    std::string block = WithLineEndings(
        std::string(indent, ' ') + segs[depth - 1] + ":\n" + EmitPluginYaml(node, indent + 2), eol);

    // The block ends after its last deeper-indented content line, so comment
    // lines that introduce the next key stay where they are.
    size_t blockEnd = lines[found].next;

    for (size_t k = found + 1; k < lines.size(); ++k)
    {
        if (lines[k].indent < 0)       continue;
        if (lines[k].indent <= indent) break;
        blockEnd = lines[k].next;
    }

    // Keep the absence of a final line break if the block ran to EOF without one
    if (blockEnd == text.size() && !text.empty() && text.back() != '\n')
        block.resize(block.size() - eol.size());

    text.replace(lines[found].start, blockEnd - lines[found].start, block);
    return true;
}

// Parse `text` and look `count` segments of `segs` up in it.
static bool LookupConfigText(const std::string& text, const std::vector<std::string>& segs,
                             size_t count, YAML::Node& out)
{
    try
    {
        out = YAML::Load(text);
        return WalkConfigPath(out, segs, count);
    }
    catch (...)
    {
        return false;
    }
}

// Render a string as a YAML scalar that reads back as exactly that string.
// Plain style when it round-trips (so `Enabled: true` stays `true`); otherwise
// single-quoted like the rest of Config.yml -- e.g. an empty string, `null`, or
// a leading `#`. The emitter itself falls back to double quotes for a value
// single quotes cannot carry, such as one containing a line break.
static std::string EmitConfigScalar(const std::string& value)
{
    {
        YAML::Emitter em;
        em << value;
        std::string plain = em.c_str();

        try
        {
            YAML::Node back = YAML::Load(plain);
            if (!plain.empty() && plain[0] != '"' && plain[0] != '\'' &&
                back.IsScalar() && back.as<std::string>() == value)
                return plain;
        }
        catch (...) {}
    }

    YAML::Emitter em;
    em << YAML::SingleQuoted << value;
    return em.c_str();
}

// Set the scalar at `segs` inside `root`. Intermediate keys are created as maps
// where missing; one that already holds a scalar or sequence is a type conflict,
// and so is a final key that holds a map or sequence -- overwriting either would
// silently discard settings the plugin did not name. Returns false on a conflict.
static bool SetConfigScalarInTree(YAML::Node& root, const std::vector<std::string>& segs,
                                  const std::string& value)
{
    if (!root.IsMap())
        root = YAML::Node(YAML::NodeType::Map);

    YAML::Node node = root;

    for (size_t i = 0; i < segs.size(); ++i)
    {
        const YAML::Node& parent = node;
        YAML::Node existing = parent[segs[i]];
        bool last = (i + 1 == segs.size());

        if (existing && !existing.IsNull() &&
            (last ? !existing.IsScalar() : !existing.IsMap()))
            return false;

        if (last)
        {
            node[segs[i]] = value;
            break;
        }

        if (!existing || existing.IsNull())
            node[segs[i]] = YAML::Node(YAML::NodeType::Map);

        node.reset(static_cast<const YAML::Node&>(node)[segs[i]]);
    }

    return true;
}

extern "C" INTERPOSER_API
BOOL InterposerSetConfigString(const wchar_t* dotPath, const wchar_t* value)
{
    FORWARD_TO_PRIMARY(InterposerSetConfigString, dotPath, value);

    if (!dotPath || !value) return FALSE;

    std::vector<std::string> segs = SplitDotPath(dotPath);
    std::wstring             wpath(dotPath);

    // Plugins write only inside a Plugins.<Name> section. Every other key is
    // parsed once by LoadConfig into the g_* globals, so writing one would change
    // the file without changing anything the running session does.
    if (segs.size() < 3 || segs[0] != "Plugins")
    {
        LogPluginEvent(L"PLUGIN CONFIG", (L"Refused write outside Plugins.<Name>: " + wpath).c_str());
        return FALSE;
    }

    std::string valueUtf8 = WideToUtf8(std::wstring(value));

    std::unique_lock lk(g_configMutex);

    // ── In memory ──────────────────────────────────────────────────────────
    // The session's merged tree first, then the file's own tree, which is what a
    // fallback re-render writes out. A conflict in either stops the write.
    if (!SetConfigScalarInTree(g_configRoot, segs, valueUtf8) ||
        !SetConfigScalarInTree(g_fileRoot, segs, valueUtf8))
    {
        LogPluginEvent(L"PLUGIN CONFIG", (L"Type conflict, not written: " + wpath).c_str());
        return FALSE;
    }

    // ── On disk ────────────────────────────────────────────────────────────
    std::string original = ReadConfigFileUtf8();
    std::string scalar   = EmitConfigScalar(valueUtf8);

    auto verified = [&](const std::string& text) -> bool
    {
        YAML::Node node;
        return LookupConfigText(text, segs, segs.size(), node)
            && node.IsScalar() && node.as<std::string>() == valueUtf8;
    };

    std::string patched      = original;
    size_t      insertedFrom = 0;
    bool        ok           = PatchConfigScalar(patched, segs, scalar, insertedFrom);

    // A patch that created a key the parser says was already there means the
    // patcher failed to recognise it (a quoted key, say) and would have
    // written a duplicate.
    if (ok && insertedFrom < segs.size())
    {
        YAML::Node node;
        if (LookupConfigText(original, segs, insertedFrom + 1, node))
            ok = false;
    }

    // Fall back to re-rendering as little as possible: this plugin's section
    // first, so one flow-styled section cannot cost every other plugin its
    // comments, and the whole Plugins block only if that fails too.
    for (size_t depth = 2; !(ok && verified(patched)); --depth)
    {
        if (depth == 0)
        {
            LogPluginEvent(L"PLUGIN CONFIG", (L"Could not update Config.yml for " + wpath).c_str());
            return FALSE;
        }

        patched = original;
        ok      = ReplaceConfigBlock(patched, segs, depth);
    }

    if (!WriteConfigFileUtf8(patched))
    {
        LogPluginEvent(L"PLUGIN CONFIG", (L"Failed to write Config.yml for " + wpath).c_str());
        return FALSE;
    }

    LogPluginEvent(L"PLUGIN CONFIG", (L"Set " + wpath).c_str());
    return TRUE;
}

// ---------------------------------------------------------------------------
// LoadConfig
// ---------------------------------------------------------------------------
// Overlay the keys present in a NetworkRelay map onto settings. Shared by
// LoadConfig, the launcher's override mapping and InterposerRelayConnect, so a
// key means the same thing wherever it comes from. Presence is what counts: a
// key that is absent keeps whatever value the settings already had.
static void ApplyRelayNode(const YAML::Node& node, RelaySettings& s, std::vector<std::wstring>& warnings)
{
    auto readBool = [&](const char* key, bool& out)
    {
        if (!node[key])
            return;
        try { out = node[key].as<bool>(); }
        catch (const YAML::Exception&)
        {
            warnings.push_back(L"NetworkRelay." + Utf8ToWide(key) + L" is not true or false");
        }
    };

    auto readInt = [&](const char* key, int& out)
    {
        if (!node[key])
            return;
        try { out = node[key].as<int>(); }
        catch (const YAML::Exception&)
        {
            warnings.push_back(L"NetworkRelay." + Utf8ToWide(key) + L" is not a number");
        }
    };

    auto readString = [&](const char* key, std::wstring& out)
    {
        if (node[key])
            out = Utf8ToWide(node[key].as<std::string>(""));
    };

    readBool("Enabled", s.enabled);
    readString("Server", s.server);
    readInt("Port", s.port);
    readString("Network", s.network);
    readString("Ticket", s.ticket);
    readString("Address", s.address);
    readBool("UDP", s.udp);
    readBool("TCP", s.tcp);
    readBool("IPX", s.ipx);
    readBool("LoopbackBroadcasts", s.loopbackBroadcast);
    readInt("AddressTimeout", s.addressTimeout);

    if (s.port <= 0 || s.port > 65535)
    {
        warnings.push_back(L"NetworkRelay.Port out of range; using 41300");
        s.port = 41300;
    }
}

bool ParseRelaySettings(const std::string& yamlUtf8, RelaySettings& settings, std::wstring* error)
{
    YAML::Node node;

    try { node = YAML::Load(yamlUtf8); }
    catch (const YAML::Exception& e)
    {
        if (error) *error = Utf8ToWide(e.what());
        return false;
    }

    if (node.IsNull())
        return true;

    if (!node.IsMap())
    {
        if (error) *error = L"expected a map of NetworkRelay keys";
        return false;
    }

    RelaySettings next = settings;
    std::vector<std::wstring> warnings;
    ApplyRelayNode(node, next, warnings);

    for (const std::wstring& warning : warnings)
        InterposerLog(L"RELAY", warning.c_str());

    settings = next;
    return true;
}

const std::wstring& GetInterposerBaseDirectory()
{
    return g_baseDirectory;
}

std::wstring GetInterposerDataDirectory()
{
    return g_baseDirectory + L".interposer" + platform::kPathSeparator;
}

// Merge `overlay` over `base` and return the result; neither input is changed.
// Maps merge key by key, recursively. A null in the overlay removes the key, so a
// launcher can take back something Config.yml armed by its mere presence
// (AudioDevices.Input, say). Anything else -- a scalar, a sequence -- replaces
// the base value outright: a FileRedirects list from the launcher is the whole
// list, not an addition to the file's.
//
// Lookups go through a const reference: the non-const operator[] inserts.
static YAML::Node MergeConfigNodes(const YAML::Node& base, const YAML::Node& overlay)
{
    if (!base.IsMap() || !overlay.IsMap())
        return YAML::Clone(overlay);

    YAML::Node merged = YAML::Clone(base);

    for (auto it = overlay.begin(); it != overlay.end(); ++it)
    {
        const std::string key = it->first.as<std::string>("");

        if (key.empty())
            continue;

        if (it->second.IsNull())
        {
            merged.remove(key);
            continue;
        }

        const YAML::Node& mergedConst = merged;
        YAML::Node existing = mergedConst[key];

        merged[key] = existing ? MergeConfigNodes(existing, it->second) : YAML::Clone(it->second);
    }

    return merged;
}

void LoadConfig()
{
    // Locate the DLL's own directory
    const std::wstring dllDirectory = platform::ModuleDirectory();

    // Warnings are deferred, like OsVersion's below: the session log does not
    // exist until the end of this function.
    std::vector<std::wstring> configWarnings;

    // The launcher may name a directory to stand in for the DLL's own, so one
    // copy of the DLL injected into many games gives each its own .interposer
    // tree. Everything below that would have been relative to the DLL is
    // relative to this instead.
    std::wstring baseDirectory = dllDirectory;

    if (std::string requested = platform::ReadLauncherValue(L"InterposerBaseDirectory"); !requested.empty())
    {
        std::wstring wide = Utf8ToWide(requested);
        std::wstring fullPath;

        if (platform::GetFullPath(wide, fullPath))
        {
            baseDirectory = fullPath;

            if (baseDirectory.back() != L'\\' && baseDirectory.back() != L'/')
                baseDirectory += platform::kPathSeparator;

            configWarnings.push_back(L"base directory from the launcher: " + baseDirectory);
        }
        else
        {
            configWarnings.push_back(L"ignoring the launcher's base directory, which is not a usable path: " + wide);
        }
    }

    std::wstring interposerDir = baseDirectory + L".interposer" + platform::kPathSeparator;
    std::wstring yamlPath      = interposerDir + L"Config.yml";

    g_configFilePath = yamlPath;
    g_baseDirectory  = baseDirectory;

    // Backing values for %INTERPOSERDIR% and %GAMEDIR%. Captured here, before
    // any rule is parsed, and stored without a trailing separator so the config
    // author's own backslash is the only one in the result.
    g_interposerDirectory = baseDirectory;
    platform::TrimTrailingSeparators(g_interposerDirectory);

    if (std::wstring exeDirectory = platform::ExecutableDirectory(); !exeDirectory.empty())
        g_gameDirectory = exeDirectory;

    platform::ResolveKnownFolders();

    // Config.yml, then the launcher's overlay over it. Either may be missing; a
    // launcher-injected copy of the DLL usually has no Config.yml at all.
    YAML::Node fileRoot;
    bool       haveFile = false;

    if (std::string fileText = ReadConfigFileUtf8(); !fileText.empty())
    {
        try
        {
            fileRoot = YAML::Load(fileText);
            haveFile = true;
        }
        catch (const YAML::Exception& e)
        {
            configWarnings.push_back(L"Config.yml could not be parsed and was ignored: " + Utf8ToWide(e.what()));
        }
    }

    YAML::Node overlay;
    bool       haveOverlay = false;

    if (std::string overlayText = platform::ReadLauncherValue(L"InterposerConfig"); !overlayText.empty())
    {
        try
        {
            overlay = YAML::Load(overlayText);

            if (overlay.IsMap())
                haveOverlay = true;
            else if (!overlay.IsNull())
                configWarnings.push_back(L"ignoring the launcher's config overlay, which is not a map");
        }
        catch (const YAML::Exception& e)
        {
            configWarnings.push_back(L"ignoring the launcher's config overlay, which could not be parsed: " + Utf8ToWide(e.what()));
        }
    }

    // Nothing to run on: as before there was an overlay, a DLL with no usable
    // Config.yml opens no log -- unless the launcher is involved, in which case
    // whatever it asked for, and anything that went wrong with it, gets a log.
    if (!haveFile && !haveOverlay && baseDirectory == dllDirectory)
        return;

    if (!fileRoot.IsMap())
        fileRoot = YAML::Node(YAML::NodeType::Map);

    YAML::Node root = haveOverlay ? MergeConfigNodes(fileRoot, overlay) : YAML::Clone(fileRoot);

    if (haveOverlay)
        configWarnings.push_back(L"applied the launcher's config overlay over " +
            (haveFile ? yamlPath : std::wstring(L"an empty config (no Config.yml)")));

    {
        std::unique_lock lk(g_configMutex);
        g_configRoot = root;
        g_fileRoot   = fileRoot;
    }

    // ── settings ─────────────────────────────────────────────────────────────
    if (YAML::Node logging = root["Logging"])
    {
        if (logging["Files"])
            g_logFiles = logging["Files"].as<bool>(false);
        if (logging["Registry"])
            g_logRegistry = logging["Registry"].as<bool>(false);
        if (logging["Downloads"])
            g_logFastDL = logging["Downloads"].as<bool>(true);
        if (logging["Plugins"])
            g_logPlugins = logging["Plugins"].as<bool>(false);
        if (logging["Identity"])
            g_logIdentity = logging["Identity"].as<bool>(false);
        if (logging["RichPresence"])
            g_logRichPresence = logging["RichPresence"].as<bool>(false);
        if (logging["DnsRedirects"])
            g_logDnsRedirects = logging["DnsRedirects"].as<bool>(true);
        if (logging["Network"])
            g_logNetwork = logging["Network"].as<bool>(false);
        if (logging["DirectInput"])
            g_logDirectInput = logging["DirectInput"].as<bool>(false);
        if (logging["Audio"])
            g_logAudio = logging["Audio"].as<bool>(false);
        if (logging["OsVersion"])
            g_logOsVersion = logging["OsVersion"].as<bool>(false);
        if (logging["Relay"])
            g_logRelay = logging["Relay"].as<bool>(false);
        if (logging["Matches"])
            g_logMatches = logging["Matches"].as<bool>(false);

        // Verbosity within whichever subsystems are enabled above. Unrecognized
        // values fall back to Info so a typo never silently suppresses output.
        if (logging["Level"])
        {
            std::string level = logging["Level"].as<std::string>("");

            for (auto& c : level)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (level == "debug")
                g_logLevel = LogLevel::Debug;
            else if (level == "trace")
                g_logLevel = LogLevel::Trace;
            else
                g_logLevel = LogLevel::Info;
        }
    }

    // ── FileRedirects (legacy: "Redirects") ──────────────────────────────────
    std::vector<std::wstring> redirectWarnings;

    YAML::Node redirects = root["FileRedirects"];
    if (!redirects)
        redirects = root["Redirects"];
    if (redirects)
    {
        if (redirects.IsSequence())
        {
            for (const auto& item : redirects)
            {
                std::string pattern = item["Pattern"] ? item["Pattern"].as<std::string>("") : "";
                std::string replacement = item["Replacement"] ? item["Replacement"].as<std::string>("") : "";

                if (pattern.empty())
                    continue;

                try
                {
                    std::vector<std::wstring> unresolved;

                    FileRedirect redirect;
                    redirect.replacement   = Utf8ToWide(replacement);
                    redirect.patternSource = Utf8ToWide(pattern);

                    redirect.patternText   = ExpandPathTokens(redirect.patternSource,
                        /*escapeForRegex=*/true, &unresolved);

                    redirect.pattern       = std::wregex(redirect.patternText,
                        std::regex_constants::ECMAScript | std::regex_constants::icase);

                    for (const std::wstring& name : unresolved)
                    {
                        redirectWarnings.push_back(
                            L"unresolved %" + name + L"% in FileRedirects pattern: "
                            + redirect.patternSource);
                    }

                    g_redirects.push_back(std::move(redirect));
                }
                catch (const std::regex_error&)
                {
                    redirectWarnings.push_back(
                        L"malformed regex in FileRedirects, rule skipped: " + Utf8ToWide(pattern));
                }
            }
        }
    }

    // ── Registry ────────────────────────────────────────────────────────────────
    //
    // Files is an overlay stack, applied in the order written: a later file
    // wins wherever it names the same key and value as an earlier one, and the
    // last entry is the only one ever written back to. Anything not
    // drive-qualified or rooted is resolved against the base directory (the
    // DLL's, unless the launcher named another), so the historical
    // '.interposer\Registry.reg' stays spelled the way it always was.
    //
    // Warnings are deferred for the same reason as OsVersion's: the session log
    // does not exist yet.
    std::vector<std::wstring> registryWarnings;

    if (YAML::Node registry = root["Registry"])
    {
        if (registry["Isolated"])
            g_registryIsolated = registry["Isolated"].as<bool>(false);

        YAML::Node files = registry["Files"];

        if (files && files.IsSequence())
        {
            for (const auto& item : files)
            {
                std::wstring entry = Utf8ToWide(item.as<std::string>(""));

                if (entry.empty())
                    continue;

                std::vector<std::wstring> unresolved;
                std::wstring resolved = ExpandPathTokens(entry, /*escapeForRegex=*/false, &unresolved);

                for (const std::wstring& name : unresolved)
                {
                    registryWarnings.push_back(
                        L"unresolved %" + name + L"% in Registry.Files entry: " + entry);
                }

                if (!platform::IsAbsolutePath(resolved))
                    resolved = baseDirectory + resolved;

                g_registryFiles.push_back(std::move(resolved));
            }
        }

        // A Files list that was written but produced nothing usable would leave
        // the game with an empty store and nowhere to persist, which is worse
        // than the default. An absent Files key is not an error — it is how you
        // set Isolated without moving the file.
        if (files && g_registryFiles.empty())
            registryWarnings.push_back(
                L"Registry.Files yielded no usable paths; using the default .interposer\\Registry.reg");
    }

    // ── DnsRedirects ──────────────────────────────────────────────────────────
    if (YAML::Node dns = root["DnsRedirects"])
    {
        if (dns.IsSequence())
        {
            for (const auto& item : dns)
            {
                std::string pattern     = item["Pattern"]     ? item["Pattern"].as<std::string>("")     : "";
                std::string replacement = item["Replacement"] ? item["Replacement"].as<std::string>("") : "";

                if (pattern.empty())
                    continue;

                try
                {
                    DnsRedirect rule;
                    rule.replacement = Utf8ToWide(replacement);
                    rule.pattern     = std::wregex(Utf8ToWide(pattern),
                        std::regex_constants::ECMAScript | std::regex_constants::icase);
                    g_dnsRedirects.push_back(std::move(rule));
                }
                catch (const std::regex_error&) { /* skip malformed patterns */ }
            }
        }
    }

    // ── DirectInput ─────────────────────────────────────────────────────────────
    if (YAML::Node directInput = root["DirectInput"])
    {
        if (directInput["FixLegacyDeviceEnumeration"])
            g_diFixLegacyEnum = directInput["FixLegacyDeviceEnumeration"].as<bool>(false);

        if (YAML::Node filter = directInput["DeviceFilter"])
        {
            if (filter["Enabled"])
                g_diFilterEnabled = filter["Enabled"].as<bool>(false);

            if (YAML::Node classes = filter["Classes"]; classes && classes.IsSequence())
            {
                for (const auto& item : classes)
                {
                    std::string name = item.as<std::string>("");

                    if (name.empty())
                        continue;

                    DiDeviceClass parsed = DiDeviceClass::Unknown;

                    if (DiClassFromName(name, parsed))
                        g_diFilterClasses.push_back(parsed);
                    else
                    {
                        // Always reported: a typo here silently hides every
                        // device of the class the user meant to allow.
                        InterposerLog(L"DINPUT",
                            (L"unknown device class in DeviceFilter.Classes: " + Utf8ToWide(name)).c_str());
                    }
                }
            }

            if (YAML::Node names = filter["Names"]; names && names.IsSequence())
            {
                for (const auto& item : names)
                {
                    std::string pat = item.as<std::string>("");

                    if (pat.empty())
                        continue;

                    try
                    {
                        DiNameFilter rule;
                        rule.patternText = Utf8ToWide(pat);
                        rule.pattern.assign(rule.patternText,
                            std::regex_constants::ECMAScript | std::regex_constants::icase);
                        g_diFilterNames.push_back(std::move(rule));
                    }
                    catch (const std::regex_error&)
                    {
                        InterposerLog(L"DINPUT",
                            (L"malformed regex in DeviceFilter.Names: " + Utf8ToWide(pat)).c_str());
                    }
                }
            }
        }
    }

    // ── OsVersion ───────────────────────────────────────────────────────────────
    //
    // Version names a preset; the individual keys below then override single
    // fields of it. Presence is what counts, not the value, so `Major: 0` is an
    // explicit zero rather than "unset".
    //
    // Warnings are collected rather than logged inline: the session log is not
    // opened until the end of LoadConfig, and a misconfiguration here is exactly
    // the thing the user needs to see.
    std::vector<std::wstring> osVersionWarnings;

    if (YAML::Node osVersion = root["OsVersion"])
    {
        std::string requested = osVersion["Version"] ? osVersion["Version"].as<std::string>("") : "";
        std::string normalized = NormalizeOsToken(requested);

        // Anything that reads as "off" leaves the feature inert, and so does an
        // absent Version — overriding Major alone should not silently start
        // lying about the OS.
        const bool disabled = normalized.empty() ||
                              normalized == "none" ||
                              normalized == "off"  ||
                              normalized == "false";

        if (!disabled)
        {
            if (const OsVersionEntry* preset = OsVersionFromName(requested))
            {
                g_osVersionEnabled   = true;
                g_osVersionName      = Utf8ToWide(preset->name);
                g_osMajorVersion     = preset->major;
                g_osMinorVersion     = preset->minor;
                g_osBuildNumber      = preset->build;
                g_osServicePack      = preset->servicePack;
                g_osServicePackMajor = preset->servicePackMajor;
                g_osProductType      = preset->productType;

                if (osVersion["Major"])
                    g_osMajorVersion = osVersion["Major"].as<unsigned int>(g_osMajorVersion);
                if (osVersion["Minor"])
                    g_osMinorVersion = osVersion["Minor"].as<unsigned int>(g_osMinorVersion);
                if (osVersion["Build"])
                    g_osBuildNumber = osVersion["Build"].as<unsigned int>(g_osBuildNumber);

                // An explicit service pack string carries its own number, so
                // wServicePackMajor follows it rather than the preset's.
                if (osVersion["ServicePack"])
                {
                    g_osServicePack      = Utf8ToWide(osVersion["ServicePack"].as<std::string>(""));
                    g_osServicePackMajor = ServicePackMajorFromName(g_osServicePack);
                }

                if (osVersion["ProductType"])
                {
                    std::string productType =
                        NormalizeOsToken(osVersion["ProductType"].as<std::string>(""));

                    if (productType == "server")
                        g_osProductType = VER_NT_SERVER;
                    else if (productType == "workstation")
                        g_osProductType = VER_NT_WORKSTATION;
                    else
                        osVersionWarnings.push_back(
                            L"unknown ProductType (expected Workstation or Server): " +
                            Utf8ToWide(osVersion["ProductType"].as<std::string>("")));
                }

                // Every client SKU since XP reports VER_SUITE_SINGLEUSERTS; a
                // plain server SKU reports nothing.
                g_osSuiteMask = (g_osProductType == VER_NT_WORKSTATION)
                    ? static_cast<WORD>(VER_SUITE_SINGLEUSERTS)
                    : 0;

                // A preset with no numbers of its own (Custom) and no overrides
                // would report 0.0.0, which is worse than telling the truth.
                if (g_osMajorVersion == 0)
                {
                    g_osVersionEnabled = false;

                    osVersionWarnings.push_back(
                        L"OsVersion.Version resolved to major version 0; reporting the real OS version instead");
                }
            }
            else
            {
                // Always reported: a typo here means the game silently keeps
                // seeing the real version, which is the bug the user was fixing.
                osVersionWarnings.push_back(L"unknown OsVersion.Version: " + Utf8ToWide(requested));
            }
        }
    }

    // ── NetworkAdapters ─────────────────────────────────────────────────────────
    if (YAML::Node na = root["NetworkAdapters"])
    {
        if (na["Enabled"])
            g_naEnabled = na["Enabled"].as<bool>(false);

        if (YAML::Node subnets = na["Subnets"]; subnets && subnets.IsSequence())
        {
            for (const auto& item : subnets)
            {
                SubnetFilter f{};
                if (ParseCidr(item.as<std::string>(""), f))
                    g_naSubnets.push_back(f);
            }
        }

        if (YAML::Node names = na["Names"]; names && names.IsSequence())
        {
            for (const auto& item : names)
            {
                std::string pat = item.as<std::string>("");
                if (pat.empty())
                    continue;
                try
                {
                    g_naNames.emplace_back(Utf8ToWide(pat),
                        std::regex_constants::ECMAScript | std::regex_constants::icase);
                }
                catch (const std::regex_error&) { /* skip malformed patterns */ }
            }
        }

        if (YAML::Node macs = na["MACs"]; macs && macs.IsSequence())
        {
            for (const auto& item : macs)
            {
                std::array<BYTE, 6> m{};
                if (ParseMac(item.as<std::string>(""), m))
                    g_naMacs.push_back(m);
            }
        }

        g_naAnyFilters = !g_naSubnets.empty() || !g_naNames.empty() || !g_naMacs.empty();
    }

    // ── NetworkRelay ────────────────────────────────────────────────────────────
    //
    // Deferred warnings, like OsVersion's: the session log does not exist yet.
    std::vector<std::wstring> relayWarnings;

    if (YAML::Node relay = root["NetworkRelay"])
    {
        if (relay.IsMap())
            ApplyRelayNode(relay, g_relayConfig, relayWarnings);
        else
            relayWarnings.push_back(L"NetworkRelay is not a map; ignored");
    }

    // -- AudioDevices ----------------------------------------------------------
    //
    // One allow-list per direction, armed by the key's presence rather than by
    // its contents, so `Input: []` (hide every microphone) is expressible and
    // distinct from an absent Input key (do not filter microphones at all).
    // Entries are literal strings, not regexes -- see config.h.
    if (YAML::Node ad = root["AudioDevices"])
    {
        if (ad["Enabled"])
            g_adEnabled = ad["Enabled"].as<bool>(false);

        ParseAudioDeviceList(ad["Output"], g_adOutput, g_adFilterOutput);
        ParseAudioDeviceList(ad["Input"],  g_adInput,  g_adFilterInput);
    }

    // ── fastDL ────────────────────────────────────────────────────────────────
    // Deferred like the others: the session log does not exist yet.
    std::vector<std::wstring> fastdlWarnings;

    if (YAML::Node fastDl = root["FastDL"])
    {
        if (fastDl["Enabled"])
            g_fastdlEnabled = fastDl["Enabled"].as<bool>(false);

        if (fastDl["BaseUrl"])
            g_fastdlBaseUrl = Utf8ToWide(fastDl["BaseUrl"].as<std::string>(""));

        if (fastDl["UseDownloadDirectory"])
            g_fastdlUseDownloadDir = fastDl["UseDownloadDirectory"].as<bool>(true);
        if (fastDl["DownloadDirectory"])
            g_fastdlDownloadDir = Utf8ToWide(fastDl["DownloadDirectory"].as<std::string>(""));
        if (fastDl["BlockSensitiveFiles"])
            g_fastdlBlockSensitiveFiles = fastDl["BlockSensitiveFiles"].as<bool>(true);

        if (fastDl["ProbeConnections"])
            g_fastdlProbeConnections = fastDl["ProbeConnections"].as<bool>(false);
        if (fastDl["ProbePort"])
            g_fastdlProbePort = fastDl["ProbePort"].as<int>(80);
        if (fastDl["ProbePath"])
            g_fastdlProbePath = Utf8ToWide(fastDl["ProbePath"].as<std::string>("/"));
        if (fastDl["ProbeTimeout"])
            g_fastdlProbeTimeout = fastDl["ProbeTimeout"].as<int>(2000);
        if (YAML::Node filteredPorts = fastDl["FilteredPorts"])
        {
            if (filteredPorts.IsSequence())
            {
                g_fastdlFilteredPorts.clear();
                for (const auto& item : filteredPorts)
                {
                    PortRange range{};
                    range.min = item["Min"] ? item["Min"].as<int>(0) : 0;
                    range.max = item["Max"] ? item["Max"].as<int>(0) : 0;
                    if (range.min > 0 && range.max >= range.min)
                        g_fastdlFilteredPorts.push_back(range);
                }
            }
        }

        if (YAML::Node allowedExtensions = fastDl["AllowedExtensions"])
        {
            if (allowedExtensions.IsSequence())
            {
                for (const auto& allowedExtension : allowedExtensions)
                {
                    std::wstring extension = Utf8ToWide(allowedExtension.as<std::string>(""));

                    if (extension.empty())
                        continue;

                    for (auto& character : extension) character = towlower(character);

                    if (extension[0] != L'.')
                        extension = L'.' + extension;

                    g_fastdlAllowedExtensions.push_back(extension);
                }
            }
        }

        if (YAML::Node paths = fastDl["Paths"])
        {
            if (paths.IsSequence())
            {
                for (const auto& item : paths)
                {
                    std::wstring local  = item["Local"]  ? Utf8ToWide(item["Local"].as<std::string>(""))  : L"";
                    std::wstring remote = item["Remote"] ? Utf8ToWide(item["Remote"].as<std::string>("")) : L"";

                    if (local.empty())
                        continue;

                    // %GAMEDIR% and the rest, so a config shipped with a game patch
                    // can name the game's folders without knowing where each
                    // player installed it. A Local with no token is unchanged.
                    std::vector<std::wstring> unresolved;
                    const std::wstring written = local;

                    local = ExpandPathTokens(local, /*escapeForRegex=*/false, &unresolved);

                    for (const std::wstring& name : unresolved)
                    {
                        fastdlWarnings.push_back(
                            L"unresolved %" + name + L"% in FastDL.Paths Local: " + written);
                    }

                    FastDLPath path;

                    path.localPrefix   = std::move(local);
                    path.remoteSubPath = std::move(remote);

                    // Ensure localPrefix ends with a separator
                    if (!path.localPrefix.empty())
                    {
                        wchar_t last = path.localPrefix.back();
                        if (last != L'\\' && last != L'/')
                            path.localPrefix += platform::kPathSeparator;
                    }

                    // Strip trailing slash from remoteSubPath
                    while (!path.remoteSubPath.empty() &&
                           (path.remoteSubPath.back() == L'/' || path.remoteSubPath.back() == L'\\'))
                        path.remoteSubPath.pop_back();

                    g_fastdlPaths.push_back(std::move(path));
                }
            }
        }
    }

    // ── player ────────────────────────────────────────────────────────────────
    if (YAML::Node player = root["Player"])
    {
        if (player["Username"])
            g_username = Utf8ToWide(player["Username"].as<std::string>(""));
        if (player["ComputerName"])
            g_computername = Utf8ToWide(player["ComputerName"].as<std::string>(""));
    }

    // ── RichPresence ────────────────────────────────────────────────────────
    if (YAML::Node rp = root["RichPresence"])
    {
        if (YAML::Node discord = rp["Discord"])
        {
            if (discord["Enabled"])
                g_rpDiscordEnabled = discord["Enabled"].as<bool>(false);
            if (discord["ApplicationId"])
                g_rpDiscordAppId = discord["ApplicationId"].as<std::string>("");
        }

        // Default presence values (apply to all backends)
        if (rp["Type"])
            g_rpDefaultType = rp["Type"].as<int>(0);
        if (rp["Name"])
            g_rpDefaultName = rp["Name"].as<std::string>("");
        if (rp["Details"])
            g_rpDefaultDetails = rp["Details"].as<std::string>("");
        if (rp["DetailsUrl"])
            g_rpDefaultDetailsUrl = rp["DetailsUrl"].as<std::string>("");
        if (rp["State"])
            g_rpDefaultState = rp["State"].as<std::string>("");
        if (rp["StateUrl"])
            g_rpDefaultStateUrl = rp["StateUrl"].as<std::string>("");
        if (rp["LargeImage"])
            g_rpDefaultLargeImageKey = rp["LargeImage"].as<std::string>("");
        if (rp["LargeImageText"])
            g_rpDefaultLargeImageText = rp["LargeImageText"].as<std::string>("");
        if (rp["SmallImage"])
            g_rpDefaultSmallImageKey = rp["SmallImage"].as<std::string>("");
        if (rp["SmallImageText"])
            g_rpDefaultSmallImageText = rp["SmallImageText"].as<std::string>("");
        if (rp["Button1Text"])
            g_rpDefaultButton1Text = rp["Button1Text"].as<std::string>("");
        if (rp["Button1Url"])
            g_rpDefaultButton1Url = rp["Button1Url"].as<std::string>("");
        if (rp["Button2Text"])
            g_rpDefaultButton2Text = rp["Button2Text"].as<std::string>("");
        if (rp["Button2Url"])
            g_rpDefaultButton2Url = rp["Button2Url"].as<std::string>("");
    }

    // ── MatchStatistics ─────────────────────────────────────────────────────
    // Where InterposerMatch* reports go (matches.cpp). ServerUrl and Token are
    // %VAR%-expanded, so a token can be kept in the environment rather than
    // the file.
    if (YAML::Node ms = root["MatchStatistics"])
    {
        auto text = [&](const char* key) { return ms[key] ? ms[key].as<std::string>("") : std::string(); };

        auto isGuid = [](const std::string& s)
        {
            if (s.size() != 36)
                return false;

            for (size_t i = 0; i < s.size(); ++i)
            {
                const bool dash = i == 8 || i == 13 || i == 18 || i == 23;

                if (dash ? s[i] != '-' : !std::isxdigit(static_cast<unsigned char>(s[i])))
                    return false;
            }

            return true;
        };

        try
        {
            if (ms["Enabled"])
                g_msEnabled = ms["Enabled"].as<bool>(true);
        }
        catch (const YAML::Exception&)
        {
            configWarnings.push_back(L"MatchStatistics.Enabled is not true or false");
        }

        if (ms["ServerUrl"])
        {
            std::wstring url = ExpandEnvVars(Utf8ToWide(text("ServerUrl")));

            while (!url.empty() && (url.back() == L'/' || url.back() == L' '))
                url.pop_back();

            std::wstring lower = url.substr(0, 8);
            for (auto& c : lower)
                c = static_cast<wchar_t>(std::towlower(c));

            if (url.empty() || lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0)
                g_msServerUrl = url;
            else
                configWarnings.push_back(L"MatchStatistics.ServerUrl is not an http:// or https:// URL: " + url);
        }

        if (ms["Token"])
            g_msToken = WideToUtf8(ExpandEnvVars(Utf8ToWide(text("Token"))));

        if (ms["GameId"])
        {
            g_msGameId = text("GameId");

            if (!g_msGameId.empty() && !isGuid(g_msGameId))
            {
                configWarnings.push_back(L"MatchStatistics.GameId is not a GUID: " + Utf8ToWide(g_msGameId));
                g_msGameId.clear();
            }
        }

        if (ms["ServerId"])
        {
            g_msServerId = text("ServerId");

            if (!g_msServerId.empty() && !isGuid(g_msServerId))
            {
                configWarnings.push_back(L"MatchStatistics.ServerId is not a GUID: " + Utf8ToWide(g_msServerId));
                g_msServerId.clear();
            }
        }

        if (ms["ReporterKind"])
        {
            std::string kind = text("ReporterKind");
            std::string lower = kind;

            for (auto& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (lower == "server")      g_msReporterKind = "Server";
            else if (lower == "host")   g_msReporterKind = "Host";
            else if (lower == "client") g_msReporterKind = "Client";
            else
                configWarnings.push_back(L"MatchStatistics.ReporterKind is not Server, Host or Client: " + Utf8ToWide(kind));
        }

        if (ms["ReporterName"])
            g_msReporterName = text("ReporterName");

        try
        {
            if (ms["FlushInterval"])
            {
                const int interval = ms["FlushInterval"].as<int>();

                if (interval >= 100 && interval <= 600000)
                    g_msFlushInterval = interval;
                else
                    configWarnings.push_back(L"MatchStatistics.FlushInterval must be between 100 and 600000 ms");
            }
        }
        catch (const YAML::Exception&)
        {
            configWarnings.push_back(L"MatchStatistics.FlushInterval is not a number");
        }
    }

    // ── Open log at .interposer\Logs\<timestamp>.log ──────────────────────────
    platform::CreateDirectoryTree(interposerDir);

    std::wstring logsDir = interposerDir + L"Logs" + platform::kPathSeparator;
    platform::CreateDirectoryTree(logsDir);

    const platform::LocalTime now = platform::Now();

    wchar_t timestampBuffer[32];
    swprintf(timestampBuffer, std::size(timestampBuffer), L"%04d-%02d-%02d_%02d-%02d-%02d",
        now.year, now.month, now.day, now.hour, now.minute, now.second);

    std::wstring logPath = logsDir + timestampBuffer + L".log";

    g_logHandle = platform::OpenForAppend(logPath);

    if (g_logHandle != platform::kInvalidFile)
    {
        wchar_t sep[128];
        swprintf(sep, std::size(sep),
            L"# === Session started %04d-%02d-%02d %02d:%02d:%02d ===\r\n",
            now.year, now.month, now.day, now.hour, now.minute, now.second);

        const std::string utf8 = WideToUtf8(sep);

        if (!utf8.empty())
            platform::WriteToFile(g_logHandle, utf8.data(), utf8.size());
    }

    // Deferred from the OsVersion parse above, which ran before the log existed.
    // InterposerLog writes regardless of the logging flags, which is what a
    // misconfiguration warrants.
    for (const std::wstring& warning : configWarnings)
        InterposerLog(L"CONFIG", warning.c_str());

    for (const std::wstring& warning : osVersionWarnings)
        InterposerLog(L"OSVERSION", warning.c_str());

    for (const std::wstring& warning : redirectWarnings)
        InterposerLog(L"FILEREDIRECT", warning.c_str());

    for (const std::wstring& warning : registryWarnings)
        InterposerLog(L"REGISTRY", warning.c_str());

    for (const std::wstring& warning : relayWarnings)
        InterposerLog(L"RELAY", warning.c_str());

    for (const std::wstring& warning : fastdlWarnings)
        InterposerLog(L"FASTDL", warning.c_str());
}
