#include "config.h"
#include "callbacks.h"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

// ---------------------------------------------------------------------------
// Globals (definitions)
// ---------------------------------------------------------------------------
bool         g_logFiles         = false;
bool         g_logRegistry      = false;
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

bool         g_logPlugins       = false;
bool         g_logIdentity     = false;
bool         g_logRichPresence  = false;
bool         g_logDnsRedirects = true;
bool         g_logNetwork      = false;

std::vector<DnsRedirect> g_dnsRedirects;

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

static std::vector<FileRedirect> g_redirects;
static HANDLE                    g_logHandle = INVALID_HANDLE_VALUE;
static std::mutex                g_logMutex;
static std::wstring              g_configFilePath; // set by LoadConfig(), used by InterposerRegisterPluginConfig

// Persisted YAML root for plugin config queries (read-only after LoadConfig).
static YAML::Node        g_configRoot;
static std::shared_mutex g_configMutex;

// ---------------------------------------------------------------------------
// ExpandEnvVars
// ---------------------------------------------------------------------------
std::wstring ExpandEnvVars(const std::wstring& input)
{
    DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);

    if (needed == 0)
        return input;

    std::wstring result(needed, L'\0');

    ExpandEnvironmentStringsW(input.c_str(), result.data(), needed);

    // ExpandEnvironmentStringsW includes the null terminator in 'needed'
    if (!result.empty() && result.back() == L'\0')
        result.pop_back();

    return result;
}

// ---------------------------------------------------------------------------
// Utf8ToWide
// ---------------------------------------------------------------------------
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring out(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

// ---------------------------------------------------------------------------
// WideToUtf8
// ---------------------------------------------------------------------------
static std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
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

std::wstring ApplyFileRedirects(const std::wstring& path)
{
    if (g_redirects.empty())
        return path;

    for (const auto& redirect : g_redirects)
    {
        std::wstring result = std::regex_replace(
            path, redirect.pattern, redirect.replacement,
            std::regex_constants::format_first_only);

        if (result != path)
            return NormalizeBackslashes(ExpandEnvVars(result));
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

    if (g_logHandle == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME systemTime{};

    GetLocalTime(&systemTime);

    wchar_t timestamp[24];
    wsprintfW(timestamp, L"%04d-%02d-%02d %02d:%02d:%02d",
        systemTime.wYear, systemTime.wMonth, systemTime.wDay, systemTime.wHour, systemTime.wMinute, systemTime.wSecond);

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

    // Convert to UTF-8
    int utf8len = WideCharToMultiByte(CP_UTF8, 0,
        line.c_str(), static_cast<int>(line.size()),
        nullptr, 0, nullptr, nullptr);

    if (utf8len <= 0)
        return;

    std::string utf8(utf8len, '\0');

    WideCharToMultiByte(CP_UTF8, 0,
        line.c_str(), static_cast<int>(line.size()),
        utf8.data(), utf8len, nullptr, nullptr);

    DWORD written = 0;

    WriteFile(g_logHandle, utf8.c_str(), static_cast<DWORD>(utf8.size()), &written, nullptr);
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

void LogDnsRedirect(const wchar_t* fromHost, const wchar_t* toHost)
{
    FireDnsCallback(fromHost, toHost);

    if (!g_logDnsRedirects)
        return;

    WriteLogLine(L"DNS REDIRECT", fromHost, toHost);
}

void LogHookInit(const wchar_t* module, const char* fn, MH_STATUS status)
{
    if (status == MH_ERROR_ALREADY_CREATED)
        return; // Hook was already installed (e.g. late-install called twice); not an error.

    wchar_t msg[128]{};
    if (status == MH_OK)
        wsprintfW(msg, L"%s!%S", module, fn);
    else
        wsprintfW(msg, L"%s!%S  %S", module, fn, MH_StatusToString(status));
    WriteLogLine(L"HOOK INIT", msg, nullptr);
}

void CloseLog()
{
    std::lock_guard<std::mutex> lk(g_logMutex);

    if (g_logHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_logHandle);
        g_logHandle = INVALID_HANDLE_VALUE;
    }
}

// ---------------------------------------------------------------------------
// Plugin API exports
// ---------------------------------------------------------------------------

// Return the effective username: the configured/injected override if set, otherwise
// the real Windows account name from GetUserNameW.
// bufferSize is in wchar_t units and must include room for the null terminator.
// Returns TRUE on success, FALSE if the buffer is too small or GetUserNameW fails.
extern "C" __declspec(dllexport)
BOOL InterposerGetUsername(wchar_t* buffer, DWORD bufferSize)
{
    if (!buffer || bufferSize == 0) return FALSE;

    if (!g_username.empty())
    {
        DWORD needed = static_cast<DWORD>(g_username.size() + 1);
        if (bufferSize < needed) return FALSE;
        wmemcpy(buffer, g_username.c_str(), needed);
        return TRUE;
    }

    // No override configured — delegate to the real API (hooks not yet involved here).
    return GetUserNameW(buffer, &bufferSize);
}

// Write a line to the session log. Always writes regardless of logging flags.
// verb    — label shown in the log (e.g. L"MYPLUGIN" or L"[MYPLUGIN]"); automatically
//           stripped of existing brackets/whitespace, truncated to 16 chars, and
//           re-wrapped as [verb] right-padded to 18 characters.
// message — path or free-form text
extern "C" __declspec(dllexport)
void InterposerLog(const wchar_t* verb, const wchar_t* message)
{
    if (!verb || !message) return;
    WriteLogLine(verb, message, nullptr);
}

// Read a scalar value from Config.yml by dot-separated YAML path.
// dotPath    — e.g. L"Plugins.MyPlugin.Setting"
// buffer     — receives the null-terminated wide string value
// bufferSize — capacity of buffer in wchar_t units
// Returns TRUE on success, FALSE if the key does not exist, is not a scalar,
// or the buffer is too small.
extern "C" __declspec(dllexport)
BOOL InterposerGetConfigString(const wchar_t* dotPath, wchar_t* buffer, DWORD bufferSize)
{
    if (!dotPath || !buffer || bufferSize == 0) return FALSE;

    std::shared_lock lk(g_configMutex);

    // Traverse the node tree one segment at a time
    YAML::Node node = g_configRoot;
    std::wstring wpath(dotPath);
    size_t start = 0;

    while (start <= wpath.size())
    {
        size_t dot = wpath.find(L'.', start);
        std::wstring wseg = (dot == std::wstring::npos)
            ? wpath.substr(start)
            : wpath.substr(start, dot - start);

        if (wseg.empty()) return FALSE;

        std::string seg = WideToUtf8(wseg);
        if (!node[seg]) return FALSE;
        node = node[seg];

        if (dot == std::wstring::npos) break;
        start = dot + 1;
    }

    if (!node.IsScalar()) return FALSE;

    std::string value;
    try { value = node.as<std::string>(); }
    catch (...) { return FALSE; }

    int wlen = MultiByteToWideChar(CP_UTF8, 0,
        value.c_str(), static_cast<int>(value.size()) + 1,
        buffer, static_cast<int>(bufferSize));

    return wlen > 0 ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
// InterposerRegisterPluginConfig
// ---------------------------------------------------------------------------

// Read the entire file at g_configFilePath into a UTF-8 string.
// Returns empty string on failure.
static std::string ReadConfigFileUtf8()
{
    HANDLE fh = CreateFileW(g_configFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (fh == INVALID_HANDLE_VALUE)
        return {};

    LARGE_INTEGER sz{};
    GetFileSizeEx(fh, &sz);
    if (sz.QuadPart == 0 || sz.QuadPart > 4 * 1024 * 1024)
    {
        CloseHandle(fh);
        return {};
    }

    std::vector<BYTE> raw(static_cast<size_t>(sz.QuadPart));
    DWORD bytesRead = 0;
    ReadFile(fh, raw.data(), static_cast<DWORD>(raw.size()), &bytesRead, nullptr);
    CloseHandle(fh);

    // Decode to UTF-8 (handle BOM variants)
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
    {
        size_t charCount = (raw.size() - 2) / 2;
        const wchar_t* wptr = reinterpret_cast<const wchar_t*>(raw.data() + 2);
        int utf8len = WideCharToMultiByte(CP_UTF8, 0, wptr, static_cast<int>(charCount),
            nullptr, 0, nullptr, nullptr);
        if (utf8len <= 0) return {};
        std::string s(utf8len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wptr, static_cast<int>(charCount),
            s.data(), utf8len, nullptr, nullptr);
        return s;
    }

    int offset = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
    return std::string(reinterpret_cast<const char*>(raw.data()) + offset, raw.size() - offset);
}

// Write a UTF-8 string to g_configFilePath (overwrites existing content).
static bool WriteConfigFileUtf8(const std::string& content)
{
    HANDLE fh = CreateFileW(g_configFilePath.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (fh == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    BOOL ok = WriteFile(fh, content.c_str(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(fh);
    return ok && written == content.size();
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
        else if (val.IsMap())
        {
            result += pad + key + ":\n";
            result += EmitPluginYaml(val, indent + 2);
        }
    }

    return result;
}

extern "C" __declspec(dllexport)
BOOL InterposerRegisterPluginConfig(const wchar_t* pluginName, const wchar_t* yamlDefaults)
{
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
// LoadConfig
// ---------------------------------------------------------------------------
void LoadConfig()
{
    // Locate the DLL's own directory
    HMODULE hSelf = nullptr;

    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadConfig),
        &hSelf);

    wchar_t dllPathBuffer[MAX_PATH] = {};

    GetModuleFileNameW(hSelf, dllPathBuffer, MAX_PATH);
    std::wstring dllDirectory(dllPathBuffer);
    auto slash = dllDirectory.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        dllDirectory.resize(slash + 1);

    std::wstring interposerDir = dllDirectory + L".interposer\\";
    std::wstring yamlPath      = interposerDir + L"Config.yml";

    g_configFilePath = yamlPath;

    HANDLE fileHandle = CreateFileW(yamlPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (fileHandle == INVALID_HANDLE_VALUE)
        return;

    LARGE_INTEGER fileSize{};
    GetFileSizeEx(fileHandle, &fileSize);
    if (fileSize.QuadPart == 0 || fileSize.QuadPart > 4 * 1024 * 1024)
    {
        CloseHandle(fileHandle);
        return;
    }

    std::vector<BYTE> raw(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;

    ReadFile(fileHandle, raw.data(), static_cast<DWORD>(raw.size()), &bytesRead, nullptr);
    CloseHandle(fileHandle);

    // Decode to UTF-8 std::string (supports UTF-16 LE BOM, UTF-8 BOM, or plain UTF-8)
    std::string utf8str;

    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
    {
        // UTF-16 LE BOM — convert to UTF-8
        size_t characterCount = (raw.size() - 2) / 2;
        const wchar_t* wptr = reinterpret_cast<const wchar_t*>(raw.data() + 2);
        int utf8len = WideCharToMultiByte(CP_UTF8, 0, wptr, static_cast<int>(characterCount),
            nullptr, 0, nullptr, nullptr);
        if (utf8len > 0)
        {
            utf8str.resize(utf8len);
            WideCharToMultiByte(CP_UTF8, 0, wptr, static_cast<int>(characterCount),
                utf8str.data(), utf8len, nullptr, nullptr);
        }
    }
    else
    {
        // UTF-8 BOM — skip 3 bytes; otherwise use raw bytes as-is
        int offset = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        utf8str.assign(reinterpret_cast<const char*>(raw.data()) + offset, raw.size() - offset);
    }

    YAML::Node root;
    try { root = YAML::Load(utf8str); }
    catch (...) { return; }

    {
        std::unique_lock lk(g_configMutex);
        g_configRoot = root;
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
    }

    // ── FileRedirects (legacy: "Redirects") ──────────────────────────────────
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
                    FileRedirect redirect;
                    redirect.replacement = Utf8ToWide(replacement);
                    redirect.pattern     = std::wregex(Utf8ToWide(pattern),
                        std::regex_constants::ECMAScript | std::regex_constants::icase);
                    g_redirects.push_back(std::move(redirect));
                }
                catch (const std::regex_error&) { /* skip malformed patterns */ }
            }
        }
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

    // ── fastDL ────────────────────────────────────────────────────────────────
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

                    FastDLPath path;
                    
                    path.localPrefix   = std::move(local);
                    path.remoteSubPath = std::move(remote);

                    // Ensure localPrefix ends with backslash
                    if (!path.localPrefix.empty())
                    {
                        wchar_t last = path.localPrefix.back();
                        if (last != L'\\' && last != L'/')
                            path.localPrefix += L'\\';
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

    // ── Open log at .interposer\Logs\<timestamp>.log ──────────────────────────
    CreateDirectoryW(interposerDir.c_str(), nullptr);

    std::wstring logsDir = interposerDir + L"Logs\\";
    CreateDirectoryW(logsDir.c_str(), nullptr);

    SYSTEMTIME systemTime{};
    GetLocalTime(&systemTime);

    wchar_t timestampBuffer[32];
    wsprintfW(timestampBuffer, L"%04d-%02d-%02d_%02d-%02d-%02d",
        systemTime.wYear, systemTime.wMonth, systemTime.wDay,
        systemTime.wHour, systemTime.wMinute, systemTime.wSecond);

    std::wstring logPath = logsDir + timestampBuffer + L".log";

    g_logHandle = CreateFileW(logPath.c_str(),
        FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (g_logHandle != INVALID_HANDLE_VALUE)
    {
        wchar_t sep[128];
        wsprintfW(sep,
            L"# === Session started %04d-%02d-%02d %02d:%02d:%02d ===\r\n",
            systemTime.wYear, systemTime.wMonth, systemTime.wDay,
            systemTime.wHour, systemTime.wMinute, systemTime.wSecond);

        int length = WideCharToMultiByte(CP_UTF8, 0, sep, -1,
            nullptr, 0, nullptr, nullptr);

        if (length > 1)
        {
            std::string utf8(length - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, sep, -1,
                utf8.data(), length - 1, nullptr, nullptr);

            DWORD w = 0;

            WriteFile(g_logHandle, utf8.c_str(),
                static_cast<DWORD>(utf8.size()), &w, nullptr);
        }
    }
}
