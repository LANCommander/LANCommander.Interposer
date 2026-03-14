#include "config.h"

#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Globals (definitions)
// ---------------------------------------------------------------------------
bool g_logFiles    = false;
bool g_logRegistry = false;

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
// INI parsing helpers
// ---------------------------------------------------------------------------
static std::wstring Trim(const std::wstring& input)
{
    size_t start = input.find_first_not_of(L" \t\r\n");
    
    if (start == std::wstring::npos)
        return {};
    
    size_t end = input.find_last_not_of(L" \t\r\n");
    
    return input.substr(start, end - start + 1);
}

static bool ParseBool(const std::wstring& input)
{
    return input == L"1" || input == L"true" || input == L"yes" || input == L"on";
}

static std::wstring ToLowerW(std::wstring input)
{
    for (auto& character : input) character = towlower(character);
    return input;
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

    std::wstring iniPath = dllDirectory + L"interposer.ini";

    HANDLE fileHandle = CreateFileW(iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
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

    // Decode to wstring (supports UTF-16 LE BOM, UTF-8 BOM, or plain UTF-8/ANSI)
    std::wstring content;
    
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
    {
        size_t characterCount = (raw.size() - 2) / 2;
        content.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2), characterCount);
    }
    else
    {
        int offset = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        
        const char* source = reinterpret_cast<const char*>(raw.data()) + offset;
        
        int sourceLength = static_cast<int>(raw.size()) - offset;
        int wideLength = MultiByteToWideChar(CP_UTF8, 0, source, sourceLength, nullptr, 0);
        if (wideLength > 0)
        {
            content.resize(wideLength);
            MultiByteToWideChar(CP_UTF8, 0, source, sourceLength, content.data(), wideLength);
        }
    }

    // Parse line by line
    std::wstring currentSection;
    std::wstring logFilePath;

    auto processLine = [&](const std::wstring& rawLine)
    {
        std::wstring line = Trim(rawLine);
        if (line.empty() || line[0] == L';' || line[0] == L'#') return;

        // Section header
        if (line[0] == L'[')
        {
            size_t end = line.find(L']');
            if (end != std::wstring::npos)
                currentSection = ToLowerW(Trim(line.substr(1, end - 1)));
            return;
        }

        // Key=Value
        size_t equals = line.find(L'=');
        
        if (equals == std::wstring::npos) 
            return;

        std::wstring key = Trim(line.substr(0, equals));
        std::wstring value = Trim(line.substr(equals + 1));
        
        if (key.empty())
            return;

        if (currentSection == L"settings")
        {
            std::wstring lkey = ToLowerW(key);
            
            if      (lkey == L"logfile")     logFilePath    = value;
            else if (lkey == L"logfiles")    g_logFiles     = ParseBool(value);
            else if (lkey == L"logregistry") g_logRegistry  = ParseBool(value);
        }
        else if (currentSection == L"fileredirects")
        {
            // key = regex pattern, val = replacement
            try
            {
                FileRedirect redirect;
                redirect.replacement = value;
                redirect.pattern     = std::wregex(key,
                    std::regex_constants::ECMAScript | std::regex_constants::icase);
                g_redirects.push_back(std::move(redirect));
            }
            catch (const std::regex_error&) { /* skip malformed patterns */ }
        }
    };

    std::wstring cursor;
    
    for (wchar_t c : content)
    {
        if (c == L'\n') { processLine(cursor); cursor.clear(); }
        else            cursor += c;
    }
    if (!cursor.empty()) processLine(cursor);

    // Open log file if a path was provided
    if (!logFilePath.empty())
    {
        std::wstring expanded = ExpandEnvVars(logFilePath);
        g_logHandle = CreateFileW(expanded.c_str(),
            FILE_APPEND_DATA, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (g_logHandle != INVALID_HANDLE_VALUE)
        {
            // Write session-start separator
            SYSTEMTIME systemTime{};
            GetLocalTime(&systemTime);
            wchar_t separatorTimestamp[128];
            wsprintfW(separatorTimestamp,
                L"# === Session started %04d-%02d-%02d %02d:%02d:%02d ===\r\n",
                systemTime.wYear, systemTime.wMonth, systemTime.wDay,
                systemTime.wHour, systemTime.wMinute, systemTime.wSecond);

            int length = WideCharToMultiByte(CP_UTF8, 0, separatorTimestamp, -1,
                nullptr, 0, nullptr, nullptr);
            
            if (length > 1)
            {
                std::string utf8(length - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, separatorTimestamp, -1,
                    utf8.data(), length - 1, nullptr, nullptr);
                
                DWORD w = 0;
                
                WriteFile(g_logHandle, utf8.c_str(),
                    static_cast<DWORD>(utf8.size()), &w, nullptr);
            }
        }
    }
}
