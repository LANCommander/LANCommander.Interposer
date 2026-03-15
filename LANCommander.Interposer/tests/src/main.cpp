// ============================================================
// LANCommander.Interposer — Integration Tests
// ============================================================
// Self-contained test runner. Writes fixture files, loads the
// DLL via LoadLibraryW (which installs all MinHook API hooks),
// exercises hooked Win32 calls, unloads the DLL, then verifies
// the log file and the persisted VirtualRegistry.reg.
//
// Build:  LANCommander.Interposer.Tests.vcxproj
// Run:    x64\Debug\LANCommander.Interposer.Tests.exe
//         (build the DLL first)
// ============================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>

// ============================================================
// Test infrastructure
// ============================================================

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg)                                    \
    do {                                                      \
        if (cond) {                                           \
            ++g_pass;                                         \
            wprintf(L"  PASS  " msg L"\n");                  \
        } else {                                              \
            ++g_fail;                                         \
            wprintf(L"  FAIL  " msg L"\n");                  \
        }                                                     \
    } while (0)

// ============================================================
// Helpers
// ============================================================

// Directory that contains this EXE (and the DLL).
static std::wstring GetExeDir()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    auto p = s.rfind(L'\\');
    if (p != std::wstring::npos) s.resize(p + 1);
    return s;
}

// Base directory for temp test artifacts: %TEMP%\InterposerTest
static std::wstring GetTestTempDir()
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"InterposerTest\\";
}

// Convert a wide string to UTF-8 (Windows paths are ASCII-safe, but handles full Unicode).
static std::string WideToUtf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
        nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
        s.data(), len, nullptr, nullptr);
    return s;
}

// Write raw UTF-8 bytes, overwriting any existing file.
// NOTE: called before LoadLibraryW, so not intercepted by hooks.
static bool WriteTextFile(const std::wstring& path, const std::string& content)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    ::WriteFile(h, content.c_str(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(h);
    return true;
}

// Read entire file as a UTF-8 std::string.
static std::string ReadFileAsUtf8(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart == 0 || sz.QuadPart > 16 * 1024 * 1024) { CloseHandle(h); return {}; }
    std::string out(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD rd;
    ::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &rd, nullptr);
    CloseHandle(h);
    return out;
}

// Read a file as std::wstring, decoding UTF-16 LE BOM if present, else UTF-8.
static std::wstring ReadFileAsWide(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart < 2 || sz.QuadPart > 16 * 1024 * 1024) { CloseHandle(h); return {}; }
    std::vector<BYTE> raw(static_cast<size_t>(sz.QuadPart));
    DWORD rd;
    ::ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &rd, nullptr);
    CloseHandle(h);

    if (raw[0] == 0xFF && raw[1] == 0xFE)
    {
        // UTF-16 LE with BOM — skip 2-byte BOM
        size_t charCount = (raw.size() - 2) / 2;
        return std::wstring(reinterpret_cast<const wchar_t*>(raw.data() + 2), charCount);
    }
    // UTF-8 / ANSI fallback
    int wlen = MultiByteToWideChar(CP_UTF8, 0,
        reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()), nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
        reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()), ws.data(), wlen);
    return ws;
}

// Return the path of the first *.log file found in logsDir, or empty string.
static std::wstring FindFirstLogFile(const std::wstring& logsDir)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((logsDir + L"*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring result = logsDir + fd.cFileName;
    FindClose(h);
    return result;
}

// Delete all *.log files in logsDir (cleans up logs from prior test runs).
static void ClearLogFiles(const std::wstring& logsDir)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((logsDir + L"*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { DeleteFileW((logsDir + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Recursively create a directory path (like mkdir -p).
static bool CreateDirs(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return true;
    if (err == ERROR_PATH_NOT_FOUND)
    {
        // Create parent first
        size_t searchFrom = path.size() > 1 ? path.size() - 2 : 0;
        auto p = path.rfind(L'\\', searchFrom);
        if (p != std::wstring::npos)
        {
            if (!CreateDirs(path.substr(0, p + 1))) return false;
            return CreateDirectoryW(path.c_str(), nullptr)
                || GetLastError() == ERROR_ALREADY_EXISTS;
        }
    }
    return false;
}

// ============================================================
// Fixture writers (all called BEFORE LoadLibraryW)
// ============================================================

// .interposer/Config.yml placed next to the DLL.
// Uses absolute paths (no %ENVVAR%). Log path is auto-generated by LoadConfig
// as .interposer\Logs\<timestamp>.log — discovered after DLL unload via FindFirstLogFile.
//
// Single-quoted YAML strings pass backslashes literally.
// Each \\ in the pattern becomes one regex backslash, matching \ in Windows paths.
static void WriteInterposerYaml(const std::wstring& yamlPath,
                                 const std::wstring& redirectBaseDir)
{
    // redirectBaseDir ends with '\', so the replacement becomes:
    //   <redirectBaseDir>$1  e.g.  C:\Temp\InterposerTest\Redirected\$1
    //
    // Pattern in single-quoted YAML: 'C:\\TestGame\\Saves\\(.+)'
    //   yaml-cpp returns: C:\\TestGame\\Saves\\(.+)
    //   std::wregex sees: C:\\TestGame\\Saves\\(.+)  (matches C:\TestGame\Saves\...)
    //
    // In this C++ string literal, \\\\ -> \\ in the file (two backslashes in YAML = one regex backslash).
    std::string content =
        "settings:\n"
        "  logFiles: true\n"
        "  logRegistry: true\n"
        "\n"
        "fileRedirects:\n"
        "  - pattern: 'C:\\\\TestGame\\\\Saves\\\\(.+)'\n"
        "    replacement: '" + WideToUtf8(redirectBaseDir) + "$1'\n";
    WriteTextFile(yamlPath, content);
}

// VirtualRegistry.reg placed next to the DLL.
// Key: HKEY_LOCAL_MACHINE\SOFTWARE\TestGame\1.0  (virtual, never touches real registry)
static void WriteVirtualReg(const std::wstring& regPath)
{
    const std::string content =
        "Windows Registry Editor Version 5.00\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\1.0]\r\n"
        "\"PlayerName\"=\"TestSoldier\"\r\n"
        "\"Version\"=dword:00000001\r\n"
        "\"BinaryData\"=hex:DE,AD,BE,EF\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\1.0\\SubKey]\r\n"
        "\"SubValue\"=\"InSubKey\"\r\n";
    WriteTextFile(regPath, content);
}

// ============================================================
// File hook tests  (run while DLL is loaded → hooks active)
// ============================================================

static void RunFileTests(const std::wstring& exeDir, const std::wstring& testTmpDir)
{
    wprintf(L"\n--- File Hook Tests ---\n");

    std::wstring redirectDir    = testTmpDir + L"Redirected\\";
    std::wstring redirectTarget = redirectDir + L"profile.dat";

    // F-01: Direct open passthrough — open the DLL itself (exists, no redirect rule matches)
    {
        std::wstring dllPath = exeDir + L"LANCommander.Interposer.dll";
        HANDLE h = CreateFileW(dllPath.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT(h != INVALID_HANDLE_VALUE,
            L"F-01: CreateFileW direct open (no redirect) succeeds");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    // F-02: CreateFileW redirect fires — opens the redirect-target file
    //        C:\TestGame\Saves\profile.dat  →  testTmpDir\Redirected\profile.dat (exists)
    {
        HANDLE h = CreateFileW(L"C:\\TestGame\\Saves\\profile.dat", GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT(h != INVALID_HANDLE_VALUE,
            L"F-02: CreateFileW redirect opens redirected file successfully");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    // F-03: CreateFileW with no matching redirect — path does not exist, expect NOT_FOUND
    {
        SetLastError(0);
        HANDLE h = CreateFileW(L"C:\\NoMatch\\path\\file.dat", GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT(h == INVALID_HANDLE_VALUE,
            L"F-03: CreateFileW non-matching path returns INVALID_HANDLE_VALUE");
        DWORD err = GetLastError();
        ASSERT(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND,
            L"F-03: non-matching path GLE is FILE_NOT_FOUND or PATH_NOT_FOUND");
    }

    // F-04: CreateFileA redirect fires — ANSI variant converts to wide and applies redirect
    {
        HANDLE h = CreateFileA("C:\\TestGame\\Saves\\profile.dat", GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT(h != INVALID_HANDLE_VALUE,
            L"F-04: CreateFileA redirect opens redirected file successfully");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    // F-05: GetFileAttributesW passthrough — the test EXE itself exists
    {
        std::wstring exePath = exeDir + L"LANCommander.Interposer.Tests.exe";
        DWORD attr = GetFileAttributesW(exePath.c_str());
        ASSERT(attr != INVALID_FILE_ATTRIBUTES,
            L"F-05: GetFileAttributesW passthrough returns valid attributes");
    }

    // F-06: GetFileAttributesW redirect fires
    //        C:\TestGame\Saves\profile.dat  →  redirectTarget (exists)
    {
        DWORD attr = GetFileAttributesW(L"C:\\TestGame\\Saves\\profile.dat");
        ASSERT(attr != INVALID_FILE_ATTRIBUTES,
            L"F-06: GetFileAttributesW redirect returns valid attributes");
    }

    // F-07: GetFileAttributesA redirect fires (ANSI variant)
    {
        DWORD attr = GetFileAttributesA("C:\\TestGame\\Saves\\profile.dat");
        ASSERT(attr != INVALID_FILE_ATTRIBUTES,
            L"F-07: GetFileAttributesA redirect returns valid attributes");
    }
}

// ============================================================
// Registry hook tests  (run while DLL is loaded → hooks active)
// ============================================================

static void RunRegistryTests()
{
    wprintf(L"\n--- Registry Hook Tests ---\n");

    // All virtual keys live under HKLM\SOFTWARE\TestGame\1.0 as defined in VirtualRegistry.reg.
    // No actual registry access happens for virtual keys — the hook serves them from its
    // in-memory store without ever calling the real advapi32 functions.

    HKEY hk = nullptr;

    // R-01: Open virtual key
    {
        LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\1.0", 0, KEY_READ, &hk);
        ASSERT(st == ERROR_SUCCESS, L"R-01a: RegOpenKeyExW virtual key returns ERROR_SUCCESS");
        ASSERT(hk != nullptr,       L"R-01b: RegOpenKeyExW returns non-null handle");
    }
    if (!hk)
    {
        wprintf(L"  SKIP remaining registry tests: key open failed\n");
        return;
    }

    // R-02: CloseKey virtual handle
    {
        LSTATUS st = RegCloseKey(hk);
        ASSERT(st == ERROR_SUCCESS, L"R-02: RegCloseKey virtual handle returns ERROR_SUCCESS");
        hk = nullptr;
    }

    // Re-open with read+write for subsequent tests
    RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\TestGame\\1.0", 0, KEY_READ | KEY_WRITE, &hk);
    if (!hk)
    {
        wprintf(L"  SKIP remaining registry tests: re-open failed\n");
        return;
    }

    // R-03: Query REG_SZ — two-step (size then data)
    {
        DWORD type = 0, cbData = 0;
        LSTATUS st = RegQueryValueExW(hk, L"PlayerName", nullptr, &type, nullptr, &cbData);
        ASSERT(st == ERROR_SUCCESS, L"R-03a: QueryValue REG_SZ size query succeeds");
        ASSERT(type == REG_SZ,      L"R-03b: PlayerName type is REG_SZ");

        std::vector<BYTE> buf(cbData);
        st = RegQueryValueExW(hk, L"PlayerName", nullptr, &type, buf.data(), &cbData);
        ASSERT(st == ERROR_SUCCESS, L"R-03c: QueryValue REG_SZ data fetch succeeds");

        const wchar_t* wstr = reinterpret_cast<const wchar_t*>(buf.data());
        ASSERT(wcscmp(wstr, L"TestSoldier") == 0,
            L"R-03d: PlayerName value is 'TestSoldier'");
    }

    // R-04: Query REG_DWORD
    {
        DWORD val = 0, cb = sizeof(DWORD), type = 0;
        LSTATUS st = RegQueryValueExW(hk, L"Version", nullptr, &type,
            reinterpret_cast<LPBYTE>(&val), &cb);
        ASSERT(st == ERROR_SUCCESS, L"R-04a: QueryValue REG_DWORD succeeds");
        ASSERT(type == REG_DWORD,   L"R-04b: Version type is REG_DWORD");
        ASSERT(val == 1,            L"R-04c: Version value is 1");
    }

    // R-05: Query REG_BINARY
    {
        DWORD type = 0, cbData = 0;
        RegQueryValueExW(hk, L"BinaryData", nullptr, &type, nullptr, &cbData);
        std::vector<BYTE> buf(cbData);
        LSTATUS st = RegQueryValueExW(hk, L"BinaryData", nullptr, &type,
            buf.data(), &cbData);
        ASSERT(st == ERROR_SUCCESS, L"R-05a: QueryValue REG_BINARY succeeds");
        ASSERT(type == REG_BINARY,  L"R-05b: BinaryData type is REG_BINARY");
        ASSERT(cbData == 4,         L"R-05c: BinaryData size is 4 bytes");
        ASSERT(buf.size() >= 4
            && buf[0] == 0xDE && buf[1] == 0xAD
            && buf[2] == 0xBE && buf[3] == 0xEF,
            L"R-05d: BinaryData bytes are DE AD BE EF");
    }

    // R-06: Query missing value returns ERROR_FILE_NOT_FOUND
    {
        DWORD sz = 0;
        LSTATUS st = RegQueryValueExW(hk, L"NoSuchValue", nullptr, nullptr, nullptr, &sz);
        ASSERT(st == ERROR_FILE_NOT_FOUND,
            L"R-06: missing value returns ERROR_FILE_NOT_FOUND");
    }

    // R-07: Value name lookup is case-insensitive
    {
        DWORD val = 0, cb = sizeof(DWORD);
        LSTATUS st = RegQueryValueExW(hk, L"version", nullptr, nullptr,
            reinterpret_cast<LPBYTE>(&val), &cb);
        ASSERT(st == ERROR_SUCCESS, L"R-07a: case-insensitive query succeeds");
        ASSERT(val == 1,            L"R-07b: case-insensitive query returns correct DWORD");
    }

    // R-08: Enumerate values (read initial state before any writes)
    {
        std::vector<std::wstring> names;
        DWORD index = 0;
        wchar_t nameBuf[256]{};
        DWORD nameLen = _countof(nameBuf);
        while (RegEnumValueW(hk, index, nameBuf, &nameLen,
            nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
        {
            names.push_back(nameBuf);
            nameLen = _countof(nameBuf);
            ++index;
        }
        bool hasPlayer = false, hasVersion = false, hasBinary = false;
        for (auto& n : names)
        {
            if (_wcsicmp(n.c_str(), L"PlayerName") == 0) hasPlayer  = true;
            if (_wcsicmp(n.c_str(), L"Version")    == 0) hasVersion = true;
            if (_wcsicmp(n.c_str(), L"BinaryData") == 0) hasBinary  = true;
        }
        ASSERT(hasPlayer,  L"R-08a: EnumValues finds PlayerName");
        ASSERT(hasVersion, L"R-08b: EnumValues finds Version");
        ASSERT(hasBinary,  L"R-08c: EnumValues finds BinaryData");
    }

    // R-09: Enumerate subkeys
    {
        wchar_t nameBuf[256]{};
        DWORD nameLen = _countof(nameBuf);
        LSTATUS st = RegEnumKeyExW(hk, 0, nameBuf, &nameLen,
            nullptr, nullptr, nullptr, nullptr);
        ASSERT(st == ERROR_SUCCESS,
            L"R-09a: EnumKeyEx first subkey returns ERROR_SUCCESS");
        ASSERT(_wcsicmp(nameBuf, L"SubKey") == 0,
            L"R-09b: first subkey name is 'SubKey'");

        nameLen = _countof(nameBuf);
        st = RegEnumKeyExW(hk, 1, nameBuf, &nameLen, nullptr, nullptr, nullptr, nullptr);
        ASSERT(st == ERROR_NO_MORE_ITEMS,
            L"R-09c: EnumKeyEx index 1 is ERROR_NO_MORE_ITEMS");
    }

    // R-10: QueryInfoKey
    {
        DWORD nSubKeys = 0, nValues = 0;
        LSTATUS st = RegQueryInfoKeyW(hk, nullptr, nullptr, nullptr,
            &nSubKeys, nullptr, nullptr, &nValues,
            nullptr, nullptr, nullptr, nullptr);
        ASSERT(st == ERROR_SUCCESS, L"R-10a: QueryInfoKey returns ERROR_SUCCESS");
        ASSERT(nSubKeys == 1,       L"R-10b: QueryInfoKey reports 1 subkey");
        ASSERT(nValues  == 3,       L"R-10c: QueryInfoKey reports 3 values");
    }

    // R-11: ANSI open + ANSI query
    {
        HKEY hkA = nullptr;
        LSTATUS st = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\TestGame\\1.0", 0, KEY_READ, &hkA);
        ASSERT(st == ERROR_SUCCESS, L"R-11a: RegOpenKeyExA returns ERROR_SUCCESS");
        if (hkA)
        {
            char buf[64]{};
            DWORD cb = sizeof(buf), type = 0;
            st = RegQueryValueExA(hkA, "PlayerName", nullptr, &type,
                reinterpret_cast<LPBYTE>(buf), &cb);
            ASSERT(st == ERROR_SUCCESS,             L"R-11b: RegQueryValueExA REG_SZ succeeds");
            ASSERT(strcmp(buf, "TestSoldier") == 0, L"R-11c: ANSI query returns 'TestSoldier'");
            RegCloseKey(hkA);
        }
    }

    // R-12: SetValue — write a new REG_SZ and read it back
    {
        const wchar_t* newVal = L"WrittenByTest";
        DWORD cb = static_cast<DWORD>((wcslen(newVal) + 1) * sizeof(wchar_t));
        LSTATUS st = RegSetValueExW(hk, L"NewValue", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(newVal), cb);
        ASSERT(st == ERROR_SUCCESS, L"R-12a: RegSetValueExW new value returns ERROR_SUCCESS");

        wchar_t readBuf[64]{};
        DWORD rCb = sizeof(readBuf), type = 0;
        st = RegQueryValueExW(hk, L"NewValue", nullptr, &type,
            reinterpret_cast<LPBYTE>(readBuf), &rCb);
        ASSERT(st == ERROR_SUCCESS,              L"R-12b: read-back of written value succeeds");
        ASSERT(wcscmp(readBuf, newVal) == 0,     L"R-12c: read-back data matches written data");
    }

    // R-13: SetValue — overwrite existing REG_DWORD
    {
        DWORD newVersion = 2;
        LSTATUS st = RegSetValueExW(hk, L"Version", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&newVersion), sizeof(DWORD));
        ASSERT(st == ERROR_SUCCESS, L"R-13a: RegSetValueExW overwrite DWORD succeeds");

        DWORD readVal = 0, cb = sizeof(DWORD);
        st = RegQueryValueExW(hk, L"Version", nullptr, nullptr,
            reinterpret_cast<LPBYTE>(&readVal), &cb);
        ASSERT(st == ERROR_SUCCESS && readVal == 2,
            L"R-13b: overwritten DWORD reads back as 2");
    }

    // R-14: DeleteValue — write then delete
    {
        const wchar_t* delVal = L"ToDelete";
        DWORD cb = static_cast<DWORD>((wcslen(delVal) + 1) * sizeof(wchar_t));
        RegSetValueExW(hk, L"TempDelete", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(delVal), cb);

        LSTATUS st = RegDeleteValueW(hk, L"TempDelete");
        ASSERT(st == ERROR_SUCCESS, L"R-14a: RegDeleteValueW returns ERROR_SUCCESS");

        DWORD qSz = 0;
        st = RegQueryValueExW(hk, L"TempDelete", nullptr, nullptr, nullptr, &qSz);
        ASSERT(st == ERROR_FILE_NOT_FOUND,
            L"R-14b: deleted value returns ERROR_FILE_NOT_FOUND on subsequent query");
    }

    // R-15: DeleteValue on missing key returns ERROR_FILE_NOT_FOUND
    {
        LSTATUS st = RegDeleteValueW(hk, L"NeverExisted");
        ASSERT(st == ERROR_FILE_NOT_FOUND,
            L"R-15: delete missing value returns ERROR_FILE_NOT_FOUND");
    }

    RegCloseKey(hk);
    hk = nullptr;

    // R-16: CreateKey (new subkey)
    {
        HKEY hNew = nullptr;
        DWORD disp = 0;
        LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\1.0\\NewSubKey",
            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
            nullptr, &hNew, &disp);
        ASSERT(st == ERROR_SUCCESS,             L"R-16a: RegCreateKeyExW new key returns ERROR_SUCCESS");
        ASSERT(disp == REG_CREATED_NEW_KEY,     L"R-16b: disposition is REG_CREATED_NEW_KEY");
        if (hNew) RegCloseKey(hNew);
    }

    // R-17: CreateKey on existing virtual key
    {
        HKEY hExist = nullptr;
        DWORD disp = 0;
        LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\1.0",
            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
            nullptr, &hExist, &disp);
        ASSERT(st == ERROR_SUCCESS,             L"R-17a: RegCreateKeyExW existing key returns ERROR_SUCCESS");
        ASSERT(disp == REG_OPENED_EXISTING_KEY, L"R-17b: disposition is REG_OPENED_EXISTING_KEY");
        if (hExist) RegCloseKey(hExist);
    }

    // R-18: Real registry passthrough — HKLM\SOFTWARE\Microsoft always exists and is readable
    {
        HKEY hReal = nullptr;
        LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft", 0, KEY_READ, &hReal);
        ASSERT(st == ERROR_SUCCESS,
            L"R-18: real registry key HKLM\\SOFTWARE\\Microsoft opens (passthrough)");
        if (hReal) RegCloseKey(hReal);
    }
}

// ============================================================
// Log verification tests  (run AFTER FreeLibrary flushes log)
// ============================================================

static void RunLogTests(const std::wstring& logPath)
{
    wprintf(L"\n--- Log Tests ---\n");

    std::string log = ReadFileAsUtf8(logPath);

    ASSERT(!log.empty(),
        L"L-01: Log file exists and is non-empty after DLL unload");
    ASSERT(log.find("=== Session started") != std::string::npos,
        L"L-02: Log contains session-start header");
    ASSERT(log.find("[FILE REDIRECT]") != std::string::npos,
        L"L-03: Log contains [FILE REDIRECT] entry");
    ASSERT(log.find("[FILE READ]") != std::string::npos,
        L"L-04: Log contains [FILE READ] entry");
    ASSERT(log.find("[FILE ATTR]") != std::string::npos,
        L"L-05: Log contains [FILE ATTR] entry");
    ASSERT(log.find("[REG OPEN]") != std::string::npos,
        L"L-06: Log contains [REG OPEN] entry");
    ASSERT(log.find("[REG READ]") != std::string::npos,
        L"L-07: Log contains [REG READ] entry");
    ASSERT(log.find("[REG WRITE]") != std::string::npos,
        L"L-08: Log contains [REG WRITE] entry");
    ASSERT(log.find("[REG DELETE]") != std::string::npos,
        L"L-09: Log contains [REG DELETE] entry");

    // Registry paths are uppercased by BuildPath → ToUpper
    ASSERT(log.find("TESTGAME") != std::string::npos,
        L"L-10: Log contains uppercased key name 'TESTGAME'");

    // Redirect log lines contain "->  " separating source from destination
    ASSERT(log.find("->") != std::string::npos,
        L"L-11: Redirect log entry contains '->' arrow");

    // The original (pre-redirect) path appears in the log
    ASSERT(log.find("TestGame") != std::string::npos || log.find("TESTGAME") != std::string::npos,
        L"L-12: Log contains reference to TestGame path");
}

// ============================================================
// Persistence tests  (run AFTER FreeLibrary → SaveRegFile called)
// ============================================================
//
// SaveRegFile() is called whenever g_dirty == true (set by any write op).
// It writes UTF-16 LE with BOM and uppercases ALL value names.

static void RunPersistenceTests(const std::wstring& regPath)
{
    wprintf(L"\n--- Persistence Tests ---\n");

    std::wstring reg = ReadFileAsWide(regPath);

    ASSERT(!reg.empty(),
        L"P-01: VirtualRegistry.reg is non-empty after DLL unload");

    // R-12 wrote "NewValue" → stored as "NEWVALUE"
    ASSERT(reg.find(L"\"NEWVALUE\"") != std::wstring::npos,
        L"P-02: VirtualRegistry.reg contains written value 'NEWVALUE'");

    // R-13 overwrote Version=1 with Version=2 → stored as dword:00000002
    ASSERT(reg.find(L"dword:00000002") != std::wstring::npos,
        L"P-03: VirtualRegistry.reg contains overwritten Version=dword:00000002");

    // R-14 deleted "TempDelete" → must not appear
    ASSERT(reg.find(L"TEMPDELETE") == std::wstring::npos,
        L"P-04: VirtualRegistry.reg does not contain deleted value 'TEMPDELETE'");

    // Original string value still present (uppercased)
    ASSERT(reg.find(L"\"PLAYERNAME\"") != std::wstring::npos,
        L"P-05: VirtualRegistry.reg still contains 'PLAYERNAME'");
}

// ============================================================
// Entry point
// ============================================================

int wmain()
{
    std::wstring exeDir        = GetExeDir();
    std::wstring testTmpDir    = GetTestTempDir();
    std::wstring interposerDir = exeDir + L".interposer\\";
    std::wstring logsDir       = interposerDir + L"Logs\\";
    std::wstring yamlPath      = interposerDir + L"Config.yml";
    std::wstring regPath       = interposerDir + L"Registry.reg";
    std::wstring dllPath       = exeDir + L"LANCommander.Interposer.dll";

    wprintf(L"LANCommander.Interposer Integration Tests\n");
    wprintf(L"DLL : %ls\n", dllPath.c_str());
    wprintf(L"Dir : %ls\n\n", interposerDir.c_str());

    // ── Setup (before LoadLibraryW) ───────────────────────────────────────────
    // Clear stale logs from prior runs so FindFirstLogFile finds only the current session.
    ClearLogFiles(logsDir);

    // Create the redirect-target directory and a file for redirect tests.
    std::wstring redirectDir    = testTmpDir + L"Redirected\\";
    std::wstring redirectTarget = redirectDir + L"profile.dat";
    CreateDirs(redirectDir);
    WriteTextFile(redirectTarget, "redirect_target_content");

    // .interposer\Config.yml must be in place before LoadLibraryW
    // (LoadConfig runs in DLL_PROCESS_ATTACH, before MH_EnableHook)
    CreateDirs(interposerDir);
    WriteInterposerYaml(yamlPath, redirectDir);

    // .interposer\Registry.reg must be in place before LoadLibraryW
    // (InstallRegistryHooks → LoadRegFile runs before MH_EnableHook)
    WriteVirtualReg(regPath);

    // ── Load DLL ─────────────────────────────────────────────────────────────
    HMODULE hDll = LoadLibraryW(dllPath.c_str());
    if (!hDll)
    {
        wprintf(L"[ERROR] LoadLibraryW failed (GLE=%lu)\n"
                L"        Build the solution before running tests.\n",
                GetLastError());
        return 1;
    }
    wprintf(L"DLL loaded successfully. Running tests...\n");

    // ── Tests (hooks active) ─────────────────────────────────────────────────
    RunFileTests(exeDir, testTmpDir);
    RunRegistryTests();

    // ── Unload DLL ───────────────────────────────────────────────────────────
    // DLL_PROCESS_DETACH fires: RemoveRegistryHooks (SaveRegFile if dirty),
    // then MH_DisableHook + MH_Uninitialize, then CloseLog.
    FreeLibrary(hDll);
    hDll = nullptr;

    // ── Post-unload tests ─────────────────────────────────────────────────────
    // Discover the timestamped log created by LoadConfig during DLL_PROCESS_ATTACH.
    std::wstring logPath = FindFirstLogFile(logsDir);
    RunLogTests(logPath);
    RunPersistenceTests(regPath);

    // ── Cleanup ──────────────────────────────────────────────────────────────
    DeleteFileW(redirectTarget.c_str());
    RemoveDirectoryW(redirectDir.c_str());
    if (!logPath.empty()) DeleteFileW(logPath.c_str());
    RemoveDirectoryW(logsDir.c_str());
    RemoveDirectoryW(testTmpDir.c_str());
    // Leave .interposer/Config.yml and .interposer/Registry.reg for post-run inspection

    // ── Summary ──────────────────────────────────────────────────────────────
    wprintf(L"\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
