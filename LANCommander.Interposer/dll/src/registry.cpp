#include "registry.h"
#include "config.h"

#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

// ============================================================
// In-memory store
// ============================================================
struct RegValue { DWORD type; std::vector<BYTE> data; };
using ValueMap = std::map<std::wstring, RegValue>;  // uppercase valueName → value
using StoreMap = std::map<std::wstring, ValueMap>;  // uppercase keyPath  → values

static StoreMap          g_store;
static std::shared_mutex g_storeMutex;
static bool              g_dirty = false;

// The overlay stack in load order, and the one layer of it that is writable.
// Each file is loaded on top of the last, so a later file wins wherever two
// name the same value; g_writeFile is the final entry and the only one this
// process ever writes to.
static std::vector<std::wstring> g_regFiles;
static std::wstring              g_writeFile;

// What the writable layer owns. g_store is the merged view every read is served
// from; g_writable is the subset serialized back out. Keeping them apart is what
// stops a value inherited from a base file being copied into the write file just
// because the game touched the key next to it — the base file stays the shared
// template and the write file stays the per-user delta.
static StoreMap g_writable;

// Values the base layers define, and the deletes recorded against them. Dropping
// a base-layer value from g_writable would not stick: the base file hands it back
// on the next launch. It is written as a regedit `"Name"=-` tombstone instead.
// With a single file — the default — g_baseValues is empty, no tombstone is
// ever produced, and the written file is byte-identical to what it always was.
static std::set<std::pair<std::wstring, std::wstring>> g_baseValues;
static std::map<std::wstring, std::set<std::wstring>>  g_tombstones;

static std::set<std::wstring> g_baseKeys;
static std::set<std::wstring> g_keyTombstones;

static void EraseSubtreeLocked(const std::wstring& keyPath)
{
    const std::wstring prefix = keyPath + L'\\';

    auto covered = [&](const std::wstring& key)
    {
        return key == keyPath || key.compare(0, prefix.size(), prefix) == 0;
    };

    for (auto it = g_store.begin(); it != g_store.end(); )
        it = covered(it->first) ? g_store.erase(it) : std::next(it);

    for (auto it = g_writable.begin(); it != g_writable.end(); )
        it = covered(it->first) ? g_writable.erase(it) : std::next(it);

    for (auto it = g_tombstones.begin(); it != g_tombstones.end(); )
        it = covered(it->first) ? g_tombstones.erase(it) : std::next(it);
}

// Drop a tombstone once the value is written again. Caller holds g_storeMutex.
static void ClearTombstone(const std::wstring& keyPath, const std::wstring& valueName)
{
    auto it = g_tombstones.find(keyPath);

    if (it == g_tombstones.end())
        return;

    it->second.erase(valueName);

    if (it->second.empty())
        g_tombstones.erase(it);
}

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

static std::wstring GetKeyPathViaSystem(HKEY hiveKey, bool rewriteCurrentUser = true);

static const std::wstring& CurrentUserHivePrefix()
{
    static const std::wstring prefix = []() -> std::wstring
    {
        using FnRtlOpenCurrentUser = LONG(NTAPI*)(ULONG, PHANDLE);

        auto openCurrentUser = reinterpret_cast<FnRtlOpenCurrentUser>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlOpenCurrentUser"));

        if (!openCurrentUser)
            return {};

        HANDLE userKey = nullptr;

        if (openCurrentUser(MAXIMUM_ALLOWED, &userKey) != 0 || !userKey)
            return {};

        // rewriteCurrentUser = false is what keeps this out of infinite recursion.
        std::wstring path = GetKeyPathViaSystem(reinterpret_cast<HKEY>(userKey), false);

        CloseHandle(userKey);

        return path;   // HKEY_USERS\<SID>
    }();

    return prefix;
}

// Map HKEY_USERS\<SID>\X onto HKEY_CURRENT_USER\X. Matched on a backslash
// boundary, so the sibling hive HKEY_USERS\<SID>_Classes is left alone.
// Input must already be uppercased.
static std::wstring RewriteCurrentUserHive(std::wstring path)
{
    const std::wstring& prefix = CurrentUserHivePrefix();

    if (prefix.empty() || path.size() < prefix.size() ||
        path.compare(0, prefix.size(), prefix) != 0)
        return path;

    if (path.size() == prefix.size())
        return L"HKEY_CURRENT_USER";

    if (path[prefix.size()] != L'\\')
        return path;

    return L"HKEY_CURRENT_USER" + path.substr(prefix.size());
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
        if (const wchar_t* predefined = PredefinedName(hKey))
            base = predefined;

    if (base.empty())
    {
        base = NormalizeVirtualStore(GetKeyPathViaSystem(hKey));

        if (base.empty())
            return {};
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
static std::wstring GetKeyPathViaSystem(HKEY hiveKey, bool rewriteCurrentUser)
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
            
            std::wstring upper = ToUpper(result);

            return rewriteCurrentUser ? RewriteCurrentUserHive(std::move(upper)) : upper;
        }
    }
    
    return ToUpper(ntPath); // unknown prefix — keep the NT path as-is
}

// Returns the path for any handle (virtual, tracked real, predefined root,
// or untracked/pre-injection real handle), or "" if completely unrecognised.
// Reads as a separate concern from BuildPath -- this one exists for log lines --
// but the two resolve a bare handle identically now that BuildPath has the
// NtQueryKey tier, so it forwards rather than keeping a second copy of the walk.
static std::wstring GetAnyPathFull(HKEY h)
{
    return BuildPath(h, nullptr);
}

// True if upperPath exactly matches, or is an ancestor/descendant of any stored key.
static bool InVirtualSpace(const std::wstring& upperPath)
{
    if (upperPath.empty()) return false;

    // Isolated makes the store authoritative for the whole registry rather than
    // just the keys it happens to contain: a key it has never heard of is still
    // virtual, so the read fails instead of falling through and the write lands
    // in the file instead of in HKLM.
    if (g_registryIsolated) return true;

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

// Emit the Debug-level verdict for a key open/create: whether the request was
// served from the virtual store or handed to the real registry, and why.
// `path` is empty when BuildPath could not resolve the handle.
static void LogVirtualSpaceVerdict(const std::wstring& path, bool inVirtualSpace)
{
    if (!g_logRegistry || g_logLevel < LogLevel::Debug)
        return;

    if (path.empty())
    {
        // BuildPath fell through all three tiers. TrackRealHandle no-ops on an
        // empty path, so every key opened through the resulting handle is also
        // unresolvable — worth flagging rather than silently passing through.
        LogRegistryDiag(L"REG MISS", L"(unknown)", L"handle not resolvable");
    }
    else if (inVirtualSpace)
        LogRegistryDiag(L"REG HIT", path.c_str(),
            g_registryIsolated ? L"served from virtual store (isolated)"
                               : L"served from virtual store");
    else
        LogRegistryDiag(L"REG MISS", path.c_str(), L"not in virtual space");
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
// Load one layer of the overlay on top of whatever is already in g_store.
// writeLayer marks the last file in the stack: its contents are recorded in
// g_writable so SaveRegFile can reproduce them, while an earlier file's are
// recorded in g_baseValues instead and are never written back out.
static void LoadRegFile(const std::wstring& path, bool writeLayer)
{
    if (path.empty())
        return;

    HANDLE fileHandle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
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
                std::wstring header = line.substr(1, end - 1);

                if (!header.empty() && header[0] == L'-')
                {
                    std::wstring doomed = ToUpper(header.substr(1));

                    if (!doomed.empty())
                    {
                        std::unique_lock lk(g_storeMutex);

                        EraseSubtreeLocked(doomed);

                        if (writeLayer)
                            g_keyTombstones.insert(doomed);
                    }

                    currentKey.clear();

                    continue;
                }

                currentKey = ToUpper(header);
                std::unique_lock lk(g_storeMutex);
                g_store.emplace(currentKey, ValueMap{});

                // A bare key header carries meaning -- it is what puts the key
                // in the virtual space -- so the write layer keeps its own.
                if (writeLayer)
                    g_writable.emplace(currentKey, ValueMap{});
                else
                    g_baseKeys.insert(currentKey);
            }

            continue;
        }

        if (currentKey.empty())
            continue;

        // Check whether this line starts a backslash-continued value
        if ((line[0] == L'"' || line[0] == L'@') && !line.empty() && line.back() == L'\\' &&
            line.find(L'=') != std::wstring::npos)
        {
            pending = line;
            pending.pop_back();

            continue;
        }

        // Value entry: "name"=<data>  or  @=<data> (default/unnamed value)
        std::wstring valueName;
        std::wstring valueData;

        if (line[0] == L'@' && line.size() >= 2 && line[1] == L'=')
        {
            // valueName stays empty — that is the default value's key in g_store
            valueData = line.substr(2);
        }
        else if (line[0] == L'"')
        {
            size_t nameEnd = line.find(L'"', 1);

            if (nameEnd == std::wstring::npos)
                continue;

            size_t equalSignPosition = nameEnd + 1;

            if (equalSignPosition >= line.size() || line[equalSignPosition] != L'=')
                continue;

            valueName = ToUpper(line.substr(1, nameEnd - 1));
            valueData = line.substr(equalSignPosition + 1);
        }
        else
        {
            continue;
        }

        // "Name"=- is regedit's delete marker. In an overlay it is how the write
        // layer suppresses a value an earlier file in the stack defines.
        if (valueData == L"-")
        {
            std::unique_lock lock(g_storeMutex);

            if (auto kit = g_store.find(currentKey); kit != g_store.end())
                kit->second.erase(valueName);

            if (auto wit = g_writable.find(currentKey); wit != g_writable.end())
                wit->second.erase(valueName);

            if (writeLayer)
                g_tombstones[currentKey].insert(valueName);

            continue;
        }

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

        if (writeLayer)
        {
            g_writable[currentKey][valueName] = registryValue;
            ClearTombstone(currentKey, valueName);
        }
        else
            g_baseValues.insert({ currentKey, valueName });

        g_store[currentKey][valueName] = std::move(registryValue);
    }
}

// Serialize the writable layer — not the merged store — to the last file in
// the stack. Values inherited from an earlier file are deliberately absent: they
// already live there, and copying them out would freeze a stale duplicate on top
// of the file they came from.
static void SaveRegFile()
{
    if (g_writeFile.empty())
        return;

    // Build the output string while holding a shared lock
    std::wstring output;
    
    output.reserve(4096);
    
    output = L"Windows Registry Editor Version 5.00\r\n";

    {
        std::shared_lock lock(g_storeMutex);

        // A key whose every value came from a base layer still needs a section
        // of its own when one of them was deleted, so the two maps are walked
        // together rather than just g_writable.
        std::set<std::wstring> keys;

        for (const auto& [key, values] : g_writable)
            keys.insert(key);

        for (const auto& [key, names] : g_tombstones)
            keys.insert(key);

        for (const std::wstring& key : g_keyTombstones)
            keys.insert(key);

        for (const std::wstring& key : keys)
        {
            if (g_keyTombstones.count(key))
            {
                output += L"\r\n[-";
                output += key;
                output += L"]\r\n";

                // Nothing of this key survives in this layer, so it gets no
                // section of its own.
                if (!g_writable.count(key) && !g_tombstones.count(key))
                    continue;
            }

            output += L"\r\n[";
            output += key;
            output += L"]\r\n";

            if (auto tombIt = g_tombstones.find(key); tombIt != g_tombstones.end())
            {
                for (const std::wstring& name : tombIt->second)
                {
                    if (name.empty())
                        output += L"@=-\r\n";
                    else
                    {
                        output += L'"';
                        output += name;
                        output += L"\"=-\r\n";
                    }
                }
            }

            auto keyIt = g_writable.find(key);

            if (keyIt == g_writable.end())
                continue;

            for (auto& [name, rv] : keyIt->second)
            {
                if (name.empty())
                    output += L"@=";
                else
                {
                    output += L'"';
                    output += name;
                    output += L"\"=";
                }

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
    HANDLE fileHandle = CreateFileW(g_writeFile.c_str(), GENERIC_WRITE, 0,
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

using FnRegGetValueW            = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
using FnRegGetValueA            = LSTATUS(WINAPI*)(HKEY, LPCSTR,  LPCSTR,  DWORD, LPDWORD, PVOID, LPDWORD);
using FnRegSetKeyValueW         = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPCVOID, DWORD);
using FnRegSetKeyValueA         = LSTATUS(WINAPI*)(HKEY, LPCSTR,  LPCSTR,  DWORD, LPCVOID, DWORD);
using FnRegDeleteKeyValueW      = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR);
using FnRegDeleteKeyValueA      = LSTATUS(WINAPI*)(HKEY, LPCSTR,  LPCSTR);
using FnRegQueryMultipleValuesW = LSTATUS(WINAPI*)(HKEY, PVALENTW, DWORD, LPWSTR, LPDWORD);
using FnRegQueryMultipleValuesA = LSTATUS(WINAPI*)(HKEY, PVALENTA, DWORD, LPSTR,  LPDWORD);
using FnRegDeleteKeyW           = LSTATUS(WINAPI*)(HKEY, LPCWSTR);
using FnRegDeleteKeyA           = LSTATUS(WINAPI*)(HKEY, LPCSTR);
using FnRegDeleteKeyExW         = LSTATUS(WINAPI*)(HKEY, LPCWSTR, REGSAM, DWORD);
using FnRegDeleteKeyExA         = LSTATUS(WINAPI*)(HKEY, LPCSTR,  REGSAM, DWORD);
using FnRegDeleteTreeW          = LSTATUS(WINAPI*)(HKEY, LPCWSTR);
using FnRegDeleteTreeA          = LSTATUS(WINAPI*)(HKEY, LPCSTR);
using FnRegOpenCurrentUser      = LSTATUS(WINAPI*)(REGSAM, PHKEY);
using FnRegOpenUserClassesRoot  = LSTATUS(WINAPI*)(HANDLE, DWORD, REGSAM, PHKEY);
using FnRegFlushKey             = LSTATUS(WINAPI*)(HKEY);
using FnRegNotifyChangeKeyValue = LSTATUS(WINAPI*)(HKEY, BOOL, DWORD, HANDLE, BOOL);
using FnRegOpenKeyTransactedW   = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY, HANDLE, PVOID);
using FnRegOpenKeyTransactedA   = LSTATUS(WINAPI*)(HKEY, LPCSTR,  DWORD, REGSAM, PHKEY, HANDLE, PVOID);
using FnRegCreateKeyTransactedW = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD, HANDLE, PVOID);
using FnRegCreateKeyTransactedA = LSTATUS(WINAPI*)(HKEY, LPCSTR,  DWORD, LPSTR,  DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD, HANDLE, PVOID);
using FnRegDeleteKeyTransactedW = LSTATUS(WINAPI*)(HKEY, LPCWSTR, REGSAM, DWORD, HANDLE, PVOID);
using FnRegDeleteKeyTransactedA = LSTATUS(WINAPI*)(HKEY, LPCSTR,  REGSAM, DWORD, HANDLE, PVOID);
using FnRegCopyTreeW            = LSTATUS(WINAPI*)(HKEY, LPCWSTR, HKEY);

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

static FnRegGetValueW            g_origRegGetValueW            = nullptr;
static FnRegGetValueA            g_origRegGetValueA            = nullptr;
static FnRegSetKeyValueW         g_origRegSetKeyValueW         = nullptr;
static FnRegSetKeyValueA         g_origRegSetKeyValueA         = nullptr;
static FnRegDeleteKeyValueW      g_origRegDeleteKeyValueW      = nullptr;
static FnRegDeleteKeyValueA      g_origRegDeleteKeyValueA      = nullptr;
static FnRegQueryMultipleValuesW g_origRegQueryMultipleValuesW = nullptr;
static FnRegQueryMultipleValuesA g_origRegQueryMultipleValuesA = nullptr;
static FnRegDeleteKeyW           g_origRegDeleteKeyW           = nullptr;
static FnRegDeleteKeyA           g_origRegDeleteKeyA           = nullptr;
static FnRegDeleteKeyExW         g_origRegDeleteKeyExW         = nullptr;
static FnRegDeleteKeyExA         g_origRegDeleteKeyExA         = nullptr;
static FnRegDeleteTreeW          g_origRegDeleteTreeW          = nullptr;
static FnRegDeleteTreeA          g_origRegDeleteTreeA          = nullptr;
static FnRegOpenCurrentUser      g_origRegOpenCurrentUser      = nullptr;
static FnRegOpenUserClassesRoot  g_origRegOpenUserClassesRoot  = nullptr;
static FnRegFlushKey             g_origRegFlushKey             = nullptr;
static FnRegNotifyChangeKeyValue g_origRegNotifyChangeKeyValue = nullptr;
static FnRegOpenKeyTransactedW   g_origRegOpenKeyTransactedW   = nullptr;
static FnRegOpenKeyTransactedA   g_origRegOpenKeyTransactedA   = nullptr;
static FnRegCreateKeyTransactedW g_origRegCreateKeyTransactedW = nullptr;
static FnRegCreateKeyTransactedA g_origRegCreateKeyTransactedA = nullptr;
static FnRegDeleteKeyTransactedW g_origRegDeleteKeyTransactedW = nullptr;
static FnRegDeleteKeyTransactedA g_origRegDeleteKeyTransactedA = nullptr;
static FnRegCopyTreeW            g_origRegCopyTreeW            = nullptr;

// ============================================================
// Virtual store operations
// ============================================================
static LSTATUS CopyOutLocked(const RegValue& rv, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
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

static const RegValue* FindValueLocked(const std::wstring& keyPath, const std::wstring& upperName)
{
    auto kit = g_store.find(keyPath);

    if (kit == g_store.end())
    {
        LogRegistryDiag(L"REG MISS", keyPath.c_str(), L"virtual key not in store");

        return nullptr;
    }

    auto vit = kit->second.find(upperName);

    if (vit == kit->second.end())
    {
        LogRegistryDiag(L"REG PARTIAL", keyPath.c_str(),
            (L"value not in store: " + (upperName.empty() ? std::wstring(L"(default)") : upperName)).c_str());

        return nullptr;
    }

    return &vit->second;
}

// --- RegQueryValueEx (shared core) ---
static LSTATUS VirtualQueryValueW(const std::wstring& keyPath, LPCWSTR lpValueName,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    std::wstring upperName = ToUpper(lpValueName ? lpValueName : L"");

    std::shared_lock lock(g_storeMutex);

    const RegValue* rv = FindValueLocked(keyPath, upperName);

    if (!rv)
        return ERROR_FILE_NOT_FOUND;

    return CopyOutLocked(*rv, lpType, lpData, lpcbData);
}

static LSTATUS VirtualQueryValueA(const std::wstring& keyPath, LPCWSTR wideValueName,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    std::wstring upperName = ToUpper(wideValueName ? wideValueName : L"");

    std::shared_lock lock(g_storeMutex);

    const RegValue* rv = FindValueLocked(keyPath, upperName);

    if (!rv)
        return ERROR_FILE_NOT_FOUND;

    // Binary types: pass raw bytes unchanged.
    if (rv->type != REG_SZ && rv->type != REG_EXPAND_SZ)
        return CopyOutLocked(*rv, lpType, lpData, lpcbData);

    if (lpType)
        *lpType = rv->type;

    const wchar_t* wideRegistryValue = reinterpret_cast<const wchar_t*>(rv->data.data());
    int wideRegistryValueLength      = static_cast<int>(rv->data.size() / sizeof(wchar_t));

    int ansiRegistryValueLength = WideCharToMultiByte(CP_ACP, 0, wideRegistryValue,
        wideRegistryValueLength, nullptr, 0, nullptr, nullptr);

    if (lpcbData)
    {
        if (lpData)
        {
            if (static_cast<int>(*lpcbData) < ansiRegistryValueLength)
            {
                *lpcbData = static_cast<DWORD>(ansiRegistryValueLength);

                return ERROR_MORE_DATA;
            }

            WideCharToMultiByte(CP_ACP, 0, wideRegistryValue, wideRegistryValueLength,
                reinterpret_cast<LPSTR>(lpData), ansiRegistryValueLength, nullptr, nullptr);
        }

        *lpcbData = static_cast<DWORD>(ansiRegistryValueLength);
    }

    return ERROR_SUCCESS;
}

static RegValue MakeRegValueFromAnsi(DWORD dwType, const BYTE* lpData, DWORD cbData)
{
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
        registryValue.data.assign(lpData, lpData + cbData);

    return registryValue;
}

static std::vector<BYTE> WideBytesToAnsiBytes(const std::vector<BYTE>& wideBytes)
{
    int characterCount = static_cast<int>(wideBytes.size() / sizeof(wchar_t));

    if (characterCount <= 0)
        return {};

    const wchar_t* wide = reinterpret_cast<const wchar_t*>(wideBytes.data());

    int ansiLength = WideCharToMultiByte(CP_ACP, 0, wide, characterCount,
        nullptr, 0, nullptr, nullptr);

    std::vector<BYTE> ansiBytes(static_cast<size_t>(ansiLength < 0 ? 0 : ansiLength));

    if (ansiLength > 0)
        WideCharToMultiByte(CP_ACP, 0, wide, characterCount,
            reinterpret_cast<LPSTR>(ansiBytes.data()), ansiLength, nullptr, nullptr);

    return ansiBytes;
}

// --- RegGetValue (shared core) ---
static DWORD RrfBitForType(DWORD type)
{
    switch (type)
    {
        case REG_NONE:        return RRF_RT_REG_NONE;
        case REG_SZ:          return RRF_RT_REG_SZ;
        case REG_EXPAND_SZ:   return RRF_RT_REG_EXPAND_SZ;
        case REG_BINARY:      return RRF_RT_REG_BINARY;
        case REG_DWORD:       return RRF_RT_REG_DWORD;
        case REG_MULTI_SZ:    return RRF_RT_REG_MULTI_SZ;
        case REG_QWORD:       return RRF_RT_REG_QWORD;
        default:              return 0;
    }
}

static LSTATUS VirtualResolveGetValue(const std::wstring& keyPath, LPCWSTR lpValue,
    DWORD dwFlags, DWORD& outType, std::vector<BYTE>& outData)
{
    if ((dwFlags & RRF_RT_ANY) == RRF_RT_REG_EXPAND_SZ && !(dwFlags & RRF_NOEXPAND))
        return ERROR_INVALID_PARAMETER;

    DWORD storedType = REG_NONE;
    {
        std::wstring upperName = ToUpper(lpValue ? lpValue : L"");

        std::shared_lock lock(g_storeMutex);

        const RegValue* rv = FindValueLocked(keyPath, upperName);

        if (!rv)
            return ERROR_FILE_NOT_FOUND;

        storedType = rv->type;
        outData    = rv->data;
    }

    const DWORD typeFilter = dwFlags & RRF_RT_ANY;

    if (typeFilter != 0 && typeFilter != RRF_RT_ANY &&
        (typeFilter & RrfBitForType(storedType)) == 0)
        return ERROR_UNSUPPORTED_TYPE;

    outType = storedType;

    if (storedType == REG_EXPAND_SZ && !(dwFlags & RRF_NOEXPAND))
    {
        std::wstring unexpanded(reinterpret_cast<const wchar_t*>(outData.data()),
            outData.size() / sizeof(wchar_t));

        // Trim at the stored terminator so it is not expanded into the middle of
        // the result.
        if (size_t nul = unexpanded.find(L'\0'); nul != std::wstring::npos)
            unexpanded.resize(nul);

        DWORD needed = ExpandEnvironmentStringsW(unexpanded.c_str(), nullptr, 0);

        if (needed > 0)
        {
            std::wstring expanded(needed, L'\0');

            if (ExpandEnvironmentStringsW(unexpanded.c_str(), expanded.data(), needed) == needed)
            {
                expanded.resize(needed - 1);   // drop the counted terminator

                const BYTE* bytes = reinterpret_cast<const BYTE*>(expanded.c_str());

                outData.assign(bytes, bytes + (expanded.size() + 1) * sizeof(wchar_t));

                // Windows reports the *expanded* type, and callers key off it.
                outType = REG_SZ;
            }
        }
    }

    if (outType == REG_SZ || outType == REG_EXPAND_SZ)
    {
        size_t characterCount = outData.size() / sizeof(wchar_t);
        const wchar_t* wide   = reinterpret_cast<const wchar_t*>(outData.data());

        if (characterCount == 0 || wide[characterCount - 1] != L'\0')
            outData.insert(outData.end(), sizeof(wchar_t), 0);
    }
    else if (outType == REG_MULTI_SZ)
    {
        // A multi-string ends on two terminators, so this may need both.
        for (;;)
        {
            size_t characterCount = outData.size() / sizeof(wchar_t);
            const wchar_t* wide   = reinterpret_cast<const wchar_t*>(outData.data());

            if (characterCount >= 2 && wide[characterCount - 1] == L'\0'
                                    && wide[characterCount - 2] == L'\0')
                break;

            outData.insert(outData.end(), sizeof(wchar_t), 0);
        }
    }

    return ERROR_SUCCESS;
}

static LSTATUS GetValueCopyOut(const std::vector<BYTE>& data, PVOID pvData, LPDWORD pcbData)
{
    DWORD needed = static_cast<DWORD>(data.size());

    if (!pvData)
    {
        if (pcbData)
            *pcbData = needed;

        return ERROR_SUCCESS;
    }

    if (!pcbData)
        return ERROR_INVALID_PARAMETER;

    if (*pcbData < needed)
    {
        *pcbData = needed;

        return ERROR_MORE_DATA;
    }

    memcpy(pvData, data.data(), needed);

    *pcbData = needed;

    return ERROR_SUCCESS;
}

static bool HasDescendantsLocked(const std::wstring& keyPath)
{
    const std::wstring prefix = keyPath + L'\\';

    auto it = g_store.lower_bound(prefix);

    return it != g_store.end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

// --- RegDeleteKey / RegDeleteKeyEx (shared core) ---
static LSTATUS VirtualDeleteKeyPath(const std::wstring& path)
{
    std::unique_lock lock(g_storeMutex);

    if (HasDescendantsLocked(path))
    {
        LogRegistryDiag(L"REG PARTIAL", path.c_str(), L"key has subkeys");

        return ERROR_ACCESS_DENIED;
    }

    if (!g_store.count(path))
    {
        LogRegistryDiag(L"REG MISS", path.c_str(), L"virtual key not in store");

        return ERROR_FILE_NOT_FOUND;
    }

    g_store.erase(path);
    g_writable.erase(path);
    g_tombstones.erase(path);   // subsumed by the key tombstone

    if (g_baseKeys.count(path))
        g_keyTombstones.insert(path);

    g_dirty = true;

    return ERROR_SUCCESS;
}

// --- RegDeleteTree (shared core) ---
static LSTATUS VirtualDeleteTree(const std::wstring& path, bool deleteRoot)
{
    std::unique_lock lock(g_storeMutex);

    if (deleteRoot && !g_store.count(path) && !HasDescendantsLocked(path))
    {
        LogRegistryDiag(L"REG MISS", path.c_str(), L"virtual key not in store");

        return ERROR_FILE_NOT_FOUND;
    }

    if (!deleteRoot)
    {
        if (auto kit = g_store.find(path); kit != g_store.end())
        {
            for (const auto& [name, rv] : kit->second)
                if (g_baseValues.count({ path, name }))
                    g_tombstones[path].insert(name);

            kit->second.clear();
        }

        g_writable[path].clear();
    }

    std::vector<std::wstring> victims;

    if (deleteRoot)
        victims.push_back(path);

    const std::wstring prefix = path + L'\\';

    for (const auto& [key, values] : g_store)
        if (key.compare(0, prefix.size(), prefix) == 0)
            victims.push_back(key);

    for (const std::wstring& victim : victims)
    {
        g_store.erase(victim);
        g_writable.erase(victim);
        g_tombstones.erase(victim);

        if (g_baseKeys.count(victim))
            g_keyTombstones.insert(victim);
    }

    g_dirty = true;

    return ERROR_SUCCESS;
}

// --- RegCopyTree (shared core) ---
using SubtreeSnapshot = std::vector<std::pair<std::wstring, ValueMap>>;

static constexpr size_t kCopyTreeMaxNodes = 4096;

static SubtreeSnapshot SnapshotVirtualSubtree(const std::wstring& rootPath)
{
    SubtreeSnapshot snapshot;

    const std::wstring prefix = rootPath + L'\\';

    std::shared_lock lock(g_storeMutex);

    if (auto it = g_store.find(rootPath); it != g_store.end())
        snapshot.emplace_back(std::wstring{}, it->second);

    for (const auto& [key, values] : g_store)
    {
        if (snapshot.size() >= kCopyTreeMaxNodes)
            break;

        if (key.compare(0, prefix.size(), prefix) == 0)
            snapshot.emplace_back(key.substr(prefix.size()), values);
    }

    return snapshot;
}

static SubtreeSnapshot SnapshotRealSubtree(HKEY hRoot)
{
    SubtreeSnapshot snapshot;

    std::vector<std::pair<HKEY, std::wstring>> pending{ { hRoot, std::wstring{} } };

    while (!pending.empty() && snapshot.size() < kCopyTreeMaxNodes)
    {
        auto [hKey, relativePath] = pending.back();
        pending.pop_back();

        ValueMap values;

        for (DWORD index = 0; ; ++index)
        {
            wchar_t name[16384];
            DWORD   nameLength = ARRAYSIZE(name);
            DWORD   type = 0, dataSize = 0;

            LSTATUS status = g_origRegEnumValueW(hKey, index, name, &nameLength,
                nullptr, &type, nullptr, &dataSize);

            if (status != ERROR_SUCCESS)
                break;

            RegValue registryValue;
            registryValue.type = type;
            registryValue.data.resize(dataSize);

            if (g_origRegQueryValueExW(hKey, name, nullptr, &type,
                    dataSize ? registryValue.data.data() : nullptr, &dataSize) == ERROR_SUCCESS)
            {
                registryValue.type = type;
                registryValue.data.resize(dataSize);

                values[ToUpper(name)] = std::move(registryValue);
            }
        }

        snapshot.emplace_back(relativePath, std::move(values));

        for (DWORD index = 0; ; ++index)
        {
            wchar_t childName[256];
            DWORD   childLength = ARRAYSIZE(childName);

            if (g_origRegEnumKeyExW(hKey, index, childName, &childLength,
                    nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;

            HKEY hChild = nullptr;

            if (g_origRegOpenKeyExW(hKey, childName, 0, KEY_READ, &hChild) == ERROR_SUCCESS)
                pending.emplace_back(hChild,
                    relativePath.empty() ? std::wstring(childName)
                                         : relativePath + L'\\' + childName);
        }

        // hRoot belongs to the caller; every other handle here is one we opened.
        if (hKey != hRoot)
            g_origRegCloseKey(hKey);
    }

    for (auto& [hKey, unused] : pending)
        if (hKey != hRoot)
            g_origRegCloseKey(hKey);

    return snapshot;
}

static void ApplyVirtualSubtree(const std::wstring& destPath, const SubtreeSnapshot& snapshot)
{
    std::unique_lock lock(g_storeMutex);

    for (const auto& [relativePath, values] : snapshot)
    {
        std::wstring keyPath = relativePath.empty() ? destPath
                                                    : destPath + L'\\' + relativePath;

        g_store.emplace(keyPath, ValueMap{});
        g_writable.emplace(keyPath, ValueMap{});

        for (const auto& [name, registryValue] : values)
        {
            g_store[keyPath][name]    = registryValue;
            g_writable[keyPath][name] = registryValue;

            ClearTombstone(keyPath, name);
        }
    }

    g_dirty = true;
}

static LSTATUS ApplyRealSubtree(HKEY hDest, const SubtreeSnapshot& snapshot)
{
    for (const auto& [relativePath, values] : snapshot)
    {
        HKEY hKey = hDest;

        if (!relativePath.empty() &&
            g_origRegCreateKeyExW(hDest, relativePath.c_str(), 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
            continue;

        for (const auto& [name, registryValue] : values)
            g_origRegSetValueExW(hKey, name.c_str(), 0, registryValue.type,
                registryValue.data.empty() ? nullptr : registryValue.data.data(),
                static_cast<DWORD>(registryValue.data.size()));

        if (hKey != hDest)
            g_origRegCloseKey(hKey);
    }

    return ERROR_SUCCESS;
}

// --- RegOpenKeyEx (shared core) ---
// The key need not be in the store: being inside the virtual space is what makes
// the open succeed, which is how a game finds a key it created at runtime.
static LSTATUS VirtualOpenKey(const std::wstring& path, PHKEY phkResult)
{
    if (phkResult)
        *phkResult = NewVirtHandle(path);

    return ERROR_SUCCESS;
}

// --- RegCreateKeyEx (shared core) ---
static LSTATUS VirtualCreateKey(const std::wstring& path, PHKEY phkResult, LPDWORD lpdwDisposition)
{
    DWORD disposition;
    {
        std::unique_lock lk(g_storeMutex);
        bool existed     = g_store.count(path) > 0;
        g_store.emplace(path, ValueMap{});
        disposition      = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;

        // A key the game invented belongs to the writable layer even before
        // a value lands in it, so an empty key still round-trips.
        if (!existed)
            g_writable.emplace(path, ValueMap{});
    }

    if (phkResult)
        *phkResult = NewVirtHandle(path);

    if (lpdwDisposition)
        *lpdwDisposition = disposition;

    return ERROR_SUCCESS;
}

// --- RegSetValueEx (shared core) ---
static void VirtualSetValueW(const std::wstring& path, LPCWSTR lpValueName, RegValue registryValue)
{
    std::wstring upperName = ToUpper(lpValueName ? lpValueName : L"");

    std::unique_lock lock(g_storeMutex);

    g_writable[path][upperName] = registryValue;
    g_store[path][upperName]    = std::move(registryValue);

    ClearTombstone(path, upperName);

    g_dirty = true;
}

// --- RegDeleteValue (shared core) ---
static LSTATUS VirtualDeleteValueW(const std::wstring& path, LPCWSTR lpValueName)
{
    std::wstring upperName = ToUpper(lpValueName ? lpValueName : L"");

    std::unique_lock lock(g_storeMutex);

    auto kit = g_store.find(path);

    if (kit == g_store.end())
    {
        LogRegistryDiag(L"REG MISS", path.c_str(), L"virtual key not in store");

        return ERROR_FILE_NOT_FOUND;
    }

    auto vit = kit->second.find(upperName);

    if (vit == kit->second.end())
    {
        LogRegistryDiag(L"REG PARTIAL", path.c_str(),
            (L"value not in store: " + (upperName.empty() ? std::wstring(L"(default)") : upperName)).c_str());

        return ERROR_FILE_NOT_FOUND;
    }

    kit->second.erase(vit);

    if (auto wit = g_writable.find(path); wit != g_writable.end())
        wit->second.erase(upperName);

    if (g_baseValues.count({ path, upperName }))
        g_tombstones[path].insert(upperName);

    g_dirty = true;

    return ERROR_SUCCESS;
}

// ============================================================
// Hook implementations
// ============================================================

// --- RegOpenKeyEx ---
static LSTATUS WINAPI HookRegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
    REGSAM samDesired, PHKEY phkResult)
{
    std::wstring path = BuildPath(hKey, lpSubKey);
    
    LogRegistryAccess(L"REG OPEN", path.empty() ? L"(unknown)" : path.c_str());

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogVirtualSpaceVerdict(path, virtualSpace);

    if (virtualSpace)
        return VirtualOpenKey(path, phkResult);

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
    LogRegistryAccess(L"REG OPEN", path.empty() ? L"(unknown)" : path.c_str());

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogVirtualSpaceVerdict(path, virtualSpace);

    if (virtualSpace)
        return VirtualOpenKey(path, phkResult);

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
    
    LogRegistryAccess(L"REG CREATE", path.empty() ? L"(unknown)" : path.c_str());

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogVirtualSpaceVerdict(path, virtualSpace);

    if (virtualSpace)
        return VirtualCreateKey(path, phkResult, lpdwDisposition);

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
    
    LogRegistryAccess(L"REG CREATE", path.empty() ? L"(unknown)" : path.c_str());

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogVirtualSpaceVerdict(path, virtualSpace);

    if (virtualSpace)
        return VirtualCreateKey(path, phkResult, lpdwDisposition);

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

static LSTATUS WINAPI HookRegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    std::wstring path = GetVirtualPath(hKey);

    if (!path.empty())
    {
        LogRegistryAccess(L"REG READ", path.c_str(), lpValueName);

        return VirtualQueryValueW(path, lpValueName, lpType, lpData, lpcbData);
    }

    // Log real registry reads
    if (g_logRegistry)
    {
        std::wstring realPath = GetAnyPathFull(hKey);

        if (!realPath.empty())
            LogRegistryAccess(L"REG READ", realPath.c_str(), lpValueName);
    }

    return g_origRegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

static LSTATUS WINAPI HookRegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    std::wstring path     = GetVirtualPath(hKey);
    std::wstring wideName = lpValueName ? AnsiToWide(lpValueName) : std::wstring{};

    if (path.empty())
    {
        // Log real registry reads
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);

            if (!realPath.empty())
                LogRegistryAccess(L"REG READ", realPath.c_str(), wideName.c_str());
        }

        return g_origRegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
    }

    LogRegistryAccess(L"REG READ", path.c_str(), wideName.c_str());

    return VirtualQueryValueA(path, wideName.c_str(), lpType, lpData, lpcbData);
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
                LogRegistryAccess(L"REG WRITE", realPath.c_str(), lpValueName);
        }

        return g_origRegSetValueExW(hKey, lpValueName, 0, dwType, lpData, cbData);
    }

    LogRegistryAccess(L"REG WRITE", path.c_str(), lpValueName);

    RegValue registryValue;
    registryValue.type = dwType;

    if (lpData && cbData > 0)
        registryValue.data.assign(lpData, lpData + cbData);

    VirtualSetValueW(path, lpValueName, std::move(registryValue));

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
                LogRegistryAccess(L"REG WRITE", realPath.c_str(), wideRegistyValueName.c_str());
        }

        return g_origRegSetValueExA(hKey, lpValueName, 0, dwType, lpData, cbData);
    }

    LogRegistryAccess(L"REG WRITE", path.c_str(), wideRegistyValueName.c_str());

    VirtualSetValueW(path, wideRegistyValueName.c_str(),
        MakeRegValueFromAnsi(dwType, lpData, cbData));

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
                LogRegistryAccess(L"REG DELETE", realPath.c_str(), lpValueName);
        }

        return g_origRegDeleteValueW(hKey, lpValueName);
    }

    LogRegistryAccess(L"REG DELETE", path.c_str(), lpValueName);

    LSTATUS status = VirtualDeleteValueW(path, lpValueName);

    if (status == ERROR_SUCCESS)
        SaveRegFile();

    return status;
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
                LogRegistryAccess(L"REG ENUM", realPath.c_str());
        }
        return g_origRegEnumValueW(hKey, dwIndex, lpValueName, lpcchValueName, nullptr, lpType, lpData, lpcbData);
    }

    std::shared_lock lock(g_storeMutex);
    
    auto kit = g_store.find(path);

    if (kit == g_store.end())
    {
        LogRegistryDiag(L"REG MISS", path.c_str(), L"virtual key not in store");

        return ERROR_NO_MORE_ITEMS;
    }

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
                LogRegistryAccess(L"REG ENUM", realPath.c_str());
        }
        return g_origRegEnumValueA(hKey, dwIndex, lpValueName, lpcchValueName, nullptr, lpType, lpData, lpcbData);
    }

    std::shared_lock lock(g_storeMutex);
    
    auto kit = g_store.find(path);

    if (kit == g_store.end())
    {
        LogRegistryDiag(L"REG MISS", path.c_str(), L"virtual key not in store");

        return ERROR_NO_MORE_ITEMS;
    }

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
                LogRegistryAccess(L"REG ENUM", realPath.c_str());
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
                LogRegistryAccess(L"REG ENUM", realPath.c_str());
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
                LogRegistryAccess(L"REG QUERY", realPath.c_str());
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
        
        if (kit == g_store.end())
            LogRegistryDiag(L"REG MISS", path.c_str(), L"virtual key not in store");
        else
        {
            valueCount = static_cast<DWORD>(kit->second.size());

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
// Composite hooks
// ============================================================
// Each of these takes hKey + lpSubKey and opens the key through an internal
// entry point (RegOpenKeyExInternalW, RegCreateKeyExInternalW) that is not the
// exported function we hook -- so the nested leaf call arrives with a real
// handle we never tracked, and without a hook here the whole operation lands in
// the real registry. Where the passthrough branch *does* reach a leaf hook, the
// composite stays silent and lets that hook do the logging, so one call by the
// game is one line in the log.

// --- RegGetValue ---
static LSTATUS WINAPI HookRegGetValueW(HKEY hkey, LPCWSTR lpSubKey, LPCWSTR lpValue,
    DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
{
    std::wstring path = BuildPath(hkey, lpSubKey);

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegGetValueW(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);

    LogRegistryAccess(L"REG READ", path.c_str(), lpValue);

    // Captured before the copy-out, which overwrites *pcbData with the required
    // size -- zeroing against that instead would run past the caller's buffer.
    DWORD callerSize = (pvData && pcbData) ? *pcbData : 0;

    DWORD             type = REG_NONE;
    std::vector<BYTE> data;

    LSTATUS status = VirtualResolveGetValue(path, lpValue, dwFlags, type, data);

    if (status == ERROR_SUCCESS)
        status = GetValueCopyOut(data, pvData, pcbData);

    // Windows reports the type even when the buffer was too small.
    if (pdwType && (status == ERROR_SUCCESS || status == ERROR_MORE_DATA))
        *pdwType = type;

    // Handled in one place so that no early return above can miss it.
    if (status != ERROR_SUCCESS && (dwFlags & RRF_ZEROONFAILURE) && pvData)
        memset(pvData, 0, callerSize);

    return status;
}

static LSTATUS WINAPI HookRegGetValueA(HKEY hkey, LPCSTR lpSubKey, LPCSTR lpValue,
    DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
{
    std::wstring wideSubKey = lpSubKey ? AnsiToWide(lpSubKey) : std::wstring{};
    std::wstring wideValue  = lpValue  ? AnsiToWide(lpValue)  : std::wstring{};

    std::wstring path = BuildPath(hkey, wideSubKey.empty() ? nullptr : wideSubKey.c_str());

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegGetValueA(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);

    LogRegistryAccess(L"REG READ", path.c_str(), wideValue.c_str());

    DWORD callerSize = (pvData && pcbData) ? *pcbData : 0;

    DWORD             type = REG_NONE;
    std::vector<BYTE> data;

    LSTATUS status = VirtualResolveGetValue(path, wideValue.c_str(), dwFlags, type, data);

    if (status == ERROR_SUCCESS)
    {
        if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)
            data = WideBytesToAnsiBytes(data);

        status = GetValueCopyOut(data, pvData, pcbData);
    }

    if (pdwType && (status == ERROR_SUCCESS || status == ERROR_MORE_DATA))
        *pdwType = type;

    if (status != ERROR_SUCCESS && (dwFlags & RRF_ZEROONFAILURE) && pvData)
        memset(pvData, 0, callerSize);

    return status;
}

// --- RegSetKeyValue ---
static LSTATUS WINAPI HookRegSetKeyValueW(HKEY hKey, LPCWSTR lpSubKey, LPCWSTR lpValueName,
    DWORD dwType, LPCVOID lpData, DWORD cbData)
{
    std::wstring path = BuildPath(hKey, lpSubKey);

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegSetKeyValueW(hKey, lpSubKey, lpValueName, dwType, lpData, cbData);

    LogRegistryAccess(L"REG WRITE", path.c_str(), lpValueName);

    // The API creates the key when it is missing, so the create belongs here too.
    VirtualCreateKey(path, nullptr, nullptr);

    RegValue registryValue;
    registryValue.type = dwType;

    if (lpData && cbData > 0)
    {
        const BYTE* bytes = static_cast<const BYTE*>(lpData);

        registryValue.data.assign(bytes, bytes + cbData);
    }

    VirtualSetValueW(path, lpValueName, std::move(registryValue));

    SaveRegFile();

    return ERROR_SUCCESS;
}

static LSTATUS WINAPI HookRegSetKeyValueA(HKEY hKey, LPCSTR lpSubKey, LPCSTR lpValueName,
    DWORD dwType, LPCVOID lpData, DWORD cbData)
{
    std::wstring wideSubKey   = lpSubKey    ? AnsiToWide(lpSubKey)    : std::wstring{};
    std::wstring wideValueName = lpValueName ? AnsiToWide(lpValueName) : std::wstring{};

    std::wstring path = BuildPath(hKey, wideSubKey.empty() ? nullptr : wideSubKey.c_str());

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegSetKeyValueA(hKey, lpSubKey, lpValueName, dwType, lpData, cbData);

    LogRegistryAccess(L"REG WRITE", path.c_str(), wideValueName.c_str());

    VirtualCreateKey(path, nullptr, nullptr);

    VirtualSetValueW(path, wideValueName.c_str(),
        MakeRegValueFromAnsi(dwType, static_cast<const BYTE*>(lpData), cbData));

    SaveRegFile();

    return ERROR_SUCCESS;
}

// --- RegDeleteKeyValue ---
static LSTATUS WINAPI HookRegDeleteKeyValueW(HKEY hKey, LPCWSTR lpSubKey, LPCWSTR lpValueName)
{
    std::wstring path = BuildPath(hKey, lpSubKey);

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegDeleteKeyValueW(hKey, lpSubKey, lpValueName);

    LogRegistryAccess(L"REG DELETE", path.c_str(), lpValueName);

    LSTATUS status = VirtualDeleteValueW(path, lpValueName);

    if (status == ERROR_SUCCESS)
        SaveRegFile();

    return status;
}

static LSTATUS WINAPI HookRegDeleteKeyValueA(HKEY hKey, LPCSTR lpSubKey, LPCSTR lpValueName)
{
    std::wstring wideSubKey    = lpSubKey    ? AnsiToWide(lpSubKey)    : std::wstring{};
    std::wstring wideValueName = lpValueName ? AnsiToWide(lpValueName) : std::wstring{};

    return HookRegDeleteKeyValueW(hKey,
        wideSubKey.empty() ? nullptr : wideSubKey.c_str(),
        lpValueName ? wideValueName.c_str() : nullptr);
}

// --- RegQueryMultipleValues ---
static LSTATUS WINAPI HookRegQueryMultipleValuesW(HKEY hKey, PVALENTW val_list, DWORD num_vals,
    LPWSTR lpValueBuf, LPDWORD ldwTotsize)
{
    std::wstring path = GetVirtualPath(hKey);

    if (path.empty())
    {
        // Nothing nested fires on the real path -- the implementation is
        // NtQueryMultipleValueKey -- so this branch logs for itself.
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);

            if (!realPath.empty())
                for (DWORD i = 0; val_list && i < num_vals; ++i)
                    LogRegistryAccess(L"REG READ", realPath.c_str(), val_list[i].ve_valuename);
        }

        return g_origRegQueryMultipleValuesW(hKey, val_list, num_vals, lpValueBuf, ldwTotsize);
    }

    if (!ldwTotsize)
        return ERROR_INVALID_PARAMETER;

    if (num_vals == 0)
    {
        *ldwTotsize = 0;

        return ERROR_SUCCESS;
    }

    if (!val_list)
        return ERROR_INVALID_PARAMETER;

    for (DWORD i = 0; i < num_vals; ++i)
        LogRegistryAccess(L"REG READ", path.c_str(), val_list[i].ve_valuename);

    std::vector<std::vector<BYTE>> payloads(num_vals);
    std::vector<DWORD>             types(num_vals, REG_NONE);
    DWORD                          total = 0;

    {
        std::shared_lock lock(g_storeMutex);

        for (DWORD i = 0; i < num_vals; ++i)
        {
            std::wstring upperName = ToUpper(val_list[i].ve_valuename
                ? val_list[i].ve_valuename : L"");

            const RegValue* rv = FindValueLocked(path, upperName);

            if (!rv)
                return ERROR_FILE_NOT_FOUND;

            types[i]    = rv->type;
            payloads[i] = rv->data;
            total      += static_cast<DWORD>(rv->data.size());
        }
    }

    if (!lpValueBuf || *ldwTotsize < total)
    {
        *ldwTotsize = total;

        return ERROR_MORE_DATA;
    }

    BYTE* cursor = reinterpret_cast<BYTE*>(lpValueBuf);

    for (DWORD i = 0; i < num_vals; ++i)
    {
        memcpy(cursor, payloads[i].data(), payloads[i].size());

        val_list[i].ve_valuelen = static_cast<DWORD>(payloads[i].size());
        val_list[i].ve_valueptr = reinterpret_cast<DWORD_PTR>(cursor);
        val_list[i].ve_type     = types[i];

        cursor += payloads[i].size();
    }

    *ldwTotsize = total;

    return ERROR_SUCCESS;
}

static LSTATUS WINAPI HookRegQueryMultipleValuesA(HKEY hKey, PVALENTA val_list, DWORD num_vals,
    LPSTR lpValueBuf, LPDWORD ldwTotsize)
{
    std::wstring path = GetVirtualPath(hKey);

    if (path.empty())
    {
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);

            if (!realPath.empty())
                for (DWORD i = 0; val_list && i < num_vals; ++i)
                {
                    std::wstring wideName = val_list[i].ve_valuename
                        ? AnsiToWide(val_list[i].ve_valuename) : std::wstring{};

                    LogRegistryAccess(L"REG READ", realPath.c_str(), wideName.c_str());
                }
        }

        return g_origRegQueryMultipleValuesA(hKey, val_list, num_vals, lpValueBuf, ldwTotsize);
    }

    if (!ldwTotsize)
        return ERROR_INVALID_PARAMETER;

    if (num_vals == 0)
    {
        *ldwTotsize = 0;

        return ERROR_SUCCESS;
    }

    if (!val_list)
        return ERROR_INVALID_PARAMETER;

    std::vector<std::wstring> names(num_vals);

    for (DWORD i = 0; i < num_vals; ++i)
    {
        names[i] = val_list[i].ve_valuename
            ? AnsiToWide(val_list[i].ve_valuename) : std::wstring{};

        LogRegistryAccess(L"REG READ", path.c_str(), names[i].c_str());
    }

    std::vector<std::vector<BYTE>> payloads(num_vals);
    std::vector<DWORD>             types(num_vals, REG_NONE);
    DWORD                          total = 0;

    {
        std::shared_lock lock(g_storeMutex);

        for (DWORD i = 0; i < num_vals; ++i)
        {
            const RegValue* rv = FindValueLocked(path, ToUpper(names[i]));

            if (!rv)
                return ERROR_FILE_NOT_FOUND;

            types[i] = rv->type;

            payloads[i] = (rv->type == REG_SZ || rv->type == REG_EXPAND_SZ
                                              || rv->type == REG_MULTI_SZ)
                ? WideBytesToAnsiBytes(rv->data)
                : rv->data;

            total += static_cast<DWORD>(payloads[i].size());
        }
    }

    if (!lpValueBuf || *ldwTotsize < total)
    {
        *ldwTotsize = total;

        return ERROR_MORE_DATA;
    }

    BYTE* cursor = reinterpret_cast<BYTE*>(lpValueBuf);

    for (DWORD i = 0; i < num_vals; ++i)
    {
        memcpy(cursor, payloads[i].data(), payloads[i].size());

        val_list[i].ve_valuelen = static_cast<DWORD>(payloads[i].size());
        val_list[i].ve_valueptr = reinterpret_cast<DWORD_PTR>(cursor);
        val_list[i].ve_type     = types[i];

        cursor += payloads[i].size();
    }

    *ldwTotsize = total;

    return ERROR_SUCCESS;
}

// --- RegDeleteKey / RegDeleteKeyEx ---
static LSTATUS WINAPI HookRegDeleteKeyW(HKEY hKey, LPCWSTR lpSubKey)
{
    std::wstring path = BuildPath(hKey, lpSubKey);

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogRegistryAccess(L"REG DELETE", path.empty() ? L"(unknown)" : path.c_str());
    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegDeleteKeyW(hKey, lpSubKey);

    LSTATUS status = VirtualDeleteKeyPath(path);

    if (status == ERROR_SUCCESS)
        SaveRegFile();

    return status;
}

static LSTATUS WINAPI HookRegDeleteKeyA(HKEY hKey, LPCSTR lpSubKey)
{
    std::wstring wideSubKey = lpSubKey ? AnsiToWide(lpSubKey) : std::wstring{};

    return HookRegDeleteKeyW(hKey, wideSubKey.empty() ? nullptr : wideSubKey.c_str());
}

static LSTATUS WINAPI HookRegDeleteKeyExW(HKEY hKey, LPCWSTR lpSubKey,
    REGSAM samDesired, DWORD Reserved)
{
    std::wstring path = BuildPath(hKey, lpSubKey);

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogRegistryAccess(L"REG DELETE", path.empty() ? L"(unknown)" : path.c_str());
    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegDeleteKeyExW(hKey, lpSubKey, samDesired, Reserved);

    LSTATUS status = VirtualDeleteKeyPath(path);

    if (status == ERROR_SUCCESS)
        SaveRegFile();

    return status;
}

static LSTATUS WINAPI HookRegDeleteKeyExA(HKEY hKey, LPCSTR lpSubKey,
    REGSAM samDesired, DWORD Reserved)
{
    std::wstring wideSubKey = lpSubKey ? AnsiToWide(lpSubKey) : std::wstring{};

    return HookRegDeleteKeyExW(hKey, wideSubKey.empty() ? nullptr : wideSubKey.c_str(),
        samDesired, Reserved);
}

// --- RegDeleteTree ---
static LSTATUS WINAPI HookRegDeleteTreeW(HKEY hKey, LPCWSTR lpSubKey)
{
    std::wstring path = BuildPath(hKey, lpSubKey);

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogRegistryAccess(L"REG DELETE", path.empty() ? L"(unknown)" : path.c_str());
    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegDeleteTreeW(hKey, lpSubKey);

    LSTATUS status = VirtualDeleteTree(path, lpSubKey && lpSubKey[0] != L'\0');

    if (status == ERROR_SUCCESS)
        SaveRegFile();

    return status;
}

static LSTATUS WINAPI HookRegDeleteTreeA(HKEY hKey, LPCSTR lpSubKey)
{
    std::wstring wideSubKey = lpSubKey ? AnsiToWide(lpSubKey) : std::wstring{};

    return HookRegDeleteTreeW(hKey, wideSubKey.empty() ? nullptr : wideSubKey.c_str());
}

// --- RegOpenCurrentUser / RegOpenUserClassesRoot ---
static LSTATUS WINAPI HookRegOpenCurrentUser(REGSAM samDesired, PHKEY phkResult)
{
    LogRegistryAccess(L"REG OPEN", L"HKEY_CURRENT_USER");

    if (g_registryIsolated)
        return VirtualOpenKey(L"HKEY_CURRENT_USER", phkResult);

    LSTATUS status = g_origRegOpenCurrentUser(samDesired, phkResult);

    if (status == ERROR_SUCCESS && phkResult && *phkResult)
        TrackRealHandle(*phkResult, L"HKEY_CURRENT_USER");

    return status;
}

static LSTATUS WINAPI HookRegOpenUserClassesRoot(HANDLE hToken, DWORD dwOptions,
    REGSAM samDesired, PHKEY phkResult)
{
    LogRegistryAccess(L"REG OPEN", L"HKEY_CLASSES_ROOT");

    if (g_registryIsolated)
        return VirtualOpenKey(L"HKEY_CLASSES_ROOT", phkResult);

    LSTATUS status = g_origRegOpenUserClassesRoot(hToken, dwOptions, samDesired, phkResult);

    if (status == ERROR_SUCCESS && phkResult && *phkResult)
        TrackRealHandle(*phkResult, L"HKEY_CLASSES_ROOT");

    return status;
}

// --- RegFlushKey ---
static LSTATUS WINAPI HookRegFlushKey(HKEY hKey)
{
    std::wstring path = GetVirtualPath(hKey);

    if (!path.empty())
    {
        if (g_dirty)
        {
            SaveRegFile();

            LogRegistryDiag(L"REG FLUSH", path.c_str(), L"virtual store written");
        }
        else
            LogRegistryDiag(L"REG FLUSH", path.c_str(), L"virtual store already persisted");

        return ERROR_SUCCESS;
    }

    if (g_logRegistry)
    {
        std::wstring realPath = GetAnyPathFull(hKey);

        if (!realPath.empty())
            LogRegistryDiag(L"REG FLUSH", realPath.c_str(), L"real key");
    }

    return g_origRegFlushKey(hKey);
}

// --- RegNotifyChangeKeyValue ---
static LSTATUS WINAPI HookRegNotifyChangeKeyValue(HKEY hKey, BOOL bWatchSubtree,
    DWORD dwNotifyFilter, HANDLE hEvent, BOOL fAsynchronous)
{
    std::wstring path = GetVirtualPath(hKey);

    if (path.empty())
    {
        if (g_logRegistry)
        {
            std::wstring realPath = GetAnyPathFull(hKey);

            if (!realPath.empty())
                LogRegistryDiag(L"REG NOTIFY", realPath.c_str(), L"real key");
        }

        return g_origRegNotifyChangeKeyValue(hKey, bWatchSubtree, dwNotifyFilter,
            hEvent, fAsynchronous);
    }

    if (fAsynchronous && !hEvent)
        return ERROR_INVALID_PARAMETER;

    if (fAsynchronous)
    {
        LogRegistryDiag(L"REG NOTIFY", path.c_str(), L"registered; the store never changes");

        return ERROR_SUCCESS;
    }

    LogRegistryDiag(L"REG NOTIFY", path.c_str(), L"synchronous wait not emulated");

    return ERROR_SUCCESS;
}

static LSTATUS WINAPI HookRegOpenKeyTransactedW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
    REGSAM samDesired, PHKEY phkResult, HANDLE hTransaction, PVOID pExtendedParameter)
{
    std::wstring path = BuildPath(hKey, lpSubKey);

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogRegistryAccess(L"REG OPEN", path.empty() ? L"(unknown)" : path.c_str());
    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegOpenKeyTransactedW(hKey, lpSubKey, ulOptions, samDesired,
            phkResult, hTransaction, pExtendedParameter);

    if (hTransaction)
        LogRegistryDiag(L"REG HIT", path.c_str(), L"transaction ignored");

    return VirtualOpenKey(path, phkResult);
}

static LSTATUS WINAPI HookRegOpenKeyTransactedA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions,
    REGSAM samDesired, PHKEY phkResult, HANDLE hTransaction, PVOID pExtendedParameter)
{
    std::wstring wideSubKey = lpSubKey ? AnsiToWide(lpSubKey) : std::wstring{};

    return HookRegOpenKeyTransactedW(hKey,
        wideSubKey.empty() ? nullptr : wideSubKey.c_str(),
        ulOptions, samDesired, phkResult, hTransaction, pExtendedParameter);
}

static LSTATUS WINAPI HookRegCreateKeyTransactedW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved,
    LPWSTR lpClass, DWORD dwOptions, REGSAM samDesired, LPSECURITY_ATTRIBUTES lpSA,
    PHKEY phkResult, LPDWORD lpdwDisposition, HANDLE hTransaction, PVOID pExtendedParameter)
{
    std::wstring path = BuildPath(hKey, lpSubKey);

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogRegistryAccess(L"REG CREATE", path.empty() ? L"(unknown)" : path.c_str());
    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegCreateKeyTransactedW(hKey, lpSubKey, Reserved, lpClass, dwOptions,
            samDesired, lpSA, phkResult, lpdwDisposition, hTransaction, pExtendedParameter);

    if (hTransaction)
        LogRegistryDiag(L"REG HIT", path.c_str(), L"transaction ignored");

    return VirtualCreateKey(path, phkResult, lpdwDisposition);
}

static LSTATUS WINAPI HookRegCreateKeyTransactedA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved,
    LPSTR lpClass, DWORD dwOptions, REGSAM samDesired, LPSECURITY_ATTRIBUTES lpSA,
    PHKEY phkResult, LPDWORD lpdwDisposition, HANDLE hTransaction, PVOID pExtendedParameter)
{
    std::wstring wideSubKey = lpSubKey ? AnsiToWide(lpSubKey) : std::wstring{};
    std::wstring path = BuildPath(hKey, wideSubKey.empty() ? nullptr : wideSubKey.c_str());

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogRegistryAccess(L"REG CREATE", path.empty() ? L"(unknown)" : path.c_str());
    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegCreateKeyTransactedA(hKey, lpSubKey, Reserved, lpClass, dwOptions,
            samDesired, lpSA, phkResult, lpdwDisposition, hTransaction, pExtendedParameter);

    if (hTransaction)
        LogRegistryDiag(L"REG HIT", path.c_str(), L"transaction ignored");

    return VirtualCreateKey(path, phkResult, lpdwDisposition);
}

static LSTATUS WINAPI HookRegDeleteKeyTransactedW(HKEY hKey, LPCWSTR lpSubKey,
    REGSAM samDesired, DWORD Reserved, HANDLE hTransaction, PVOID pExtendedParameter)
{
    std::wstring path = BuildPath(hKey, lpSubKey);

    bool virtualSpace = !path.empty() && InVirtualSpace(path);

    LogRegistryAccess(L"REG DELETE", path.empty() ? L"(unknown)" : path.c_str());
    LogVirtualSpaceVerdict(path, virtualSpace);

    if (!virtualSpace)
        return g_origRegDeleteKeyTransactedW(hKey, lpSubKey, samDesired, Reserved,
            hTransaction, pExtendedParameter);

    if (hTransaction)
        LogRegistryDiag(L"REG HIT", path.c_str(), L"transaction ignored");

    LSTATUS status = VirtualDeleteKeyPath(path);

    if (status == ERROR_SUCCESS)
        SaveRegFile();

    return status;
}

static LSTATUS WINAPI HookRegDeleteKeyTransactedA(HKEY hKey, LPCSTR lpSubKey,
    REGSAM samDesired, DWORD Reserved, HANDLE hTransaction, PVOID pExtendedParameter)
{
    std::wstring wideSubKey = lpSubKey ? AnsiToWide(lpSubKey) : std::wstring{};

    return HookRegDeleteKeyTransactedW(hKey,
        wideSubKey.empty() ? nullptr : wideSubKey.c_str(),
        samDesired, Reserved, hTransaction, pExtendedParameter);
}

// --- RegCopyTree ---
static LSTATUS WINAPI HookRegCopyTreeW(HKEY hKeySrc, LPCWSTR lpSubKey, HKEY hKeyDest)
{
    std::wstring sourcePath = BuildPath(hKeySrc, lpSubKey);
    std::wstring destPath   = GetVirtualPath(hKeyDest);

    bool sourceVirtual = !sourcePath.empty() && InVirtualSpace(sourcePath);
    bool destVirtual   = !destPath.empty();

    if (!sourceVirtual && !destVirtual)
        return g_origRegCopyTreeW(hKeySrc, lpSubKey, hKeyDest);

    LogRegistryDiag(L"REG COPY", sourcePath.empty() ? L"(unknown)" : sourcePath.c_str(),
        destVirtual ? destPath.c_str() : L"(real key)");

    // One line for the destination root rather than one [REG WRITE] per value: a
    // tree copy would otherwise flood both the log and the plugin callbacks.
    if (destVirtual)
        LogRegistryAccess(L"REG CREATE", destPath.c_str());

    SubtreeSnapshot snapshot;

    if (sourceVirtual)
        snapshot = SnapshotVirtualSubtree(sourcePath);
    else
    {
        HKEY hSource = hKeySrc;
        bool opened  = false;

        if (lpSubKey && lpSubKey[0] != L'\0')
        {
            if (g_origRegOpenKeyExW(hKeySrc, lpSubKey, 0, KEY_READ, &hSource) != ERROR_SUCCESS)
                return ERROR_FILE_NOT_FOUND;

            opened = true;
        }

        snapshot = SnapshotRealSubtree(hSource);

        if (opened)
            g_origRegCloseKey(hSource);
    }

    // An empty source is a successful no-op, not a failure.
    if (snapshot.empty())
        return ERROR_SUCCESS;

    if (destVirtual)
    {
        ApplyVirtualSubtree(destPath, snapshot);

        SaveRegFile();

        return ERROR_SUCCESS;
    }

    return ApplyRealSubtree(hKeyDest, snapshot);
}

// Create every missing directory above filePath. SaveRegFile opens the write
// file with CREATE_ALWAYS, which will not create directories, and a
// Registry.Files entry may point somewhere that does not exist yet.
static void EnsureParentDirectory(const std::wstring& filePath)
{
    size_t slash = filePath.find_last_of(L"\\/");

    if (slash == std::wstring::npos)
        return;

    std::wstring directory = filePath.substr(0, slash);

    for (size_t i = 3; i <= directory.size(); ++i)
    {
        // i == size() closes out the final segment, which has no separator.
        if (i != directory.size() && directory[i] != L'\\' && directory[i] != L'/')
            continue;

        CreateDirectoryW(directory.substr(0, i).c_str(), nullptr);
    }
}

// ============================================================
// Public API
// ============================================================
// Hooks a registry API in KernelBase, falling back to advapi32.
template <typename Fn>
static void HookRegistryApi(const char* name, Fn detour, Fn* orig)
{
    MH_STATUS status = MH_CreateHookApi(L"kernelbase", name,
        reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(orig));

    if (status == MH_OK || status == MH_ERROR_ALREADY_CREATED)
    {
        LogHookInit(L"kernelbase", name, status);
        return;
    }

    // A missing module or export is the expected route to the fallback; anything
    // else is worth reporting, because the fallback would otherwise hide it.
    if (status != MH_ERROR_MODULE_NOT_FOUND && status != MH_ERROR_FUNCTION_NOT_FOUND)
        LogHookInit(L"kernelbase", name, status);

    LogHookInit(L"advapi32", name,
        MH_CreateHookApi(L"advapi32", name,
            reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(orig)));
}

void InstallRegistryHooks()
{
    // Locate the default store, .interposer\Registry.reg, next to our DLL
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

    // Config.yml may replace the single default file with an overlay stack. The
    // last entry is the writable layer — everything before it is a read-only
    // template loaded underneath it.
    g_regFiles = g_registryFiles;

    if (g_regFiles.empty())
        g_regFiles.push_back(interposerDir + L"Registry.reg");

    g_writeFile = g_regFiles.back();

    // Only the writable layer needs somewhere to land; a base file that is not
    // there is simply an empty layer.
    EnsureParentDirectory(g_writeFile);

    for (size_t i = 0; i < g_regFiles.size(); ++i)
    {
        const bool writeLayer = (i + 1 == g_regFiles.size());

        LoadRegFile(g_regFiles[i], writeLayer);

        LogRegistryDiag(L"REG LAYER", g_regFiles[i].c_str(),
            writeLayer ? L"writable" : L"read-only");
    }

    if (g_registryIsolated)
        LogRegistryDiag(L"REG LAYER", g_writeFile.c_str(),
            L"isolated: every key is served from the virtual store");

    // Install the 42 registry hooks, preferring KernelBase (see HookRegistryApi).
    // The leaf functions first: each operates on a handle the caller already holds.
    HookRegistryApi("RegOpenKeyExW",    HookRegOpenKeyExW, &g_origRegOpenKeyExW);
    HookRegistryApi("RegOpenKeyExA",    HookRegOpenKeyExA, &g_origRegOpenKeyExA);
    HookRegistryApi("RegCreateKeyExW",  HookRegCreateKeyExW, &g_origRegCreateKeyExW);
    HookRegistryApi("RegCreateKeyExA",  HookRegCreateKeyExA, &g_origRegCreateKeyExA);
    HookRegistryApi("RegCloseKey",      HookRegCloseKey, &g_origRegCloseKey);
    HookRegistryApi("RegQueryValueExW", HookRegQueryValueExW, &g_origRegQueryValueExW);
    HookRegistryApi("RegQueryValueExA", HookRegQueryValueExA, &g_origRegQueryValueExA);
    HookRegistryApi("RegSetValueExW",   HookRegSetValueExW, &g_origRegSetValueExW);
    HookRegistryApi("RegSetValueExA",   HookRegSetValueExA, &g_origRegSetValueExA);
    HookRegistryApi("RegDeleteValueW",  HookRegDeleteValueW, &g_origRegDeleteValueW);
    HookRegistryApi("RegDeleteValueA",  HookRegDeleteValueA, &g_origRegDeleteValueA);
    HookRegistryApi("RegEnumValueW",    HookRegEnumValueW, &g_origRegEnumValueW);
    HookRegistryApi("RegEnumValueA",    HookRegEnumValueA, &g_origRegEnumValueA);
    HookRegistryApi("RegEnumKeyExW",    HookRegEnumKeyExW, &g_origRegEnumKeyExW);
    HookRegistryApi("RegEnumKeyExA",    HookRegEnumKeyExA, &g_origRegEnumKeyExA);
    HookRegistryApi("RegQueryInfoKeyW", HookRegQueryInfoKeyW, &g_origRegQueryInfoKeyW);
    HookRegistryApi("RegQueryInfoKeyA", HookRegQueryInfoKeyA, &g_origRegQueryInfoKeyA);

    HookRegistryApi("RegGetValueW",            HookRegGetValueW, &g_origRegGetValueW);
    HookRegistryApi("RegGetValueA",            HookRegGetValueA, &g_origRegGetValueA);
    HookRegistryApi("RegSetKeyValueW",         HookRegSetKeyValueW, &g_origRegSetKeyValueW);
    HookRegistryApi("RegSetKeyValueA",         HookRegSetKeyValueA, &g_origRegSetKeyValueA);
    HookRegistryApi("RegDeleteKeyValueW",      HookRegDeleteKeyValueW, &g_origRegDeleteKeyValueW);
    HookRegistryApi("RegDeleteKeyValueA",      HookRegDeleteKeyValueA, &g_origRegDeleteKeyValueA);
    HookRegistryApi("RegQueryMultipleValuesW", HookRegQueryMultipleValuesW, &g_origRegQueryMultipleValuesW);
    HookRegistryApi("RegQueryMultipleValuesA", HookRegQueryMultipleValuesA, &g_origRegQueryMultipleValuesA);

    HookRegistryApi("RegDeleteKeyW",    HookRegDeleteKeyW, &g_origRegDeleteKeyW);
    HookRegistryApi("RegDeleteKeyA",    HookRegDeleteKeyA, &g_origRegDeleteKeyA);
    HookRegistryApi("RegDeleteKeyExW",  HookRegDeleteKeyExW, &g_origRegDeleteKeyExW);
    HookRegistryApi("RegDeleteKeyExA",  HookRegDeleteKeyExA, &g_origRegDeleteKeyExA);
    HookRegistryApi("RegDeleteTreeW",   HookRegDeleteTreeW, &g_origRegDeleteTreeW);
    HookRegistryApi("RegDeleteTreeA",   HookRegDeleteTreeA, &g_origRegDeleteTreeA);

    // Hive roots, flush and change notification.
    HookRegistryApi("RegOpenCurrentUser",      HookRegOpenCurrentUser, &g_origRegOpenCurrentUser);
    HookRegistryApi("RegOpenUserClassesRoot",  HookRegOpenUserClassesRoot, &g_origRegOpenUserClassesRoot);
    HookRegistryApi("RegFlushKey",             HookRegFlushKey, &g_origRegFlushKey);
    HookRegistryApi("RegNotifyChangeKeyValue", HookRegNotifyChangeKeyValue, &g_origRegNotifyChangeKeyValue);

    // The transacted six. KernelBase exports none of them, so all six take the
    // advapi32 fallback.
    HookRegistryApi("RegOpenKeyTransactedW",   HookRegOpenKeyTransactedW, &g_origRegOpenKeyTransactedW);
    HookRegistryApi("RegOpenKeyTransactedA",   HookRegOpenKeyTransactedA, &g_origRegOpenKeyTransactedA);
    HookRegistryApi("RegCreateKeyTransactedW", HookRegCreateKeyTransactedW, &g_origRegCreateKeyTransactedW);
    HookRegistryApi("RegCreateKeyTransactedA", HookRegCreateKeyTransactedA, &g_origRegCreateKeyTransactedA);
    HookRegistryApi("RegDeleteKeyTransactedW", HookRegDeleteKeyTransactedW, &g_origRegDeleteKeyTransactedW);
    HookRegistryApi("RegDeleteKeyTransactedA", HookRegDeleteKeyTransactedA, &g_origRegDeleteKeyTransactedA);

    HookRegistryApi("RegCopyTreeW", HookRegCopyTreeW, &g_origRegCopyTreeW);
}

void RemoveRegistryHooks()
{
    // No MinHook teardown here — dllmain.cpp calls MH_DisableHook / MH_Uninitialize.
    // Just flush any pending writes.
    if (g_dirty)
        SaveRegFile();
}

// ---------------------------------------------------------------------------
// Plugin API export
// ---------------------------------------------------------------------------

// Inject a REG_SZ value into the in-memory virtual store so that subsequent
// RegQueryValueEx calls for keyPath\valueName return value.
// keyPath must be a full path starting with a hive name, e.g.
//   L"HKEY_LOCAL_MACHINE\\SOFTWARE\\MyGame"
// valueName may be "@" or nullptr/empty to target the default (unnamed) value.
// The value is in-memory only and is NOT persisted to Registry.reg.
extern "C" __declspec(dllexport)
void InterposerSetRegistryValue(const wchar_t* keyPath, const wchar_t* valueName, const wchar_t* value)
{
    if (!keyPath || !value) return;

    std::wstring upperPath = ToUpper(keyPath);
    std::wstring upperName = valueName ? ToUpper(valueName) : std::wstring{};
    if (upperName == L"@") upperName.clear(); // @ = default value

    std::wstring wval(value);
    RegValue rv;
    rv.type = REG_SZ;
    rv.data.resize((wval.size() + 1) * sizeof(wchar_t));
    memcpy(rv.data.data(), wval.c_str(), rv.data.size());

    std::unique_lock lk(g_storeMutex);
    g_store[upperPath][upperName] = std::move(rv);
    // g_dirty intentionally NOT set — plugin values are transient
}

// Inject a transient REG_SZ value into every virtual store key whose path ends
// with keySuffix (matched on a backslash boundary, case-insensitive).
// valueName may be "@" or nullptr/empty to target the default (unnamed) value.
// Returns the number of keys updated; 0 means keySuffix matched nothing in the
// virtual store — ensure the key exists in Registry.reg.
extern "C" __declspec(dllexport)
DWORD InterposerSetRegistryValueBySuffix(const wchar_t* keySuffix, const wchar_t* valueName, const wchar_t* value)
{
    if (!keySuffix || !value) return 0;

    std::wstring upperSuffix = ToUpper(keySuffix);
    if (!upperSuffix.empty() && upperSuffix.front() == L'\\')
        upperSuffix.erase(0, 1); // strip optional leading backslash

    std::wstring upperName = valueName ? ToUpper(valueName) : std::wstring{};
    if (upperName == L"@") upperName.clear(); // @ = default value

    std::wstring wval(value);
    RegValue rv;
    rv.type = REG_SZ;
    rv.data.resize((wval.size() + 1) * sizeof(wchar_t));
    memcpy(rv.data.data(), wval.c_str(), rv.data.size());

    DWORD count = 0;
    std::unique_lock lk(g_storeMutex);
    for (auto& [path, values] : g_store)
    {
        if (path.size() >= upperSuffix.size())
        {
            size_t offset = path.size() - upperSuffix.size();
            if ((offset == 0 || path[offset - 1] == L'\\') &&
                path.compare(offset, upperSuffix.size(), upperSuffix) == 0)
            {
                values[upperName] = rv;
                ++count;
            }
        }
    }
    // g_dirty intentionally NOT set — plugin values are transient
    return count;
}
