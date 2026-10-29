#include "platform.h"

#include <cwctype>
#include <iterator>

namespace platform
{

const wchar_t kPathSeparator = L'\\';

// ── Paths ─────────────────────────────────────────────────────────────────────

bool IsAbsolutePath(const std::wstring& path)
{
    if (path.size() >= 2 && path[1] == L':')
        return true;

    return !path.empty() && (path[0] == L'\\' || path[0] == L'/');
}

void TrimTrailingSeparators(std::wstring& path)
{
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/'))
    {
        // "C:\" keeps its slash
        if (path.size() == 3 && path[1] == L':')
            break;

        path.pop_back();
    }
}

bool GetFullPath(const std::wstring& path, std::wstring& out)
{
    wchar_t fullPath[MAX_PATH]{};
    const DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, fullPath, nullptr);

    // Too long for the buffer: the return is the size needed, and nothing was written.
    if (length == 0 || length >= MAX_PATH)
        return false;

    out = fullPath;
    return true;
}

std::wstring ModulePath()
{
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(static_cast<HMODULE>(SelfModule()), buffer, MAX_PATH);
    return buffer;
}

std::wstring ExecutablePath()
{
    wchar_t buffer[MAX_PATH]{};
    return GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0 ? std::wstring(buffer) : std::wstring();
}

std::wstring ModuleDirectory()
{
    wchar_t buffer[MAX_PATH]{};

    GetModuleFileNameW(static_cast<HMODULE>(SelfModule()), buffer, MAX_PATH);

    std::wstring directory(buffer);
    const size_t slash = directory.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        directory.resize(slash + 1);

    return directory;
}

std::wstring ExecutableDirectory()
{
    wchar_t buffer[MAX_PATH]{};

    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0)
        return {};

    std::wstring directory(buffer);
    const size_t slash = directory.find_last_of(L"\\/");

    if (slash == std::wstring::npos)
        return {};

    directory.resize(slash);
    return directory;
}

// CreateDirectoryW makes one level only, and a launcher's base directory is
// typically several levels below anything that exists yet.
void CreateDirectoryTree(const std::wstring& path)
{
    if (path.empty() || CreateDirectoryW(path.c_str(), nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS)
        return;

    std::wstring parent = path;
    TrimTrailingSeparators(parent);

    const size_t slash = parent.find_last_of(L"\\/");

    // Stop at the root: a drive ("C:") or nothing left to strip
    if (slash == std::wstring::npos || slash < 3)
        return;

    parent.resize(slash);

    CreateDirectoryTree(parent);
    CreateDirectoryW(path.c_str(), nullptr);
}

std::vector<DirectoryEntry> ListDirectory(const std::wstring& directory)
{
    std::vector<DirectoryEntry> entries;

    std::wstring pattern = directory;

    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/')
        pattern += L'\\';

    pattern += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);

    if (find == INVALID_HANDLE_VALUE)
        return entries;

    do
    {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        entries.push_back({ fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 });
    }
    while (FindNextFileW(find, &fd));

    FindClose(find);
    return entries;
}

// ── Text ──────────────────────────────────────────────────────────────────────

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring out(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), len);
    return out;
}

std::string Utf16LeToUtf8(const unsigned char* data, size_t bytes)
{
    // wchar_t is UTF-16 LE here, so the bytes already are a wide string.
    const auto* wide  = reinterpret_cast<const wchar_t*>(data);
    const int   count = static_cast<int>(bytes / sizeof(wchar_t));

    int len = WideCharToMultiByte(CP_UTF8, 0, wide, count, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, count, out.data(), len, nullptr, nullptr);
    return out;
}

// ── Files ─────────────────────────────────────────────────────────────────────

FileHandle OpenForAppend(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    return file == INVALID_HANDLE_VALUE ? kInvalidFile : reinterpret_cast<FileHandle>(file);
}

bool WriteToFile(FileHandle file, const void* data, size_t bytes)
{
    DWORD written = 0;

    return file != kInvalidFile &&
        WriteFile(reinterpret_cast<HANDLE>(file), data, static_cast<DWORD>(bytes), &written, nullptr) &&
        written == bytes;
}

void CloseFile(FileHandle file)
{
    if (file != kInvalidFile)
        CloseHandle(reinterpret_cast<HANDLE>(file));
}

bool ReadWholeFile(const std::wstring& path, size_t maxBytes, std::vector<unsigned char>& out)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);

    if (size.QuadPart == 0 || static_cast<unsigned long long>(size.QuadPart) > maxBytes)
    {
        CloseHandle(file);
        return false;
    }

    out.assign(static_cast<size_t>(size.QuadPart), 0);
    DWORD bytesRead = 0;
    ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &bytesRead, nullptr);
    CloseHandle(file);
    return true;
}

bool WriteWholeFile(const std::wstring& path, const std::string& content)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    BOOL ok = WriteFile(file, content.c_str(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == content.size();
}

bool PathExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ReplaceFile(const std::wstring& from, const std::wstring& to)
{
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

void RemoveFile(const std::wstring& path)
{
    DeleteFileW(path.c_str());
}

// ── Environment ───────────────────────────────────────────────────────────────

bool GetEnvironmentValue(const std::wstring& name, std::wstring& out)
{
    if (name.empty())
        return false;

    DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);

    if (needed == 0)
        return false;

    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(name.c_str(), value.data(), needed);

    if (written == 0)
        return false;

    value.resize(written);
    out = value;
    return true;
}

std::wstring ExpandEnvironment(const std::wstring& text)
{
    DWORD needed = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);

    if (needed == 0)
        return text;

    std::wstring result(needed, L'\0');

    ExpandEnvironmentStringsW(text.c_str(), result.data(), needed);

    // ExpandEnvironmentStringsW includes the null terminator in 'needed'
    if (!result.empty() && result.back() == L'\0')
        result.pop_back();

    return result;
}

namespace
{
    // Known folders that have no environment variable equivalent, resolved from
    // HKCU\...\Explorer\User Shell Folders rather than SHGetKnownFolderPath:
    // LoadConfig() runs from DllMain under the loader lock, and pulling shell32
    // and ole32 in at that point invites a deadlock. advapi32 is already loaded,
    // and our own registry hooks are not installed until after LoadConfig
    // returns, so these reads cannot re-enter them.
    //
    // Names that already exist as environment variables — APPDATA, LOCALAPPDATA,
    // USERPROFILE, PROGRAMDATA, PROGRAMFILES — are deliberately absent. The
    // environment is consulted first, so listing them here would only shadow
    // behaviour that existing configs already depend on.
    struct PathTokenEntry {
        const wchar_t* name;      // token spelling, uppercase
        const wchar_t* regValue;  // value name under User Shell Folders
        const wchar_t* suffix;    // appended to the resolved folder
        const wchar_t* fallback;  // relative to %USERPROFILE% when the value is absent
    };

    constexpr PathTokenEntry kPathTokens[] = {
        { L"SAVEDGAMES",  L"{4C5C32FF-BB9D-43b0-B5B4-2D72E54EAAA4}", L"",           L"Saved Games"         },
        { L"DOCUMENTS",   L"Personal",                               L"",           L"Documents"           },
        { L"MYDOCUMENTS", L"Personal",                               L"",           L"Documents"           },
        { L"MYGAMES",     L"Personal",                               L"\\My Games", L"Documents\\My Games" },
        { L"DESKTOP",     L"Desktop",                                L"",           L"Desktop"             },
        { L"PICTURES",    L"My Pictures",                            L"",           L"Pictures"            },
        { L"MUSIC",       L"My Music",                               L"",           L"Music"               },
        { L"VIDEOS",      L"My Video",                               L"",           L"Videos"              },
    };

    // Resolved once by ResolveKnownFolders(), which LoadConfig calls before any
    // hook is enabled. Caching is not just an optimization: the replacement side
    // of a rule expands tokens on every redirected file open, and a registry read
    // from inside the CreateFileW hook would re-enter our own advapi32 hooks.
    std::wstring g_pathTokenValues[std::size(kPathTokens)];

    // Read one REG_SZ / REG_EXPAND_SZ from User Shell Folders. Values there are
    // normally REG_EXPAND_SZ holding things like %USERPROFILE%\Saved Games, hence
    // the ExpandEnvironment pass on the way out.
    bool ReadUserShellFolder(const wchar_t* valueName, std::wstring& out)
    {
        HKEY key = nullptr;

        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return false;

        DWORD type  = 0;
        DWORD bytes = 0;
        LONG  status = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);

        if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t) ||
            (type != REG_SZ && type != REG_EXPAND_SZ))
        {
            RegCloseKey(key);
            return false;
        }

        std::wstring raw(bytes / sizeof(wchar_t), L'\0');

        status = RegQueryValueExW(key, valueName, nullptr, &type,
            reinterpret_cast<LPBYTE>(raw.data()), &bytes);

        RegCloseKey(key);

        if (status != ERROR_SUCCESS)
            return false;

        // The stored byte count includes the terminator; std::wstring supplies its own.
        while (!raw.empty() && raw.back() == L'\0')
            raw.pop_back();

        if (raw.empty())
            return false;

        out = ExpandEnvironment(raw);
        TrimTrailingSeparators(out);
        return !out.empty();
    }
}

void ResolveKnownFolders()
{
    std::wstring profile;

    if (GetEnvironmentValue(L"USERPROFILE", profile))
        TrimTrailingSeparators(profile);

    for (size_t i = 0; i < std::size(kPathTokens); ++i)
    {
        const PathTokenEntry& entry = kPathTokens[i];

        std::wstring folder;

        if (ReadUserShellFolder(entry.regValue, folder))
            g_pathTokenValues[i] = folder + entry.suffix;
        else if (!profile.empty())
            g_pathTokenValues[i] = profile + L"\\" + entry.fallback;
    }
}

bool KnownFolder(const std::wstring& upperName, std::wstring& out)
{
    for (size_t i = 0; i < std::size(kPathTokens); ++i)
    {
        if (upperName != kPathTokens[i].name || g_pathTokenValues[i].empty())
            continue;

        out = g_pathTokenValues[i];
        return true;
    }

    return false;
}

bool CurrentUserName(std::wstring& out)
{
    wchar_t buffer[257]{};   // UNLEN + 1
    DWORD   size = static_cast<DWORD>(std::size(buffer));

    if (!GetUserNameW(buffer, &size))
        return false;

    out = buffer;
    return true;
}

// ── Time ──────────────────────────────────────────────────────────────────────

LocalTime Now()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return { st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond };
}

// ── Process and launcher ──────────────────────────────────────────────────────

unsigned long ProcessId()
{
    return GetCurrentProcessId();
}

// Read Local\<name>_<pid>, a UTF-8 string the launcher wrote before injecting.
// The whole view is mapped and read up to its first NUL, so the launcher sizes
// the mapping to its content rather than to a limit agreed in advance.
std::string ReadLauncherValue(const wchar_t* name)
{
    wchar_t mappingName[96]{};
    swprintf_s(mappingName, L"Local\\%ls_%lu", name, GetCurrentProcessId());

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName);
    if (!mapping)
        return {};

    std::string text;

    if (const char* view = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)))
    {
        MEMORY_BASIC_INFORMATION info{};
        const SIZE_T size = VirtualQuery(view, &info, sizeof(info)) ? info.RegionSize : 0;

        text.assign(view, strnlen(view, size));
        UnmapViewOfFile(view);
    }

    CloseHandle(mapping);
    return text;
}

FileHandle ConnectEventChannel()
{
    wchar_t pipeName[128]{};
    swprintf_s(pipeName, L"\\\\.\\pipe\\InterposerEvents_%lu", GetCurrentProcessId());

    HANDLE pipe = CreateFileW(pipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

    return pipe == INVALID_HANDLE_VALUE ? kInvalidFile : reinterpret_cast<FileHandle>(pipe);
}

bool WriteEventChannel(FileHandle channel, const void* data, size_t bytes)
{
    DWORD written = 0;
    return WriteFile(reinterpret_cast<HANDLE>(channel), data, static_cast<DWORD>(bytes), &written, nullptr) != FALSE;
}

// ── Modules ───────────────────────────────────────────────────────────────────

Module SelfModule()
{
    HMODULE self = nullptr;

    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&SelfModule),
        &self);

    return self;
}

// Read from the version resource. Cached after the first call.
std::wstring ModuleVersion()
{
    static std::wstring s_version;
    static bool         s_ready = false;

    if (s_ready)
        return s_version;

    s_ready   = true;
    s_version = L"1.0";

    const std::wstring dllPath = ModulePath();

    DWORD dummy = 0;
    DWORD size  = GetFileVersionInfoSizeW(dllPath.c_str(), &dummy);
    if (size == 0)
        return s_version;

    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(dllPath.c_str(), 0, size, buf.data()))
        return s_version;

    VS_FIXEDFILEINFO* info    = nullptr;
    UINT              infoLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\",
            reinterpret_cast<LPVOID*>(&info), &infoLen) || !info)
        return s_version;

    if (info->dwFileVersionMS == 0 && info->dwFileVersionLS == 0)
        return s_version;

    wchar_t text[32]{};
    swprintf_s(text, L"%d.%d.%d.%d",
        HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
        HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));

    s_version = text;
    return s_version;
}

Module LoadModule(const std::wstring& path, std::wstring& error)
{
    HMODULE module = LoadLibraryW(path.c_str());

    if (!module)
        error = L"error " + std::to_wstring(GetLastError());

    return module;
}

void* FindSymbol(Module module, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
}

void UnloadModule(Module module)
{
    FreeLibrary(static_cast<HMODULE>(module));
}

const std::vector<std::wstring>& PluginExtensions()
{
    static const std::vector<std::wstring> extensions = { L".dll", L".asi" };
    return extensions;
}

} // namespace platform
