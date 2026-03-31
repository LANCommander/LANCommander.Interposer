#include "registry.h"
#include "config.h"

#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

// ============================================================
// In-memory store
// ============================================================
struct RegValue { DWORD type; std::vector<BYTE> data; };
using ValueMap = std::map<std::wstring, RegValue>;  // uppercase valueName → value
using StoreMap = std::map<std::wstring, ValueMap>;  // uppercase keyPath  → values

static StoreMap          g_store;
static std::shared_mutex g_storeMutex;
static std::wstring      g_filePath;
static bool              g_dirty = false;

// ============================================================
// Virtual handle table
// ============================================================
struct VirtKey { std::wstring path; };

static std::map<HKEY, VirtKey*> g_handles;
static std::mutex               g_handleMutex;

static HKEY NewVirtHandle(std::wstring path)
{
    VirtKey* vk = new VirtKey{ std::move(path) };
    std::lock_guard lk(g_handleMutex);
    g_handles[reinterpret_cast<HKEY>(vk)] = vk;
    return reinterpret_cast<HKEY>(vk);
}

// Returns the virtual key's path, or "" if hKey is not a virtual handle.
static std::wstring GetVirtualPath(HKEY h)
{
    std::lock_guard lk(g_handleMutex);
    auto it = g_handles.find(h);
    if (it != g_handles.end())
        return it->second->path;
    return {};
}

// ============================================================
// Real handle tracking (for logging real registry operations)
// ============================================================
// Tracks real OS registry handles opened through our hooks so we can log
// their paths when they're queried, written, or deleted.
static std::map<HKEY, std::wstring> g_realHandles;
static std::mutex                   g_realHandleMtx;

static void TrackRealHandle(HKEY hKey, const std::wstring& path)
{
    if (path.empty()) return;
    std::lock_guard lk(g_realHandleMtx);
    g_realHandles[hKey] = path;
}

static void UntrackRealHandle(HKEY hKey)
{
    std::lock_guard lk(g_realHandleMtx);
    g_realHandles.erase(hKey);
}

// ============================================================
// Path utilities
// ============================================================
static const wchar_t* PredefinedName(HKEY h)
{
    if (h == HKEY_CLASSES_ROOT)   return L"HKEY_CLASSES_ROOT";
    if (h == HKEY_CURRENT_USER)   return L"HKEY_CURRENT_USER";
    if (h == HKEY_LOCAL_MACHINE)  return L"HKEY_LOCAL_MACHINE";
    if (h == HKEY_USERS)          return L"HKEY_USERS";
    if (h == HKEY_CURRENT_CONFIG) return L"HKEY_CURRENT_CONFIG";
    return nullptr;
}

static std::wstring ToUpper(std::wstring s)
{
    for (auto& c : s) c = towupper(c);
    return s;
}

// Translate WOW64 UAC registry virtualization paths back to their canonical locations.
//   HKEY_CURRENT_USER\SOFTWARE\CLASSES\VIRTUALSTORE\MACHINE\<X>  ->  HKEY_LOCAL_MACHINE\<X>
//   HKEY_USERS\<SID>\SOFTWARE\CLASSES\VIRTUALSTORE\MACHINE\<X>   ->  HKEY_LOCAL_MACHINE\<X>
// Input must already be uppercased (as returned by ToUpper / BuildPath).
static std::wstring NormalizeVirtualStore(std::wstring path)
{
    // HKEY_CURRENT_USER variant
    {
        static const wchar_t kPrefix[] =
            L"HKEY_CURRENT_USER\\SOFTWARE\\CLASSES\\VIRTUALSTORE\\MACHINE\\";
        const size_t kPrefixLen = ARRAYSIZE(kPrefix) - 1;
        if (path.size() > kPrefixLen && path.compare(0, kPrefixLen, kPrefix) == 0)
            return L"HKEY_LOCAL_MACHINE\\" + path.substr(kPrefixLen);
    }

    // HKEY_USERS\<SID>\... variant
    {
        static const wchar_t kHU[]  = L"HKEY_USERS\\";
        static const wchar_t kVSM[] = L"\\SOFTWARE\\CLASSES\\VIRTUALSTORE\\MACHINE\\";
        const size_t kHULen  = ARRAYSIZE(kHU)  - 1;
        const size_t kVSMLen = ARRAYSIZE(kVSM) - 1;

        if (path.size() > kHULen + kVSMLen &&
            path.compare(0, kHULen, kHU) == 0)
        {
            size_t sidEnd = path.find(L'\\', kHULen);
            if (sidEnd != std::wstring::npos &&
                path.size() > sidEnd + kVSMLen &&
                path.compare(sidEnd, kVSMLen, kVSM) == 0)
            {
                return L"HKEY_LOCAL_MACHINE\\" + path.substr(sidEnd + kVSMLen);
            }
        }
    }

    return path;
}

// Returns the full uppercase path from base hKey + optional subkey.
// Returns "" if hKey is an unrecognised real OS handle.
static std::wstring BuildPath(HKEY hKey, LPCWSTR lpSubKey)
{
    std::wstring base;

    // 1. Virtual handles issued by us
    {
        std::lock_guard lk(g_handleMutex);
        auto it = g_handles.find(hKey);
        if (it != g_handles.end())
            base = it->second->path;
    }

    // 2. Real handles we tracked (e.g. from a previous RegOpenKeyExW call)
    if (base.empty())
    {
        std::lock_guard lk(g_realHandleMtx);
        auto it = g_realHandles.find(hKey);
        if (it != g_realHandles.end())
            base = it->second;
    }

    // 3. Predefined root handles (HKLM, HKCU, …)
    if (base.empty())
    {
        const wchar_t* predefined = PredefinedName(hKey);
        if (!predefined)
            return {};
        base = predefined;
    }

    if (lpSubKey && lpSubKey[0] != L'\0')
    {
        base += L'\\';
        base += lpSubKey;
    }

    return NormalizeVirtualStore(ToUpper(base));
}

// Ask the OS for the full path of any open HKEY via NtQueryKey, then convert
// the NT path (\REGISTRY\MACHINE\...) to a Win32 hive path (HKEY_LOCAL_MACHINE\...).
// Works for predefined roots and handles opened before our hooks were installed.
static std::wstring GetKeyPathViaSystem(HKEY hiveKey)
{
    using FnNtQueryKey = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static FnNtQueryKey s_fn   = nullptr;
    static bool         s_init = false;
    
    if (!s_init)
    {
        s_fn   = reinterpret_cast<FnNtQueryKey>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryKey"));
        s_init = true;
    }
    
    if (!s_fn) 
        return {};

    // KeyNameInformation (class 3): { ULONG NameLength; WCHAR Name[1]; }
    constexpr int KeyNameInformation = 3;
    ULONG needed = 0;
    s_fn(hiveKey, KeyNameInformation, nullptr, 0, &needed);
    if (needed == 0) return {};

    std::vector<BYTE> buffer(needed + sizeof(ULONG) + sizeof(WCHAR));
    if (s_fn(hiveKey, KeyNameInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed) != 0)
        return {};

    ULONG nameBytes = *reinterpret_cast<const ULONG*>(buffer.data());
    std::wstring ntPath(reinterpret_cast<const WCHAR*>(buffer.data() + sizeof(ULONG)),
                        nameBytes / sizeof(WCHAR));

    // Map NT registry root prefixes → Win32 hive names
    static const struct { const wchar_t* nt; const wchar_t* win32; } hiveKeyMappings[] = {
        { L"\\REGISTRY\\MACHINE", L"HKEY_LOCAL_MACHINE"  },
        { L"\\REGISTRY\\USER",    L"HKEY_USERS"          },
        { L"\\REGISTRY\\CONFIG",  L"HKEY_CURRENT_CONFIG" },
    };
    for (const auto& hiveKeyMapping : hiveKeyMappings)
    {
        size_t ntLength = wcslen(hiveKeyMapping.nt);
        if (ntPath.size() >= ntLength &&
            _wcsnicmp(ntPath.c_str(), hiveKeyMapping.nt, ntLength) == 0 &&
            (ntPath.size() == ntLength || ntPath[ntLength] == L'\\'))
        {
            std::wstring result = hiveKeyMapping.win32;
            
            if (ntPath.size() > ntLength)
                result += ntPath.substr(ntLength); // retains leading '\'
            
            return ToUpper(result);
        }
    }
    
    return ToUpper(ntPath); // unknown prefix — keep the NT path as-is
}

// Returns the path for any handle (virtual, tracked real, predefined root,
// or untracked/pre-injection real handle), or "" if completely unrecognised.
// NOTE: defined after BuildPath/ToUpper/PredefinedName so all helpers are available.
static std::wstring GetAnyPathFull(HKEY h)
{
    {
        std::lock_guard lk(g_handleMutex);
        auto it = g_handles.find(h);
        if (it != g_handles.end()) return it->second->path;
    }
    {
        std::lock_guard lk(g_realHandleMtx);
        auto it = g_realHandles.find(h);
        if (it != g_realHandles.end()) return it->second;
    }
    if (const wchar_t* name = PredefinedName(h))
        return ToUpper(name);
    return NormalizeVirtualStore(GetKeyPathViaSystem(h));
}

// True if upperPath exactly matches, or is an ancestor/descendant of any stored key.
static bool InVirtualSpace(const std::wstring& upperPath)
{
    if (upperPath.empty()) return false;

    std::shared_lock lock(g_storeMutex);
    
    for (auto& [key, _] : g_store)
    {
        if (key.size() >= upperPath.size())
        {
            // key is a descendant-or-equal of upperPath
            if (key.compare(0, upperPath.size(), upperPath) == 0 &&
                (key.size() == upperPath.size() || key[upperPath.size()] == L'\\'))
                return true;
        }
        else
        {
            // key is an ancestor of upperPath
            if (upperPath.compare(0, key.size(), key) == 0 &&
                (upperPath.size() == key.size() || upperPath[key.size()] == L'\\'))
                return true;
        }
    }
    
    return false;
}

// ============================================================
// String conversion helpers
// ============================================================
static std::wstring AnsiToWide(LPCSTR input, int length = -1)
{
    if (!input)
        return {};
    
    if (length == 0)
        return {};
    
    int wideLength = MultiByteToWideChar(CP_ACP, 0, input, length, nullptr, 0);
    
    if (wideLength <= 0)
        return {};
    
    std::wstring wideString(wideLength, L'\0');
    
    MultiByteToWideChar(CP_ACP, 0, input, length, wideString.data(), wideLength);
    
    // When len == -1 the null terminator is counted in wlen
    if (length == -1 && !wideString.empty() && wideString.back() == L'\0')
        wideString.pop_back();
    
    return wideString;
}

static std::string WideToAnsi(LPCWSTR input, int length = -1)
{
    if (!input)
        return {};
    
    if (length == 0)
        return {};
    
    int ansiLength = WideCharToMultiByte(CP_ACP, 0, input, length, nullptr, 0, nullptr, nullptr);
    
    if (ansiLength <= 0) 
        return {};
    
    std::string ansiString(ansiLength, '\0');
    
    WideCharToMultiByte(CP_ACP, 0, input, length, ansiString.data(), ansiLength, nullptr, nullptr);
    
    if (length == -1 && !ansiString.empty() && ansiString.back() == '\0')
        ansiString.pop_back();
    
    return ansiString;
}

// ============================================================
// .reg file parsing helpers
// ============================================================

// Parse a comma-separated hex byte string with optional embedded whitespace / backslashes.
static std::vector<BYTE> ParseHexBytes(const std::wstring& input)
{
    std::vector<BYTE> result;
    std::wstring token;
    for (wchar_t character : input)
    {
        if (character == L',' )
        {
            if (!token.empty())
            {
                result.push_back(static_cast<BYTE>(wcstoul(token.c_str(), nullptr, 16)));
                token.clear();
            }
        }
        else if (character == L' ' || character == L'\t' || character == L'\\' || character == L'\r' || character == L'\n')
        {
            // skip whitespace / continuation characters
        }
        else
        {
            token += character;
        }
    }
    
    if (!token.empty())
        result.push_back(static_cast<BYTE>(wcstoul(token.c_str(), nullptr, 16)));
    
    return result;
}

static std::wstring UnescapeString(const std::wstring& input)
{
    std::wstring result;
    result.reserve(input.size());
    
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (input[i] == L'\\' && i + 1 < input.size())
        {
            ++i;
            
            if (input[i] == L'\\')
                result += L'\\';
            else if (input[i] == L'"')
                result += L'"';
            else
            {
                result += L'\\'; result += input[i];
            }
        }
        else
            result += input[i];
    }
    
    return result;
}

static std::wstring EscapeString(const std::wstring& input)
{
    std::wstring result;
    result.reserve(input.size());
    
    for (wchar_t character : input)
    {
        if (character == L'\\')
            result += L"\\\\";
        else if (character == L'"')
            result += L"\\\"";
        else
            result += character;
    }
    
    return result;
}

// ============================================================
// File I/O
// ============================================================
static void LoadRegFile()
{
    if (g_filePath.empty())
        return;

    HANDLE fileHandle = CreateFileW(g_filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    
    if (fileHandle == INVALID_HANDLE_VALUE)
        return;

    LARGE_INTEGER fileSize{};
    
    GetFileSizeEx(fileHandle, &fileSize);
    
    if (fileSize.QuadPart == 0 || fileSize.QuadPart > 64 * 1024 * 1024)
    {
        CloseHandle(fileHandle);
        return;
    }

    std::vector<BYTE> raw(static_cast<size_t>(fileSize.QuadPart));
    
    DWORD bytesRead = 0;
    
    ReadFile(fileHandle, raw.data(), static_cast<DWORD>(raw.size()), &bytesRead, nullptr);
    
    CloseHandle(fileHandle);

    // Decode to wstring
    std::wstring content;
    
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
    {
        // UTF-16 LE with BOM
        size_t characterCount = (raw.size() - 2) / 2;
        
        content.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2), characterCount);
    }
    else
    {
        // UTF-8 / ANSI
        const char* source = reinterpret_cast<const char*>(raw.data());
        
        int wideLength = MultiByteToWideChar(CP_UTF8, 0, source, static_cast<int>(raw.size()), nullptr, 0);
        
        content.resize(wideLength);
        
        MultiByteToWideChar(CP_UTF8, 0, source, static_cast<int>(raw.size()), content.data(), wideLength);
    }

    // Split into lines
    std::vector<std::wstring> lines;
    {
        std::wstring currentLine;
        
        for (wchar_t character : content)
        {
            if (character == L'\n')
            {
                if (!currentLine.empty() && currentLine.back() == L'\r')
                    currentLine.pop_back();
                
                lines.push_back(std::move(currentLine));
                
                currentLine.clear();
            }
            else
                currentLine += character;
        }
        
        if (!currentLine.empty())
            lines.push_back(std::move(currentLine));
    }

    // Parse
    std::wstring currentKey;
    std::wstring pending;   // accumulated across backslash-continued lines

    for (auto& rawLine : lines)
    {
        std::wstring line;

        // Accumulate continuation lines
        if (!pending.empty())
        {
            pending += rawLine;
            if (!pending.empty() && pending.back() == L'\\')
            {
                pending.pop_back();
                continue;
            }
            
            line = std::move(pending);
            pending.clear();
        }
        else
        {
            line = rawLine;
        }

        // Skip comments and blank lines
        if (line.empty() || line[0] == L';')
            continue;

        // Key header
        if (line[0] == L'[')
        {
            size_t end = line.rfind(L']');
            if (end != std::wstring::npos && end > 0)
            {
                currentKey = ToUpper(line.substr(1, end - 1));
                std::unique_lock lk(g_storeMutex);
                g_store.emplace(currentKey, ValueMap{});
            }
            
            continue;
        }

        if (currentKey.empty())
            continue;

        // Check whether this line starts a backslash-continued value
        if (line[0] == L'"' && !line.empty() && line.back() == L'\\' &&
            line.find(L'=') != std::wstring::npos)
        {
            pending = line;
            pending.pop_back();
            
            continue;
        }

        // Value entry: "name"=<data>
        if (line[0] != L'"')
            continue;

        size_t nameEnd = line.find(L'"', 1);
        
        if (nameEnd == std::wstring::npos)
            continue;
        
        size_t equalSignPosition = nameEnd + 1;
        
        if (equalSignPosition >= line.size() || line[equalSignPosition] != L'=')
            continue;

        std::wstring valueName = ToUpper(line.substr(1, nameEnd - 1));
        std::wstring valueData = line.substr(equalSignPosition + 1);

        RegValue registryValue{};

        if (!valueData.empty() && valueData[0] == L'"')
        {
            registryValue.type = REG_SZ;
            
            size_t strEnd = valueData.rfind(L'"');
            
            if (strEnd == 0)
                strEnd = valueData.size(); // malformed, treat whole as content
            
            std::wstring valueString = UnescapeString(valueData.substr(1, strEnd - 1));
            
            registryValue.data.resize((valueString.size() + 1) * sizeof(wchar_t));
            
            memcpy(registryValue.data.data(), valueString.c_str(), registryValue.data.size());
        }
        else if (valueData.compare(0, 6, L"dword:") == 0)
        {
            registryValue.type = REG_DWORD;
            
            DWORD dw = wcstoul(valueData.c_str() + 6, nullptr, 16);
            
            registryValue.data.resize(sizeof(DWORD));
            
            memcpy(registryValue.data.data(), &dw, sizeof(DWORD));
        }
        else if (valueData.compare(0, 7, L"hex(2):") == 0)
        {
            registryValue.type = REG_EXPAND_SZ;
            registryValue.data = ParseHexBytes(valueData.substr(7));
        }
        else if (valueData.compare(0, 7, L"hex(7):") == 0)
        {
            registryValue.type = REG_MULTI_SZ;
            registryValue.data = ParseHexBytes(valueData.substr(7));
        }
        else if (valueData.compare(0, 7, L"hex(b):") == 0)
        {
            registryValue.type = REG_QWORD;
            registryValue.data = ParseHexBytes(valueData.substr(7));
        }
        else if (valueData.compare(0, 4, L"hex:") == 0)
        {
            registryValue.type = REG_BINARY;
            registryValue.data = ParseHexBytes(valueData.substr(4));
        }
        else
            continue; // unknown type

        std::unique_lock lock(g_storeMutex);
        
        g_store[currentKey][valueName] = std::move(registryValue);
    }
}

static void SaveRegFile()
{
    if (g_filePath.empty())
        return;

    // Build the output string while holding a shared lock
    std::wstring output;
    
    output.reserve(4096);
    
    output = L"Windows Registry Editor Version 5.00\r\n";

    {
        std::shared_lock lock(g_storeMutex);

        for (auto& [key, values] : g_store)
        {
            output += L"\r\n[";
            output += key;
            output += L"]\r\n";

            for (auto& [name, rv] : values)
            {
                output += L'"';
                output += name;
                output += L"\"=";

                if (rv.type == REG_SZ)
                {
                    const wchar_t* wideString = reinterpret_cast<const wchar_t*>(rv.data.data());
                    size_t wideLength = rv.data.size() / sizeof(wchar_t);
                    
                    if (wideLength > 0 && wideString[wideLength - 1] == L'\0')
                        --wideLength; // strip stored null
                    
                    output += L'"';
                    output += EscapeString(std::wstring(wideString, wideLength));
                    output += L"\"\r\n";
                }
                else if (rv.type == REG_DWORD)
                {
                    DWORD dw = 0;
                    
                    if (rv.data.size() >= sizeof(DWORD))
                        memcpy(&dw, rv.data.data(), sizeof(DWORD));
                    
                    wchar_t buffer[16];
                    swprintf_s(buffer, L"dword:%08x", dw);
                    output += buffer;
                    output += L"\r\n";
                }
                else
                {
                    const wchar_t* prefix =
                        (rv.type == REG_EXPAND_SZ) ? L"hex(2):" :
                        (rv.type == REG_MULTI_SZ)  ? L"hex(7):" :
                        (rv.type == REG_QWORD)     ? L"hex(b):" :
                                                     L"hex:";
                    
                    output += prefix;
                    
                    // Emit bytes with line-wrap every 25 bytes
                    for (size_t i = 0; i < rv.data.size(); ++i)
                    {
                        wchar_t byteString
                        [4];
                        swprintf_s(byteString, L"%02x", rv.data[i]);
                        
                        output += byteString;
                        
                        if (i + 1 < rv.data.size())
                        {
                            output += L',';
                            
                            if ((i + 1) % 25 == 0)
                                output += L"\\\r\n  ";
                        }
                    }
                    
                    output += L"\r\n";
                }
            }
        }
    }

    // Write UTF-16 LE with BOM
    HANDLE fileHandle = CreateFileW(g_filePath.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    
    if (fileHandle == INVALID_HANDLE_VALUE)
        return;

    WORD bom = 0xFEFF;
    DWORD written = 0;
    
    WriteFile(fileHandle, &bom, sizeof(bom), &written, nullptr);
    WriteFile(fileHandle, output.data(), static_cast<DWORD>(output.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(fileHandle);

    g_dirty = false;
}

// ============================================================
// Trampoline pointers
// ============================================================
using FnRegOpenKeyExW    = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using FnRegOpenKeyExA    = LSTATUS(WINAPI*)(HKEY, LPCSTR,  DWORD, REGSAM, PHKEY);
using FnRegCreateKeyExW  = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR,  DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
using FnRegCreateKeyExA  = LSTATUS(WINAPI*)(HKEY, LPCSTR,  DWORD, LPSTR,   DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
using FnRegCloseKey      = LSTATUS(WINAPI*)(HKEY);
using FnRegQueryValueExW = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using FnRegQueryValueExA = LSTATUS(WINAPI*)(HKEY, LPCSTR,  LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using FnRegSetValueExW   = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
using FnRegSetValueExA   = LSTATUS(WINAPI*)(HKEY, LPCSTR,  DWORD, DWORD, const BYTE*, DWORD);
using FnRegDeleteValueW  = LSTATUS(WINAPI*)(HKEY, LPCWSTR);
using FnRegDeleteValueA  = LSTATUS(WINAPI*)(HKEY, LPCSTR);
using FnRegEnumValueW    = LSTATUS(WINAPI*)(HKEY, DWORD, LPWSTR,  LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using FnRegEnumValueA    = LSTATUS(WINAPI*)(HKEY, DWORD, LPSTR,   LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using FnRegEnumKeyExW    = LSTATUS(WINAPI*)(HKEY, DWORD, LPWSTR,  LPDWORD, LPDWORD, LPWSTR,  LPDWORD, PFILETIME);
using FnRegEnumKeyExA    = LSTATUS(WINAPI*)(HKEY, DWORD, LPSTR,   LPDWORD, LPDWORD, LPSTR,   LPDWORD, PFILETIME);
using FnRegQueryInfoKeyW = LSTATUS(WINAPI*)(HKEY, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);
using FnRegQueryInfoKeyA = LSTATUS(WINAPI*)(HKEY, LPSTR,  LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);

static FnRegOpenKeyExW    g_origRegOpenKeyExW    = nullptr;
static FnRegOpenKeyExA    g_origRegOpenKeyExA    = nullptr;
static FnRegCreateKeyExW  g_origRegCreateKeyExW  = nullptr;
static FnRegCreateKeyExA  g_origRegCreateKeyExA  = nullptr;
static FnRegCloseKey      g_origRegCloseKey      = nullptr;
static FnRegQueryValueExW g_origRegQueryValueExW = nullptr;
static FnRegQueryValueExA g_origRegQueryValueExA = nullptr;
static FnRegSetValueExW   g_origRegSetValueExW   = nullptr;
static FnRegSetValueExA   g_origRegSetValueExA   = nullptr;
static FnRegDeleteValueW  g_origRegDeleteValueW  = nullptr;
static FnRegDeleteValueA  g_origRegDeleteValueA  = nullptr;
static FnRegEnumValueW    g_origRegEnumValueW    = nullptr;
static FnRegEnumValueA    g_origRegEnumValueA    = nullptr;
static FnRegEnumKeyExW    g_origRegEnumKeyExW    = nullptr;
static FnRegEnumKeyExA    g_origRegEnumKeyExA    = nullptr;
static FnRegQueryInfoKeyW g_origRegQueryInfoKeyW = nullptr;
static FnRegQueryInfoKeyA g_origRegQueryInfoKeyA = nullptr;

// ============================================================
// Hook implementations
// ============================================================

// --- RegOpenKeyEx ---
static LSTATUS WINAPI HookRegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
    REGSAM samDesired, PHKEY phkResult)
{
    std::wstring path = BuildPath(hKey, lpSubKey);
    
    LogRegistryAccess(L"[REG OPEN]     ", path.empty() ? L"(unknown)" : path.c_str());

    if (!path.empty() && InVirtualSpace(path))
    {
        if (phkResult) *phkResult = NewVirtHandle(path);
        return ERROR_SUCCESS;
    }

    LSTATUS st = g_origRegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
    
    if (st == ERROR_SUCCESS && phkResult && *phkResult)
        TrackRealHandle(*phkResult, path);
    
    return st;
}

static LSTATUS WINAPI HookRegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions,
    REGSAM samDesired, PHKEY phkResult)
{
    std::wstring wideSubKey = lpSubKey ? AnsiToWide(lpSubKey) : std::wstring{};
    std::wstring path = BuildPath(hKey, wideSubKey.empty() ? nullptr : wideSubKey.c_str());
    LogRegistryAccess(L"[REG OPEN]     ", path.empty() ? L"(unknown)" : path.c_str());

    if (!path.empty() && InVirtualSpace(path))
    {
        if (phkResult)
            *phkResult = NewVirtHandle(path);
        
        return ERROR_SUCCESS;
    }

    LSTATUS st = g_origRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
    
    if (st == ERROR_SUCCESS && phkResult && *phkResult)
        TrackRealHandle(*phkResult, path);
    
    return st;
}

// --- RegCreateKeyEx ---
static LSTATUS WINAPI HookRegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved,
    LPWSTR lpClass, DWORD dwOptions, REGSAM samDesired,
    LPSECURITY_ATTRIBUTES lpSA, PHKEY phkResult, LPDWORD lpdwDisposition)
{
    std::wstring path = BuildPath(hKey, lpSubKey);
    
    LogRegistryAccess(L"[REG CREATE]   ", path.empty() ? L"(unknown)" : path.c_str());

    if (!path.empty() && InVirtualSpace(path))
    {
        DWORD disposition;
        {
            std::unique_lock lk(g_storeMutex);
            bool existed     = g_store.count(path) > 0;
            g_store.emplace(path, ValueMap{});
            disposition      = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
        }
        
        if (phkResult)
            *phkResult = NewVirtHandle(path);
        
        if (lpdwDisposition)
            *lpdwDisposition = disposition;
        
        return ERROR_SUCCESS;
    }

    LSTATUS status = g_origRegCreateKeyExW(hKey, lpSubKey, Reserved, lpClass, dwOptions,
        samDesired, lpSA, phkResult, lpdwDisposition);
    
    if (status == ERROR_SUCCESS && phkResult && *phkResult)
        TrackRealHandle(*phkResult, path);
    
    return status;
}

static LSTATUS WINAPI HookRegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved,
    LPSTR lpClass, DWORD dwOptions, REGSAM samDesired,
    LPSECURITY_ATTRIBUTES lpSA, PHKEY phkResult, LPDWORD lpdwDisposition)
{
    std::wstring wideSubKey = lpSubKey ? AnsiToWide(lpSubKey) : std::wstring{};
    std::wstring path = BuildPath(hKey, wideSubKey.empty() ? nullptr : wideSubKey.c_str());
    
    LogRegistryAccess(L"[REG CREATE]   ", path.empty() ? L"(unknown)" : path.c_str());

    if (!path.empty() && InVirtualSpace(path))
    {
        DWORD disposition;
        {
            std::unique_lock lk(g_storeMutex);
            bool existed     = g_store.count(path) > 0;
            g_store.emplace(path, ValueMap{});
            disposition      = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
        }
        
        if (phkResult)
            *phkResult = NewVirtHandle(path);
        
        if (lpdwDisposition)
            *lpdwDisposition  = disposition;
        
        return ERROR_SUCCESS;
    }

    LSTATUS status = g_origRegCreateKeyExA(hKey, lpSubKey, Reserved, lpClass, dwOptions,
        samDesired, lpSA, phkResult, lpdwDisposition);
    
    if (status == ERROR_SUCCESS && phkResult && *phkResult)
        TrackRealHandle(*phkResult, path);
    
    return status;
}

// --- RegCloseKey ---
static LSTATUS WINAPI HookRegCloseKey(HKEY hKey)
{
    {
        std::lock_guard lock(g_handleMutex);
        auto it = g_handles.find(hKey);
        
        if (it != g_handles.end())
        {
            delete it->second;
            g_handles.erase(it);
            
            return ERROR_SUCCESS;
        }
    }
    
    UntrackRealHandle(hKey);
    
    return g_origRegCloseKey(hKey);
}

// --- RegQueryValueEx (shared core) ---
static LSTATUS VirtualQueryValueW(const std::wstring& keyPath, LPCWSTR lpValueName,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    std::wstring upperName = ToUpper(lpValueName ? lpValueName : L"");

    std::shared_lock lock(g_storeMutex);

    auto kit = g_store.find(keyPath);
    
    if (kit == g_store.end())
        return ERROR_FILE_NOT_FOUND;

    auto vit = kit->second.find(upperName);
    
    if (vit == kit->second.end())
        return ERROR_FILE_NOT_FOUND;

    const RegValue& rv = vit->second;
    
    if (lpType)
        *lpType = rv.type;

    if (lpcbData)
    {
        DWORD needed = static_cast<DWORD>(rv.data.size());
        
        if (lpData)
        {
            if (*lpcbData < needed)
            {
                *lpcbData = needed;
                return ERROR_MORE_DATA;
            }
            
            memcpy(lpData, rv.data.data(), needed);
        }
        
        *lpcbData = needed;
    }
    
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI HookRegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    std::wstring path = GetVirtualPath(hKey);
    
    if (!path.empty())
    {
        LogRegistryAccess(L"[REG READ]     ", path.c_str(), lpValueName);
        
        return VirtualQueryValueW(path, lpValueName, lpType, lpData, lpcbData);
    }

    // Log real registry reads
    if (g_logRegistry)
    {
        std::wstring realPath = GetAnyPathFull(hKey);
        
        if (!realPath.empty())
            LogRegistryAccess(L"[REG READ]     ", realPath.c_str(), lpValueName);
    }

    return g_origRegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

static LSTATUS WINAPI HookRegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    std::wstring path = GetVirtualPath(hKey);
    
    if (path.empty())
    {
        // Log real registry reads
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);
            std::wstring wideName = lpValueName ? AnsiToWide(lpValueName) : std::wstring{};
            
            if (!realPath.empty())
                LogRegistryAccess(L"[REG READ]     ", realPath.c_str(), wideName.c_str());
        }
        
        return g_origRegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
    }

    std::wstring wideName = lpValueName ? AnsiToWide(lpValueName) : std::wstring{};
    
    LogRegistryAccess(L"[REG READ]     ", path.c_str(), wideName.c_str());
    
    DWORD type = 0;

    // Query wide first to discover type and data size
    LSTATUS status = VirtualQueryValueW(path, wideName.c_str(), &type, nullptr, nullptr);
    
    if (status != ERROR_SUCCESS)
        return status;
    
    if (lpType)
        *lpType = type;

    if (type == REG_SZ || type == REG_EXPAND_SZ)
    {
        // Convert stored wide data to ANSI
        std::shared_lock lock(g_storeMutex);
        
        auto kit = g_store.find(path);
        
        if (kit == g_store.end())
            return ERROR_FILE_NOT_FOUND;
        
        auto vit = kit->second.find(ToUpper(wideName));
        
        if (vit == kit->second.end())
            return ERROR_FILE_NOT_FOUND;

        const RegValue& registryValue = vit->second;
        const wchar_t* wideRegistryValue = reinterpret_cast<const wchar_t*>(registryValue.data.data());
        int wideRegistryValueLength = static_cast<int>(registryValue.data.size() / sizeof(wchar_t));

        int ansiRegistryValueLength = WideCharToMultiByte(CP_ACP, 0, wideRegistryValue, wideRegistryValueLength, nullptr, 0, nullptr, nullptr);
        
        if (lpcbData)
        {
            if (lpData)
            {
                if (static_cast<int>(*lpcbData) < ansiRegistryValueLength)
                {
                    *lpcbData = static_cast<DWORD>(ansiRegistryValueLength);
                    
                    return ERROR_MORE_DATA;
                }
                
                WideCharToMultiByte(CP_ACP, 0, wideRegistryValue, wideRegistryValueLength, reinterpret_cast<LPSTR>(lpData), ansiRegistryValueLength, nullptr, nullptr);
            }
            
            *lpcbData = static_cast<DWORD>(ansiRegistryValueLength);
        }
        
        return ERROR_SUCCESS;
    }
    else
    {
        // Binary types: pass raw bytes unchanged
        return VirtualQueryValueW(path, wideName.c_str(), lpType, lpData, lpcbData);
    }
}

// --- RegSetValueEx ---
static LSTATUS WINAPI HookRegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD /*Reserved*/,
    DWORD dwType, const BYTE* lpData, DWORD cbData)
{
    std::wstring path = GetVirtualPath(hKey);
    
    if (path.empty())
    {
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);
            
            if (!realPath.empty())
                LogRegistryAccess(L"[REG WRITE]    ", realPath.c_str(), lpValueName);
        }
        
        return g_origRegSetValueExW(hKey, lpValueName, 0, dwType, lpData, cbData);
    }

    LogRegistryAccess(L"[REG WRITE]    ", path.c_str(), lpValueName);

    RegValue registryValue;
    registryValue.type = dwType;
    
    if (lpData && cbData > 0)
        registryValue.data.assign(lpData, lpData + cbData);

    {
        std::unique_lock lock(g_storeMutex);
        
        g_store[path][ToUpper(lpValueName ? lpValueName : L"")] = std::move(registryValue);
        g_dirty = true;
    }
    
    SaveRegFile();
    
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI HookRegSetValueExA(HKEY hKey, LPCSTR lpValueName, DWORD /*Reserved*/,
    DWORD dwType, const BYTE* lpData, DWORD cbData)
{
    std::wstring path  = GetVirtualPath(hKey);
    std::wstring wideRegistyValueName = lpValueName ? AnsiToWide(lpValueName) : std::wstring{};
    if (path.empty())
    {
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);
            
            if (!realPath.empty())
                LogRegistryAccess(L"[REG WRITE]    ", realPath.c_str(), wideRegistyValueName.c_str());
        }
        
        return g_origRegSetValueExA(hKey, lpValueName, 0, dwType, lpData, cbData);
    }

    LogRegistryAccess(L"[REG WRITE]    ", path.c_str(), wideRegistyValueName.c_str());
    
    RegValue registryValue;
    registryValue.type = dwType;

    if ((dwType == REG_SZ || dwType == REG_EXPAND_SZ) && lpData && cbData > 0)
    {
        // Convert ANSI string to wide (cbData typically includes the null terminator)
        int wideLength = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<LPCSTR>(lpData),
            static_cast<int>(cbData), nullptr, 0);
        
        registryValue.data.resize(static_cast<size_t>(wideLength) * sizeof(wchar_t));
        
        MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<LPCSTR>(lpData),
            static_cast<int>(cbData), reinterpret_cast<LPWSTR>(registryValue.data.data()), wideLength);
    }
    else if (lpData && cbData > 0)
    {
        registryValue.data.assign(lpData, lpData + cbData);
    }

    {
        std::unique_lock lock(g_storeMutex);
        g_store[path][ToUpper(wideRegistyValueName)] = std::move(registryValue);
        g_dirty = true;
    }
    
    SaveRegFile();
    
    return ERROR_SUCCESS;
}

// --- RegDeleteValue ---
static LSTATUS WINAPI HookRegDeleteValueW(HKEY hKey, LPCWSTR lpValueName)
{
    std::wstring path = GetVirtualPath(hKey);
    
    if (path.empty())
    {
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);
            if (!realPath.empty())
                LogRegistryAccess(L"[REG DELETE]   ", realPath.c_str(), lpValueName);
        }
        
        return g_origRegDeleteValueW(hKey, lpValueName);
    }

    LogRegistryAccess(L"[REG DELETE]   ", path.c_str(), lpValueName);
    
    std::wstring upperName = ToUpper(lpValueName ? lpValueName : L"");
    {
        std::unique_lock lock(g_storeMutex);
        auto kit = g_store.find(path);
        
        if (kit == g_store.end())
            return ERROR_FILE_NOT_FOUND;
        
        auto vit = kit->second.find(upperName);
        
        if (vit == kit->second.end())
            return ERROR_FILE_NOT_FOUND;
        
        kit->second.erase(vit);
        g_dirty = true;
    }
    
    SaveRegFile();
    
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI HookRegDeleteValueA(HKEY hKey, LPCSTR lpValueName)
{
    // Delegate to W variant which handles both logging and virtual store.
    std::wstring wideRegistryValueName = lpValueName ? AnsiToWide(lpValueName) : std::wstring{};
    
    return HookRegDeleteValueW(hKey, wideRegistryValueName.c_str());
}

// --- RegEnumValue ---
static LSTATUS WINAPI HookRegEnumValueW(HKEY hKey, DWORD dwIndex, LPWSTR lpValueName,
    LPDWORD lpcchValueName, LPDWORD /*lpReserved*/, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    std::wstring path = GetVirtualPath(hKey);

    if (path.empty())
    {
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);
            if (!realPath.empty())
                LogRegistryAccess(L"[REG ENUM]     ", realPath.c_str());
        }
        return g_origRegEnumValueW(hKey, dwIndex, lpValueName, lpcchValueName, nullptr, lpType, lpData, lpcbData);
    }

    std::shared_lock lock(g_storeMutex);
    
    auto kit = g_store.find(path);
    
    if (kit == g_store.end())
        return ERROR_NO_MORE_ITEMS;
    
    if (dwIndex >= static_cast<DWORD>(kit->second.size())) 
        return ERROR_NO_MORE_ITEMS;

    auto iterator = kit->second.begin();
    
    std::advance(iterator, dwIndex);

    const std::wstring& name = iterator->first;
    
    if (lpcchValueName)
    {
        DWORD needed = static_cast<DWORD>(name.size());
        
        if (lpValueName)
        {
            if (*lpcchValueName <= needed)
            {
                *lpcchValueName = needed + 1;
                
                return ERROR_MORE_DATA;
            }
            
            wmemcpy(lpValueName, name.c_str(), name.size() + 1);
        }
        
        *lpcchValueName = needed;
    }

    const RegValue& registryValue = iterator->second;
    
    if (lpType)
        *lpType = registryValue.type;

    if (lpcbData)
    {
        DWORD needed = static_cast<DWORD>(registryValue.data.size());
        
        if (lpData)
        {
            if (*lpcbData < needed)
            {
                *lpcbData = needed;
                
                return ERROR_MORE_DATA;
            }
            
            memcpy(lpData, registryValue.data.data(), needed);
        }
        
        *lpcbData = needed;
    }
    
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI HookRegEnumValueA(HKEY hKey, DWORD dwIndex, LPSTR lpValueName,
    LPDWORD lpcchValueName, LPDWORD /*lpReserved*/, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    std::wstring path = GetVirtualPath(hKey);

    if (path.empty())
    {
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);
            if (!realPath.empty())
                LogRegistryAccess(L"[REG ENUM]     ", realPath.c_str());
        }
        return g_origRegEnumValueA(hKey, dwIndex, lpValueName, lpcchValueName, nullptr, lpType, lpData, lpcbData);
    }

    std::shared_lock lock(g_storeMutex);
    
    auto kit = g_store.find(path);
    
    if (kit == g_store.end())
        return ERROR_NO_MORE_ITEMS;
    
    if (dwIndex >= static_cast<DWORD>(kit->second.size()))
        return ERROR_NO_MORE_ITEMS;

    auto iterator = kit->second.begin();
    
    std::advance(iterator, dwIndex);

    // Convert name to ANSI
    std::string ansiName = WideToAnsi(iterator->first.c_str());
    
    if (lpcchValueName)
    {
        DWORD needed = static_cast<DWORD>(ansiName.size());
        
        if (lpValueName)
        {
            if (*lpcchValueName <= needed)
            {
                *lpcchValueName = needed + 1;
                
                return ERROR_MORE_DATA;
            }
            
            memcpy(lpValueName, ansiName.c_str(), ansiName.size() + 1);
        }
        
        *lpcchValueName = needed;
    }

    const RegValue& registryValue = iterator->second;
    
    if (lpType)
        *lpType = registryValue.type;

    if (registryValue.type == REG_SZ || registryValue.type == REG_EXPAND_SZ)
    {
        const wchar_t* wideRegistryValue = reinterpret_cast<const wchar_t*>(registryValue.data.data());
        
        int wideRegistryValueLength = static_cast<int>(registryValue.data.size() / sizeof(wchar_t));
        
        int ansiRegistryValueLength = WideCharToMultiByte(CP_ACP, 0, wideRegistryValue, wideRegistryValueLength, nullptr, 0, nullptr, nullptr);
        
        if (lpcbData)
        {
            if (lpData)
            {
                if (static_cast<int>(*lpcbData) < ansiRegistryValueLength)
                {
                    *lpcbData = static_cast<DWORD>(ansiRegistryValueLength);
                    
                    return ERROR_MORE_DATA;
                }
                
                WideCharToMultiByte(CP_ACP, 0, wideRegistryValue, wideRegistryValueLength, reinterpret_cast<LPSTR>(lpData), ansiRegistryValueLength, nullptr, nullptr);
            }
            
            *lpcbData = static_cast<DWORD>(ansiRegistryValueLength);
        }
    }
    else
    {
        DWORD needed = static_cast<DWORD>(registryValue.data.size());
        
        if (lpcbData)
        {
            if (lpData)
            {
                if (*lpcbData < needed)
                {
                    *lpcbData = needed;
                    return ERROR_MORE_DATA;
                }
                
                memcpy(lpData, registryValue.data.data(), needed);
            }
            
            *lpcbData = needed;
        }
    }
    
    return ERROR_SUCCESS;
}

// --- RegEnumKeyEx ---
// Returns sorted list of direct child key names under parentPath.
static std::vector<std::wstring> GetDirectChildren(const std::wstring& parentPath)
{
    std::vector<std::wstring> children;
    std::wstring prefix = parentPath + L'\\';

    std::shared_lock lock(g_storeMutex);
    
    for (auto& [key, _] : g_store)
    {
        if (key.size() > prefix.size() &&
            key.compare(0, prefix.size(), prefix) == 0)
        {
            std::wstring rest = key.substr(prefix.size());
            
            if (rest.find(L'\\') == std::wstring::npos) // direct child only
                children.push_back(rest);
        }
    }
    
    return children;
}

static LSTATUS WINAPI HookRegEnumKeyExW(HKEY hKey, DWORD dwIndex, LPWSTR lpName,
    LPDWORD lpcchName, LPDWORD /*lpReserved*/, LPWSTR lpClass, LPDWORD lpcchClass,
    PFILETIME lpftLastWriteTime)
{
    std::wstring path = GetVirtualPath(hKey);

    if (path.empty())
    {
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);
            if (!realPath.empty())
                LogRegistryAccess(L"[REG ENUM]     ", realPath.c_str());
        }
        return g_origRegEnumKeyExW(hKey, dwIndex, lpName, lpcchName, nullptr, lpClass, lpcchClass, lpftLastWriteTime);
    }

    auto children = GetDirectChildren(path);
    
    if (dwIndex >= static_cast<DWORD>(children.size()))
        return ERROR_NO_MORE_ITEMS;

    const std::wstring& child = children[dwIndex];
    
    if (lpcchName)
    {
        DWORD needed = static_cast<DWORD>(child.size());
        
        if (lpName)
        {
            if (*lpcchName <= needed)
            {
                *lpcchName = needed + 1;
                return ERROR_MORE_DATA;
            }
            wmemcpy(lpName, child.c_str(), child.size() + 1);
        }
        
        *lpcchName = needed;
    }
    
    if (lpClass && lpcchClass)
        *lpcchClass = 0;
    if (lpftLastWriteTime)
        *lpftLastWriteTime = {};
    
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI HookRegEnumKeyExA(HKEY hKey, DWORD dwIndex, LPSTR lpName,
    LPDWORD lpcchName, LPDWORD /*lpReserved*/, LPSTR lpClass, LPDWORD lpcchClass,
    PFILETIME lpftLastWriteTime)
{
    std::wstring path = GetVirtualPath(hKey);

    if (path.empty())
    {
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);
            if (!realPath.empty())
                LogRegistryAccess(L"[REG ENUM]     ", realPath.c_str());
        }
        return g_origRegEnumKeyExA(hKey, dwIndex, lpName, lpcchName, nullptr, lpClass, lpcchClass, lpftLastWriteTime);
    }

    auto children = GetDirectChildren(path);
    
    if (dwIndex >= static_cast<DWORD>(children.size()))
        return ERROR_NO_MORE_ITEMS;

    std::string ansiChild = WideToAnsi(children[dwIndex].c_str());
    
    if (lpcchName)
    {
        DWORD needed = static_cast<DWORD>(ansiChild.size());
        
        if (lpName)
        {
            if (*lpcchName <= needed)
            {
                *lpcchName = needed + 1;
                return ERROR_MORE_DATA;
            }
            
            memcpy(lpName, ansiChild.c_str(), ansiChild.size() + 1);
        }
        
        *lpcchName = needed;
    }
    
    if (lpClass && lpcchClass)
        *lpcchClass = 0;
    
    if (lpftLastWriteTime)
        *lpftLastWriteTime = {};
    
    return ERROR_SUCCESS;
}

// --- RegQueryInfoKey ---
static LSTATUS WINAPI HookRegQueryInfoKeyW(HKEY hKey, LPWSTR lpClass, LPDWORD lpcchClass,
    LPDWORD /*lpReserved*/, LPDWORD lpcSubKeys, LPDWORD lpcbMaxSubKeyLen,
    LPDWORD lpcbMaxClassLen, LPDWORD lpcValues, LPDWORD lpcbMaxValueNameLen,
    LPDWORD lpcbMaxValueLen, LPDWORD lpcbSecurityDescriptor, PFILETIME lpftLastWriteTime)
{
    std::wstring path = GetVirtualPath(hKey);

    if (path.empty())
    {
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);
            if (!realPath.empty())
                LogRegistryAccess(L"[REG QUERY]    ", realPath.c_str());
        }
        return g_origRegQueryInfoKeyW(hKey, lpClass, lpcchClass, nullptr, lpcSubKeys,
            lpcbMaxSubKeyLen, lpcbMaxClassLen, lpcValues, lpcbMaxValueNameLen,
            lpcbMaxValueLen, lpcbSecurityDescriptor, lpftLastWriteTime);
    }

    if (lpClass && lpcchClass)
        *lpcchClass = 0;

    auto children = GetDirectChildren(path);
    
    if (lpcSubKeys)
        *lpcSubKeys = static_cast<DWORD>(children.size());
    
    if (lpcbMaxSubKeyLen)
    {
        DWORD maxLength = 0;
        
        for (auto& c : children)
            if (static_cast<DWORD>(c.size()) > maxLength) maxLength = static_cast<DWORD>(c.size());
        
        *lpcbMaxSubKeyLen = maxLength;
    }
    
    if (lpcbMaxClassLen)
        *lpcbMaxClassLen = 0;

    DWORD valueCount = 0, maxValName = 0, maxValData = 0;
    {
        std::shared_lock lock(g_storeMutex);
        
        auto kit = g_store.find(path);
        
        if (kit != g_store.end())
        {
            valueCount = static_cast<DWORD>(kit->second.size());\
            
            for (auto& [name, rv] : kit->second)
            {
                if (static_cast<DWORD>(name.size()) > maxValName)
                    maxValName = static_cast<DWORD>(name.size());
                
                if (static_cast<DWORD>(rv.data.size()) > maxValData)
                    maxValData = static_cast<DWORD>(rv.data.size());
            }
        }
    }
    
    if (lpcValues)
        *lpcValues = valueCount;
    
    if (lpcbMaxValueNameLen)
        *lpcbMaxValueNameLen = maxValName;
    
    if (lpcbMaxValueLen) 
        *lpcbMaxValueLen = maxValData;
    
    if (lpcbSecurityDescriptor)
        *lpcbSecurityDescriptor = 0;
    
    if (lpftLastWriteTime)
        *lpftLastWriteTime = {};
    
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI HookRegQueryInfoKeyA(HKEY hKey, LPSTR lpClass, LPDWORD lpcchClass,
    LPDWORD lpReserved, LPDWORD lpcSubKeys, LPDWORD lpcbMaxSubKeyLen,
    LPDWORD lpcbMaxClassLen, LPDWORD lpcValues, LPDWORD lpcbMaxValueNameLen,
    LPDWORD lpcbMaxValueLen, LPDWORD lpcbSecurityDescriptor, PFILETIME lpftLastWriteTime)
{
    // Delegate to W variant; ignore lpClass / lpcchClass (ANSI class name rarely used)
    return HookRegQueryInfoKeyW(hKey, nullptr, nullptr, lpReserved, lpcSubKeys,
        lpcbMaxSubKeyLen, lpcbMaxClassLen, lpcValues, lpcbMaxValueNameLen,
        lpcbMaxValueLen, lpcbSecurityDescriptor, lpftLastWriteTime);
}

// ============================================================
// Public API
// ============================================================
void InstallRegistryHooks()
{
    // Locate .interposer\Registry.reg next to our DLL
    HMODULE hSelf = nullptr;

    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&InstallRegistryHooks),
        &hSelf);

    wchar_t dllPath[MAX_PATH] = {};

    GetModuleFileNameW(hSelf, dllPath, MAX_PATH);

    std::wstring directory(dllPath);
    auto slash = directory.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        directory.resize(slash + 1);

    std::wstring interposerDir = directory + L".interposer\\";
    CreateDirectoryW(interposerDir.c_str(), nullptr);

    g_filePath = interposerDir + L"Registry.reg";

    LoadRegFile();

    // Install the 17 advapi32 hooks (no MH_Initialize / MH_EnableHook here —
    // those are owned by dllmain.cpp)
    MH_CreateHookApi(L"advapi32", "RegOpenKeyExW",
        reinterpret_cast<LPVOID>(HookRegOpenKeyExW),
        reinterpret_cast<LPVOID*>(&g_origRegOpenKeyExW));
    MH_CreateHookApi(L"advapi32", "RegOpenKeyExA",
        reinterpret_cast<LPVOID>(HookRegOpenKeyExA),
        reinterpret_cast<LPVOID*>(&g_origRegOpenKeyExA));
    MH_CreateHookApi(L"advapi32", "RegCreateKeyExW",
        reinterpret_cast<LPVOID>(HookRegCreateKeyExW),
        reinterpret_cast<LPVOID*>(&g_origRegCreateKeyExW));
    MH_CreateHookApi(L"advapi32", "RegCreateKeyExA",
        reinterpret_cast<LPVOID>(HookRegCreateKeyExA),
        reinterpret_cast<LPVOID*>(&g_origRegCreateKeyExA));
    MH_CreateHookApi(L"advapi32", "RegCloseKey",
        reinterpret_cast<LPVOID>(HookRegCloseKey),
        reinterpret_cast<LPVOID*>(&g_origRegCloseKey));
    MH_CreateHookApi(L"advapi32", "RegQueryValueExW",
        reinterpret_cast<LPVOID>(HookRegQueryValueExW),
        reinterpret_cast<LPVOID*>(&g_origRegQueryValueExW));
    MH_CreateHookApi(L"advapi32", "RegQueryValueExA",
        reinterpret_cast<LPVOID>(HookRegQueryValueExA),
        reinterpret_cast<LPVOID*>(&g_origRegQueryValueExA));
    MH_CreateHookApi(L"advapi32", "RegSetValueExW",
        reinterpret_cast<LPVOID>(HookRegSetValueExW),
        reinterpret_cast<LPVOID*>(&g_origRegSetValueExW));
    MH_CreateHookApi(L"advapi32", "RegSetValueExA",
        reinterpret_cast<LPVOID>(HookRegSetValueExA),
        reinterpret_cast<LPVOID*>(&g_origRegSetValueExA));
    MH_CreateHookApi(L"advapi32", "RegDeleteValueW",
        reinterpret_cast<LPVOID>(HookRegDeleteValueW),
        reinterpret_cast<LPVOID*>(&g_origRegDeleteValueW));
    MH_CreateHookApi(L"advapi32", "RegDeleteValueA",
        reinterpret_cast<LPVOID>(HookRegDeleteValueA),
        reinterpret_cast<LPVOID*>(&g_origRegDeleteValueA));
    MH_CreateHookApi(L"advapi32", "RegEnumValueW",
        reinterpret_cast<LPVOID>(HookRegEnumValueW),
        reinterpret_cast<LPVOID*>(&g_origRegEnumValueW));
    MH_CreateHookApi(L"advapi32", "RegEnumValueA",
        reinterpret_cast<LPVOID>(HookRegEnumValueA),
        reinterpret_cast<LPVOID*>(&g_origRegEnumValueA));
    MH_CreateHookApi(L"advapi32", "RegEnumKeyExW",
        reinterpret_cast<LPVOID>(HookRegEnumKeyExW),
        reinterpret_cast<LPVOID*>(&g_origRegEnumKeyExW));
    MH_CreateHookApi(L"advapi32", "RegEnumKeyExA",
        reinterpret_cast<LPVOID>(HookRegEnumKeyExA),
        reinterpret_cast<LPVOID*>(&g_origRegEnumKeyExA));
    MH_CreateHookApi(L"advapi32", "RegQueryInfoKeyW",
        reinterpret_cast<LPVOID>(HookRegQueryInfoKeyW),
        reinterpret_cast<LPVOID*>(&g_origRegQueryInfoKeyW));
    MH_CreateHookApi(L"advapi32", "RegQueryInfoKeyA",
        reinterpret_cast<LPVOID>(HookRegQueryInfoKeyA),
        reinterpret_cast<LPVOID*>(&g_origRegQueryInfoKeyA));
}

void RemoveRegistryHooks()
{
    // No MinHook teardown here — dllmain.cpp calls MH_DisableHook / MH_Uninitialize.
    // Just flush any pending writes.
    if (g_dirty)
        SaveRegFile();
}
