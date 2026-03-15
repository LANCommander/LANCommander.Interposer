#include "config.h"

#include <mutex>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

// ---------------------------------------------------------------------------
// Globals (definitions)
// ---------------------------------------------------------------------------
bool g_logFiles    = false;
bool g_logRegistry = false;

bool                      g_fastdlEnabled             = false;
bool                      g_logFastDL                 = true;
std::wstring              g_fastdlBaseUrl;
std::vector<std::wstring> g_fastdlAllowedExtensions;
std::vector<FastDLPath>   g_fastdlPaths;
bool                      g_fastdlUseDownloadDir      = true;
std::wstring              g_fastdlDownloadDir;
bool                      g_fastdlBlockSensitiveFiles = true;

static std::vector<FileRedirect> g_redirects;
static HANDLE                    g_logHandle = INVALID_HANDLE_VALUE;
static std::mutex                g_logMutex;

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
// ApplyFileRedirects
// ---------------------------------------------------------------------------
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
            return ExpandEnvVars(result);
    }

    return path;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
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
    line += verb;
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
    if (!g_logFiles)
        return;

    WriteLogLine(verb, sourcePath, redirectionPath);
}

void LogRegistryAccess(const wchar_t* verb, const wchar_t* keyPath, const wchar_t* valueName)
{
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

    // ── settings ─────────────────────────────────────────────────────────────
    if (YAML::Node logging = root["Logging"])
    {
        if (logging["Files"])
            g_logFiles = logging["Files"].as<bool>(false);
        if (logging["Registry"])
            g_logRegistry = logging["Registry"].as<bool>(false);
        if (logging["Downloads"])
            g_logFastDL = logging["logDownloads"].as<bool>(true);
    }

    // ── fileRedirects ─────────────────────────────────────────────────────────
    if (YAML::Node redirects = root["Redirects"])
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
