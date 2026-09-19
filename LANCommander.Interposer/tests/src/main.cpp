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
#include <shlobj.h>          // SHGetKnownFolderPath / FOLDERID_SavedGames (F-09)
#include <string>
#include <vector>
#include <cstdio>
#include <filesystem>

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

// Environment variable the fixture's %TOKEN% pattern refers to. Set before the
// DLL loads so LoadConfig sees it. A purpose-made variable keeps the test off
// %TEMP%, whose value GetTempPathW does not always agree with.
static const wchar_t* kTokenEnvVar = L"INTERPOSER_TEST_ROOT";

// Independent oracle for %SAVEDGAMES%. The DLL resolves that token out of User
// Shell Folders because it runs under the loader lock; the test EXE is under no
// such constraint, so asking Windows directly is a real cross-check rather than
// a re-implementation of the same lookup.
static std::wstring GetSavedGamesDir()
{
    PWSTR raw = nullptr;

    if (FAILED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &raw)) || !raw)
        return {};

    std::wstring out(raw);
    CoTaskMemFree(raw);
    return out;
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

// Number of log lines containing both substrings. Used to prove a hook fires
// exactly once -- a KernelBase ANSI export that calls its own exported wide
// counterpart would otherwise log the same access twice.
static size_t CountLinesContaining(const std::string& log, const char* a, const char* b)
{
    size_t count = 0;
    size_t pos = 0;

    while (pos < log.size())
    {
        size_t eol = log.find('\n', pos);
        if (eol == std::string::npos) eol = log.size();

        std::string line = log.substr(pos, eol - pos);
        if (line.find(a) != std::string::npos && line.find(b) != std::string::npos)
            ++count;

        pos = eol + 1;
    }

    return count;
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
        "Logging:\n"
        "  Files: true\n"
        "  Registry: true\n"
        "  DirectInput: true\n"
        "  OsVersion: true\n"
        "  Level: Trace\n"
        "\n"
        "DirectInput:\n"
        "  FixLegacyDeviceEnumeration: true\n"
        "  DeviceFilter:\n"
        "    Enabled: true\n"
        "    Classes: ['Mouse', 'Keyboard']\n"
        "    Names: []\n"
        "\n"
        // Spaces and case are deliberate: the name should still resolve to the
        // WindowsServer2003 preset. ServicePack overrides just that one field,
        // and the number in it should carry into wServicePackMajor.
        "OsVersion:\n"
        "  Version: 'windows server 2003'\n"
        "  ServicePack: 'Service Pack 1'\n"
        "\n"
        // Two-file overlay: RegistryBase.reg is a read-only template and
        // Registry.reg layers on top of it and takes every write. Both entries
        // are relative, so this covers resolution against the DLL's directory
        // as well as the overlay itself.
        "Registry:\n"
        "  Files:\n"
        "    - '.interposer\\RegistryBase.reg'\n"
        "    - '.interposer\\Registry.reg'\n"
        "\n"
        "Player:\n"
        "  Username: TestPlayer\n"
        "  ComputerName: TestMachine\n"
        "\n"
        "Redirects:\n"
        "  - Pattern: 'C:\\\\TestGame\\\\Saves\\\\(.+)'\n"
        "    Replacement: '" + WideToUtf8(redirectBaseDir) + "$1'\n"
        // Pattern-side %TOKEN%. wmain sets INTERPOSER_TEST_ROOT to the test temp
        // directory before LoadLibraryW, so the DLL resolves it at parse time and the
        // compiled regex matches the expanded path the caller actually passes.
        "  - Pattern: '%INTERPOSER_TEST_ROOT%\\\\TokenSource\\\\(.+)'\n"
        "    Replacement: '" + WideToUtf8(redirectBaseDir) + "$1'\n"
        // Known-folder token on the replacement side. F-09 checks where this lands
        // against SHGetKnownFolderPath, which the test EXE may call freely -- the DLL
        // cannot, because LoadConfig runs under the loader lock.
        "  - Pattern: 'C:\\\\TestGame\\\\Tokens\\\\(.+)'\n"
        "    Replacement: '%SAVEDGAMES%\\LANCommanderTest\\$1'\n"
        // A token that resolves to nothing is left literal and warned about rather
        // than dropped, so the rule simply never matches (L-21).
        "  - Pattern: '%NOSUCHTOKEN%\\\\Nowhere\\\\(.+)'\n"
        "    Replacement: 'C:\\Nowhere\\$1'\n";
    WriteTextFile(yamlPath, content);
}

// VirtualRegistry.reg placed next to the DLL.
// Key: HKEY_LOCAL_MACHINE\SOFTWARE\TestGame\1.0  (virtual, never touches real registry)
static void WriteVirtualReg(const std::wstring& regPath)
{
    const std::string content =
        "Windows Registry Editor Version 5.00\r\n"
        "\r\n"
        // The game's root key, declared bare. Without it TestGame is not a store
        // key, and InVirtualSpace only relates ancestors and descendants -- so a
        // sibling of 1.0 such as TestGame\DelKey would fall outside the virtual
        // space and a create would try, and fail, against the real HKLM.
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame]\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\1.0]\r\n"
        "\"PlayerName\"=\"TestSoldier\"\r\n"
        "\"Version\"=dword:00000001\r\n"
        "\"BinaryData\"=hex:DE,AD,BE,EF\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\1.0\\SubKey]\r\n"
        "\"SubValue\"=\"InSubKey\"\r\n"
        "\r\n"
        // The write layer's half of the overlay key. Shared is defined in both
        // files, so a read of it must come back with this one.
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\Overlay]\r\n"
        "\"Shared\"=\"FromWrite\"\r\n"
        "\"WriteOnly\"=\"OnlyInWriteLayer\"\r\n"
        "\r\n"
        // RegGetValue's fixtures. Expand is a REG_EXPAND_SZ holding %TEMP%, which
        // is set in every process, so the expanded form is never the literal.
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\Get]\r\n"
        "\"GetOnce\"=\"once\"\r\n"
        "\"Expand\"=hex(2):25,00,54,00,45,00,4d,00,50,00,25,00,00,00\r\n"
        "\r\n"
        // Under HKCU rather than HKLM, so that a handle resolved through
        // BuildPath's NtQueryKey tier has to survive the
        // HKEY_USERS\<SID> -> HKEY_CURRENT_USER rewrite to find it.
        "[HKEY_CURRENT_USER\\SOFTWARE\\TestGame\\User]\r\n"
        "\"Marker\"=\"FromHkcu\"\r\n"
        "\r\n"
        // A key the base layer defines, deleted by this layer before the game
        // ever runs. Pre-seeding it is what lets the reader and writer halves of
        // a key tombstone be tested with no delete hook involved at all.
        "[-HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\Tomb\\Preseeded]\r\n";
    WriteTextFile(regPath, content);
}

// RegistryBase.reg -- the read-only first layer of the overlay, loaded
// underneath Registry.reg. Deliberately kept out of the 1.0 key: R-09 and R-10
// count that key's subkeys and values exactly.
static void WriteRegistryBaseReg(const std::wstring& basePath)
{
    const std::string content =
        "Windows Registry Editor Version 5.00\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\Overlay]\r\n"
        "\"BaseOnly\"=\"FromBase\"\r\n"
        "\"Shared\"=\"FromBase\"\r\n"
        "\"Doomed\"=\"DeleteMe\"\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\BaseKey]\r\n"
        "\"Marker\"=\"OnlyInBaseLayer\"\r\n"
        "\r\n"
        // The key-tombstone fixtures. Tomb carries a bare header of its own so
        // that it is a store key and therefore an ancestor of every child below
        // -- without it a deleted child would not be in the virtual space and a
        // follow-up read would fall through to the real registry, passing for
        // entirely the wrong reason.
        //
        // It needs two surviving children for the same reason: once Tomb\Doomed
        // leaves the flat store, Tomb is what keeps the branch virtual.
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\Tomb]\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\Tomb\\Doomed]\r\n"
        "\"Marker\"=\"TombstoneMe\"\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\Tomb\\Kept]\r\n"
        "\"Marker\"=\"stays\"\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\TestGame\\Tomb\\Preseeded]\r\n"
        "\"Marker\"=\"NeverLoaded\"\r\n";
    WriteTextFile(basePath, content);
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

    // F-08: a %TOKEN% inside a Pattern expands before the regex is compiled
    {
        std::wstring tokenPath = testTmpDir + L"TokenSource\\profile.dat";
        HANDLE h = CreateFileW(tokenPath.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT(h != INVALID_HANDLE_VALUE,
            L"F-08: %%TOKEN%% in a Pattern expands and matches the expanded path");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    {
        std::wstring savedGames = GetSavedGamesDir();

        HANDLE h = CreateFileW(L"C:\\TestGame\\Tokens\\probe.dat", GENERIC_WRITE,
            0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT(h != INVALID_HANDLE_VALUE,
            L"F-09a: write through a %%SAVEDGAMES%% redirect succeeds");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

        ASSERT(!savedGames.empty(),
            L"F-09b: SHGetKnownFolderPath resolves FOLDERID_SavedGames");

        if (!savedGames.empty())
        {
            std::wstring expected = savedGames + L"\\LANCommanderTest\\probe.dat";
            DWORD attr = GetFileAttributesW(expected.c_str());
            ASSERT(attr != INVALID_FILE_ATTRIBUTES,
                L"F-09c: %%SAVEDGAMES%% resolves to the real Saved Games folder");
        }
    }

    {
        SetLastError(0);
        HANDLE h = CreateFileW(L"C:\\Nowhere\\unresolved.dat", GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT(h == INVALID_HANDLE_VALUE,
            L"F-10: rule with an unresolved %%TOKEN%% does not redirect");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    // The regression test for the KernelBase gap. This EXE links the dynamic
    // UCRT, so the open below routes ucrtbase -> api-ms-win-core-file-l1-1-0 ->
    // KernelBase!CreateFileW and never touches the kernel32 thunk. Hooking
    // kernel32 alone leaves the CRT -- and therefore fopen, ifstream and
    // std::filesystem -- completely invisible, so F-11 fails against such a build.
    {
        FILE* f = nullptr;
        errno_t err = _wfopen_s(&f, L"C:\\TestGame\\Saves\\profile.dat", L"rb");

        ASSERT(err == 0 && f != nullptr,
            L"F-11: a CRT open is intercepted and redirected");

        if (f)
        {
            char buf[64] = {};
            size_t read = fread(buf, 1, sizeof(buf) - 1, f);
            ASSERT(read == 23 && std::string(buf) == "redirect_target_content",
                L"F-11: the CRT open reads the redirect target's contents");
            fclose(f);
        }
    }

    // F-12 / F-13: GetFileAttributesEx is what the CRT's _wstat and
    // std::filesystem::file_size call. Both character sets must redirect.
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        BOOL ok = GetFileAttributesExW(L"C:\\TestGame\\Saves\\profile.dat", GetFileExInfoStandard, &data);
        ASSERT(ok, L"F-12: GetFileAttributesExW redirect succeeds");
        ASSERT(ok && data.nFileSizeLow == 23,
            L"F-12: GetFileAttributesExW reports the redirect target's size");
    }

    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        BOOL ok = GetFileAttributesExA("C:\\TestGame\\Saves\\profile.dat", GetFileExInfoStandard, &data);
        ASSERT(ok, L"F-13: GetFileAttributesExA redirect succeeds");
        ASSERT(ok && data.nFileSizeLow == 23,
            L"F-13: GetFileAttributesExA reports the redirect target's size");
    }

    // F-14: the modern open, which msvcp140 imports for std::filesystem.
    {
        HANDLE h = CreateFile2(L"C:\\TestGame\\Saves\\profile.dat", GENERIC_READ, FILE_SHARE_READ,
            OPEN_EXISTING, nullptr);
        ASSERT(h != INVALID_HANDLE_VALUE,
            L"F-14: CreateFile2 redirect opens the redirected file");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    // F-15: the real STL route, whichever of the hooked entry points it picks.
    {
        std::error_code ec;
        bool exists = std::filesystem::exists(L"C:\\TestGame\\Saves\\profile.dat", ec);
        ASSERT(exists, L"F-15: std::filesystem::exists sees the redirected file");

        auto size = std::filesystem::file_size(L"C:\\TestGame\\Saves\\profile.dat", ec);
        ASSERT(!ec && size == 23,
            L"F-15: std::filesystem::file_size reports the redirect target's size");
    }

    // F-16: double-fire probes. Both paths are outside every configured rule, so
    // each logs one plain verb rather than a [FILE REDIRECT] line -- the log tests
    // then assert that "one" is literal. KernelBase's GetFileAttributesA and
    // DeleteFileA call their exported wide counterparts, which are also hooked, so
    // an ANSI hook that used its own trampoline would log each access twice.
    {
        SetLastError(0);
        DWORD attr = GetFileAttributesA("C:\\NoMatch\\path\\dupe-attr.dat");
        ASSERT(attr == INVALID_FILE_ATTRIBUTES,
            L"F-16: GetFileAttributesA on a non-existent, non-matching path fails");

        SetLastError(0);
        BOOL deleted = DeleteFileA("C:\\NoMatch\\path\\dupe-del.dat");
        ASSERT(!deleted,
            L"F-16: DeleteFileA on a non-existent, non-matching path fails");
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

    // ---- The composite APIs: hKey + lpSubKey, opened internally. --------------
    // Every one of these would have reached the real registry before the
    // composite hooks existed, because the internal open hands the nested leaf
    // hook a handle it never tracked.

    const wchar_t* kVersionKey = L"SOFTWARE\\TestGame\\1.0";
    const wchar_t* kGetKey     = L"SOFTWARE\\TestGame\\Get";

    // A real HKCU key for the passthrough assertions. Deliberately not under
    // TestGame: it has to sit outside the virtual space, and InVirtualSpace only
    // relates ancestors and descendants, so diverging at the third segment from
    // HKEY_CURRENT_USER\SOFTWARE\TESTGAME\USER is enough. Removed by
    // CleanRealProbeKey after the DLL is unloaded.
    const wchar_t* kRealProbeKey = L"Software\\LANCommanderInterposerTest";

    // R-19: RegGetValueW against a virtual key, through the subkey path.
    {
        DWORD type = 0, cb = 0;

        LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, kVersionKey, L"PlayerName",
            RRF_RT_ANY, &type, nullptr, &cb);

        ASSERT(st == ERROR_SUCCESS && cb > 0,
            L"R-19a: RegGetValueW size query succeeds (pvData == NULL)");
        ASSERT(type == REG_SZ, L"R-19b: RegGetValueW reports REG_SZ");

        std::vector<BYTE> buf(cb);
        DWORD cb2 = cb;

        st = RegGetValueW(HKEY_LOCAL_MACHINE, kVersionKey, L"PlayerName",
            RRF_RT_ANY, nullptr, buf.data(), &cb2);

        ASSERT(st == ERROR_SUCCESS
            && wcscmp(reinterpret_cast<const wchar_t*>(buf.data()), L"TestSoldier") == 0,
            L"R-19c: RegGetValueW returns the virtual value, not the real one");
    }

    // R-19d: GetOnce is read exactly once, here and nowhere else, because L-36
    // counts the [REG READ] lines it produces to pin the once-only rule on the
    // virtual branch of a composite.
    {
        wchar_t buf[32]{};
        DWORD cb = sizeof(buf);

        LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, kGetKey, L"GetOnce",
            RRF_RT_ANY, nullptr, buf, &cb);

        ASSERT(st == ERROR_SUCCESS && wcscmp(buf, L"once") == 0,
            L"R-19d: RegGetValueW reads a value only this case touches");
    }

    // R-20/R-21: the RRF_RT_* type filter.
    {
        wchar_t buf[64]{};
        DWORD cb = sizeof(buf);

        LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, kVersionKey, L"PlayerName",
            RRF_RT_REG_DWORD, nullptr, buf, &cb);

        ASSERT(st == ERROR_UNSUPPORTED_TYPE,
            L"R-20: a type filter the value does not match is ERROR_UNSUPPORTED_TYPE");

        cb = sizeof(buf);
        st = RegGetValueW(HKEY_LOCAL_MACHINE, kVersionKey, L"PlayerName",
            RRF_RT_REG_SZ, nullptr, buf, &cb);

        ASSERT(st == ERROR_SUCCESS, L"R-21: the matching type filter succeeds");
    }

    // R-22: a buffer too small reports the size it needed.
    {
        BYTE buf[2]{};
        DWORD cb = sizeof(buf);

        LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, kVersionKey, L"PlayerName",
            RRF_RT_ANY, nullptr, buf, &cb);

        ASSERT(st == ERROR_MORE_DATA, L"R-22a: a short buffer is ERROR_MORE_DATA");
        ASSERT(cb > sizeof(buf),      L"R-22b: and pcbData is raised to what was needed");
    }

    // R-23: REG_EXPAND_SZ. Expanded by default and reported as REG_SZ, which is
    // what Windows does; RRF_NOEXPAND hands back the stored form.
    {
        wchar_t buf[512]{};
        DWORD type = 0, cb = sizeof(buf);

        LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, kGetKey, L"Expand",
            RRF_RT_ANY, &type, buf, &cb);

        ASSERT(st == ERROR_SUCCESS && type == REG_SZ && wcscmp(buf, L"%TEMP%") != 0,
            L"R-23a: REG_EXPAND_SZ is expanded and reported as REG_SZ");

        wchar_t raw[512]{};
        type = 0;
        cb   = sizeof(raw);

        st = RegGetValueW(HKEY_LOCAL_MACHINE, kGetKey, L"Expand",
            RRF_NOEXPAND | RRF_RT_REG_EXPAND_SZ, &type, raw, &cb);

        ASSERT(st == ERROR_SUCCESS && type == REG_EXPAND_SZ && wcscmp(raw, L"%TEMP%") == 0,
            L"R-23b: RRF_NOEXPAND returns the unexpanded value and its real type");
    }

    // R-23c: the same call against a *real* REG_EXPAND_SZ value, so that the one
    // fidelity judgement in the RRF_* handling is measured rather than assumed:
    // RRF_RT_ANY has the REG_EXPAND_SZ bit set, and if Windows really rejected
    // that pair then RRF_RT_ANY could never be used on an expandable value at all.
    // kRealProbeKey is outside the virtual space, so this goes to the trampoline.
    {
        HKEY hProbe = nullptr;

        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRealProbeKey, 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &hProbe, nullptr)
                    == ERROR_SUCCESS && hProbe)
        {
            const wchar_t* expandable = L"%TEMP%";

            RegSetValueExW(hProbe, L"RealExpand", 0, REG_EXPAND_SZ,
                reinterpret_cast<const BYTE*>(expandable),
                static_cast<DWORD>((wcslen(expandable) + 1) * sizeof(wchar_t)));

            RegCloseKey(hProbe);

            wchar_t buf[512]{};
            DWORD type = 0, cb = sizeof(buf);

            LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, kRealProbeKey, L"RealExpand",
                RRF_RT_ANY, &type, buf, &cb);

            ASSERT(st == ERROR_SUCCESS && type == REG_SZ,
                L"R-23c: the real RegGetValueW accepts RRF_RT_ANY on a REG_EXPAND_SZ value");
        }
        else
            ASSERT(false, L"R-23c: could not create the real-registry probe key");
    }

    // R-24: the ANSI entry point, including the terminator in the byte count.
    {
        char buf[64]{};
        DWORD cb = sizeof(buf);

        LSTATUS st = RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TestGame\\1.0",
            "PlayerName", RRF_RT_ANY, nullptr, buf, &cb);

        ASSERT(st == ERROR_SUCCESS && strcmp(buf, "TestSoldier") == 0,
            L"R-24a: RegGetValueA returns the virtual value as ANSI");
        ASSERT(cb == strlen("TestSoldier") + 1,
            L"R-24b: the reported size includes the terminator");
    }

    // R-25: a missing value, and RRF_ZEROONFAILURE against the caller's buffer.
    {
        BYTE buf[8];
        memset(buf, 0xCD, sizeof(buf));
        DWORD cb = sizeof(buf);

        LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, kVersionKey, L"NoSuchValue",
            RRF_RT_ANY | RRF_ZEROONFAILURE, nullptr, buf, &cb);

        ASSERT(st == ERROR_FILE_NOT_FOUND,
            L"R-25a: a missing value under a virtual key is ERROR_FILE_NOT_FOUND");

        bool zeroed = true;
        for (BYTE b : buf) if (b != 0) zeroed = false;

        ASSERT(zeroed, L"R-25b: RRF_ZEROONFAILURE zeroes the caller's buffer");
    }

    // R-26: RegSetKeyValueW into an existing virtual key, read back through the
    // composite that would previously have read the real registry.
    {
        const wchar_t* written = L"SetByKeyValue";
        DWORD cb = static_cast<DWORD>((wcslen(written) + 1) * sizeof(wchar_t));

        LSTATUS st = RegSetKeyValueW(HKEY_LOCAL_MACHINE, kGetKey, L"SetKeyValue",
            REG_SZ, written, cb);

        wchar_t buf[64]{};
        DWORD read = sizeof(buf);

        if (st == ERROR_SUCCESS)
            st = RegGetValueW(HKEY_LOCAL_MACHINE, kGetKey, L"SetKeyValue",
                RRF_RT_ANY, nullptr, buf, &read);

        ASSERT(st == ERROR_SUCCESS && wcscmp(buf, written) == 0,
            L"R-26: RegSetKeyValueW writes into the virtual store and reads back");
    }

    // R-27: and it creates the subkey when it is missing, as the real API does.
    {
        DWORD value = 7;

        LSTATUS st = RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Get\\Made",
            L"Made", REG_DWORD, &value, sizeof(value));

        DWORD readBack = 0, cb = sizeof(readBack);

        if (st == ERROR_SUCCESS)
            st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Get\\Made",
                L"Made", RRF_RT_REG_DWORD, nullptr, &readBack, &cb);

        ASSERT(st == ERROR_SUCCESS && readBack == 7,
            L"R-27: RegSetKeyValueW creates the subkey it was given");
    }

    // R-28: RegDeleteKeyValueW takes it away again.
    {
        LSTATUS st = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\Get\\Made", L"Made");

        ASSERT(st == ERROR_SUCCESS, L"R-28a: RegDeleteKeyValueW succeeds");

        DWORD readBack = 0, cb = sizeof(readBack);

        st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Get\\Made",
            L"Made", RRF_RT_ANY, nullptr, &readBack, &cb);

        ASSERT(st == ERROR_FILE_NOT_FOUND,
            L"R-28b: and the value no longer reads back");
    }

    // R-34: RegQueryMultipleValues. Implemented on NtQueryMultipleValueKey, so
    // nothing about it reaches a leaf hook.
    {
        HKEY hk34 = nullptr;

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVersionKey, 0, KEY_READ, &hk34)
                == ERROR_SUCCESS && hk34)
        {
            wchar_t namePlayer[]  = L"PlayerName";
            wchar_t nameVersion[] = L"Version";

            VALENTW entries[2]{};
            entries[0].ve_valuename = namePlayer;
            entries[1].ve_valuename = nameVersion;

            DWORD total = 0;
            LSTATUS st = RegQueryMultipleValuesW(hk34, entries, 2, nullptr, &total);

            ASSERT(st == ERROR_MORE_DATA && total > 0,
                L"R-34a: a null buffer reports the total size as ERROR_MORE_DATA");

            std::vector<BYTE> buf(total);
            DWORD size = total;

            st = RegQueryMultipleValuesW(hk34, entries, 2,
                reinterpret_cast<LPWSTR>(buf.data()), &size);

            ASSERT(st == ERROR_SUCCESS, L"R-34b: the real pass succeeds");
            ASSERT(entries[0].ve_type == REG_SZ && entries[1].ve_type == REG_DWORD,
                L"R-34c: each entry comes back with its stored type");

            bool stringOk = st == ERROR_SUCCESS && entries[0].ve_valueptr != 0
                && wcscmp(reinterpret_cast<const wchar_t*>(entries[0].ve_valueptr),
                          L"TestSoldier") == 0;

            ASSERT(stringOk, L"R-34d: ve_valueptr points at the value inside the buffer");
            ASSERT(size == entries[0].ve_valuelen + entries[1].ve_valuelen,
                L"R-34e: ldwTotsize is the sum of the entry lengths");

            RegCloseKey(hk34);
        }
        else
            ASSERT(false, L"R-34a: could not open the key for RegQueryMultipleValues");
    }

    // ---- The delete family. ----------------------------------------------------
    // RegDeleteKeyW was the total miss: advapi32 implements it with NtOpenKey and
    // NtDeleteKey inline, so before this it reached no hook of ours at all.

    auto createKey = [](const wchar_t* subKey) -> bool
    {
        HKEY hk = nullptr;

        LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &hk, nullptr);

        if (hk) RegCloseKey(hk);

        return st == ERROR_SUCCESS;
    };

    // R-29: a key with a subkey cannot be deleted. Windows reaches this through
    // NtDeleteKey's STATUS_CANNOT_DELETE, which surfaces as ERROR_ACCESS_DENIED.
    {
        createKey(L"SOFTWARE\\TestGame\\DelKey");
        createKey(L"SOFTWARE\\TestGame\\DelKey\\Child");

        LSTATUS st = RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\DelKey");

        ASSERT(st == ERROR_ACCESS_DENIED,
            L"R-29: RegDeleteKeyW on a key with subkeys is ERROR_ACCESS_DENIED");
    }

    // R-30: the childless one goes, and only once.
    {
        LSTATUS st = RegDeleteKeyW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\DelKey\\Child");

        ASSERT(st == ERROR_SUCCESS, L"R-30a: RegDeleteKeyW deletes a childless key");

        st = RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\DelKey\\Child");

        ASSERT(st == ERROR_FILE_NOT_FOUND,
            L"R-30b: deleting it again is ERROR_FILE_NOT_FOUND, not a passthrough");

        // Deliberately not asserting on a re-open: HookRegOpenKeyExW succeeds for
        // any path in the virtual space whether or not the exact key is in the
        // store, which is existing and intended behaviour. The enumeration is what
        // actually shows the key is gone.
        HKEY hParent = nullptr;

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\DelKey",
                0, KEY_READ, &hParent) == ERROR_SUCCESS && hParent)
        {
            wchar_t name[256];
            DWORD cch = ARRAYSIZE(name);

            st = RegEnumKeyExW(hParent, 0, name, &cch, nullptr, nullptr, nullptr, nullptr);

            RegCloseKey(hParent);
        }

        ASSERT(st == ERROR_NO_MORE_ITEMS,
            L"R-30c: and the parent now enumerates no subkeys");
    }

    // R-31: RegDeleteKeyEx, with a WOW64 view the virtual store does not have.
    {
        createKey(L"SOFTWARE\\TestGame\\DelKeyEx");

        LSTATUS st = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\DelKeyEx",
            KEY_WOW64_64KEY, 0);

        ASSERT(st == ERROR_SUCCESS,
            L"R-31: RegDeleteKeyExW deletes the key and ignores the WOW64 view");
    }

    // R-32: RegDeleteTreeW with a subkey path takes the named key and everything
    // under it.
    {
        createKey(L"SOFTWARE\\TestGame\\Tree");
        createKey(L"SOFTWARE\\TestGame\\Tree\\A");
        createKey(L"SOFTWARE\\TestGame\\Tree\\A\\B");

        DWORD marker = 1;
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Tree\\A\\B",
            L"Deep", REG_DWORD, &marker, sizeof(marker));

        LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Tree");

        ASSERT(st == ERROR_SUCCESS, L"R-32a: RegDeleteTreeW succeeds");

        DWORD readBack = 0, cb = sizeof(readBack);

        st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Tree\\A\\B",
            L"Deep", RRF_RT_ANY, nullptr, &readBack, &cb);

        ASSERT(st == ERROR_FILE_NOT_FOUND,
            L"R-32b: a value three levels down the deleted tree is gone");
    }

    // R-33: the other shape of the same API. A null lpSubKey empties hKey but
    // leaves hKey itself in place -- getting this backwards would silently delete
    // a key the game expects to still be there.
    {
        createKey(L"SOFTWARE\\TestGame\\Tree2");
        createKey(L"SOFTWARE\\TestGame\\Tree2\\C");

        DWORD marker = 1;
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Tree2",
            L"Surface", REG_DWORD, &marker, sizeof(marker));

        HKEY hTree2 = nullptr;
        LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Tree2",
            0, KEY_ALL_ACCESS, &hTree2);

        if (st == ERROR_SUCCESS && hTree2)
        {
            st = RegDeleteTreeW(hTree2, nullptr);

            ASSERT(st == ERROR_SUCCESS,
                L"R-33a: RegDeleteTreeW with a null subkey succeeds");

            DWORD subKeys = 0, values = 0;

            LSTATUS info = RegQueryInfoKeyW(hTree2, nullptr, nullptr, nullptr,
                &subKeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);

            ASSERT(info == ERROR_SUCCESS && subKeys == 0 && values == 0,
                L"R-33b: the key is left with no subkeys and no values");

            RegCloseKey(hTree2);
        }
        else
            ASSERT(false, L"R-33a: could not open Tree2");

        // ...and the key itself survives, which is the whole point.
        HKEY hGame = nullptr;
        bool sawTree2 = false, sawTree = false;

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame", 0, KEY_READ, &hGame)
                == ERROR_SUCCESS && hGame)
        {
            wchar_t name[256];

            for (DWORD i = 0; ; ++i)
            {
                DWORD cch = ARRAYSIZE(name);

                if (RegEnumKeyExW(hGame, i, name, &cch, nullptr, nullptr, nullptr, nullptr)
                        != ERROR_SUCCESS)
                    break;

                if (_wcsicmp(name, L"Tree2") == 0) sawTree2 = true;
                if (_wcsicmp(name, L"Tree") == 0)  sawTree  = true;
            }

            RegCloseKey(hGame);
        }

        ASSERT(sawTree2 && !sawTree,
            L"R-33c: a null subkey empties the key but does not delete it, unlike R-32");
    }

    // ---- Hive roots and handles we did not issue. -------------------------------

    // R-35: RegOpenCurrentUser hands back a hive root. Before it was hooked the
    // handle was untracked, so every key opened through it -- including virtual
    // ones -- went to the real registry.
    {
        HKEY hUser = nullptr;
        LSTATUS st = RegOpenCurrentUser(KEY_READ, &hUser);

        ASSERT(st == ERROR_SUCCESS && hUser != nullptr,
            L"R-35a: RegOpenCurrentUser succeeds");

        std::wstring value;

        if (hUser)
        {
            HKEY hGameUser = nullptr;

            if (RegOpenKeyExW(hUser, L"SOFTWARE\\TestGame\\User", 0, KEY_READ, &hGameUser)
                    == ERROR_SUCCESS && hGameUser)
            {
                wchar_t buf[64]{};
                DWORD cb = sizeof(buf);

                if (RegQueryValueExW(hGameUser, L"Marker", nullptr, nullptr,
                        reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS)
                    value = buf;

                RegCloseKey(hGameUser);
            }

            RegCloseKey(hUser);
        }

        ASSERT(value == L"FromHkcu",
            L"R-35b: a virtual key opened through that root is served from the store");
    }

    // R-36: BuildPath's NtQueryKey tier. A duplicated handle is a perfectly valid
    // key handle that g_realHandles has never seen, which is the same position a
    // handle the game opened before injection is in. R-36 proves the tier did not
    // break passthrough; L-42 proves it actually resolved the path.
    {
        HKEY hSystem = nullptr;
        LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM", 0, KEY_READ, &hSystem);

        HANDLE hDuplicate = nullptr;

        if (st == ERROR_SUCCESS && hSystem)
            DuplicateHandle(GetCurrentProcess(), hSystem, GetCurrentProcess(),
                &hDuplicate, 0, FALSE, DUPLICATE_SAME_ACCESS);

        HKEY hControlSet = nullptr;

        st = hDuplicate
            ? RegOpenKeyExW(reinterpret_cast<HKEY>(hDuplicate), L"CurrentControlSet",
                  0, KEY_READ, &hControlSet)
            : ERROR_INVALID_HANDLE;

        ASSERT(st == ERROR_SUCCESS,
            L"R-36: a subkey opens through an untracked handle (passthrough intact)");

        if (hControlSet) RegCloseKey(hControlSet);
        if (hDuplicate)  CloseHandle(hDuplicate);
        if (hSystem)     RegCloseKey(hSystem);
    }

    // R-36b: the same tier against HKCU, which is where the HKEY_USERS\<SID>
    // rewrite earns its place -- NtQueryKey reports the SID form, and the store is
    // keyed on HKEY_CURRENT_USER. TierFour does not exist, so this only has to be
    // logged, not to succeed; L-43 is the assertion that matters.
    {
        HKEY hProbe = nullptr;

        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRealProbeKey, 0, KEY_READ, &hProbe)
                == ERROR_SUCCESS && hProbe)
        {
            HANDLE hDuplicate = nullptr;

            DuplicateHandle(GetCurrentProcess(), hProbe, GetCurrentProcess(),
                &hDuplicate, 0, FALSE, DUPLICATE_SAME_ACCESS);

            if (hDuplicate)
            {
                HKEY hTierFour = nullptr;

                RegOpenKeyExW(reinterpret_cast<HKEY>(hDuplicate), L"TierFour",
                    0, KEY_READ, &hTierFour);

                if (hTierFour) RegCloseKey(hTierFour);

                CloseHandle(hDuplicate);
            }

            RegCloseKey(hProbe);

            ASSERT(true, L"R-36b: an HKCU subkey open through an untracked handle is logged");
        }
        else
            ASSERT(false, L"R-36b: could not open the real-registry probe key");
    }

    // R-37: RegFlushKey on a virtual handle. Never reaches the trampoline, which
    // would reject our heap-pointer HKEY with ERROR_INVALID_HANDLE.
    {
        HKEY hk37 = nullptr;
        LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVersionKey, 0, KEY_READ, &hk37);

        if (st == ERROR_SUCCESS && hk37)
        {
            st = RegFlushKey(hk37);
            RegCloseKey(hk37);
        }

        ASSERT(st == ERROR_SUCCESS, L"R-37: RegFlushKey on a virtual handle succeeds");
    }

    // R-38: change notification. Registering succeeds and the event is never
    // signalled, because nothing outside this process can change the store.
    {
        HKEY hk38 = nullptr;
        HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVersionKey, 0, KEY_NOTIFY, &hk38);

        if (st == ERROR_SUCCESS && hk38)
        {
            st = RegNotifyChangeKeyValue(hk38, TRUE, REG_NOTIFY_CHANGE_LAST_SET,
                hEvent, TRUE);

            ASSERT(st == ERROR_SUCCESS,
                L"R-38a: RegNotifyChangeKeyValue on a virtual handle registers");

            ASSERT(WaitForSingleObject(hEvent, 0) == WAIT_TIMEOUT,
                L"R-38b: and the event is not signalled");

            RegCloseKey(hk38);
        }
        else
            ASSERT(false, L"R-38a: could not open the key for RegNotifyChangeKeyValue");

        if (hEvent) CloseHandle(hEvent);
    }

    // R-39: RegOpenKeyTransactedW. A null transaction would fail the real API, so
    // this passes only because the virtual branch ignores the transaction -- which
    // is exactly the semantic being pinned, with no dependency on ktmw32.
    {
        HKEY hk39 = nullptr;
        LSTATUS st = RegOpenKeyTransactedW(HKEY_LOCAL_MACHINE, kVersionKey, 0, KEY_READ,
            &hk39, nullptr, nullptr);

        std::wstring value;

        if (st == ERROR_SUCCESS && hk39)
        {
            wchar_t buf[64]{};
            DWORD cb = sizeof(buf);

            if (RegQueryValueExW(hk39, L"PlayerName", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS)
                value = buf;

            RegCloseKey(hk39);
        }

        ASSERT(st == ERROR_SUCCESS && value == L"TestSoldier",
            L"R-39: RegOpenKeyTransactedW serves a virtual key, ignoring the transaction");
    }

    // R-40: and the transacted create and delete round-trip the same way.
    {
        HKEY hk40 = nullptr;
        DWORD disposition = 0;

        LSTATUS st = RegCreateKeyTransactedW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\Transacted", 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_ALL_ACCESS, nullptr, &hk40, &disposition, nullptr, nullptr);

        ASSERT(st == ERROR_SUCCESS && disposition == REG_CREATED_NEW_KEY,
            L"R-40a: RegCreateKeyTransactedW creates the key in the store");

        if (hk40) RegCloseKey(hk40);

        st = RegDeleteKeyTransactedW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\Transacted", 0, 0, nullptr, nullptr);

        ASSERT(st == ERROR_SUCCESS,
            L"R-40b: RegDeleteKeyTransactedW deletes it again");
    }

    // ---- RegCopyTreeW, in the three combinations that touch the store. ---------

    auto openVirtual = [](const wchar_t* subKey) -> HKEY
    {
        HKEY hk = nullptr;

        RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_ALL_ACCESS, nullptr, &hk, nullptr);

        return hk;
    };

    // R-41: virtual -> virtual, including a child one level down.
    {
        const wchar_t* top = L"CopiedTop";

        RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\CopySrc", L"Top",
            REG_SZ, top, static_cast<DWORD>((wcslen(top) + 1) * sizeof(wchar_t)));

        const wchar_t* deep = L"CopiedDeep";

        RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\CopySrc\\Child",
            L"Deep", REG_SZ, deep,
            static_cast<DWORD>((wcslen(deep) + 1) * sizeof(wchar_t)));

        HKEY hDst = openVirtual(L"SOFTWARE\\TestGame\\CopyDst");

        LSTATUS st = hDst
            ? RegCopyTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\CopySrc", hDst)
            : ERROR_INVALID_HANDLE;

        if (hDst) RegCloseKey(hDst);

        ASSERT(st == ERROR_SUCCESS, L"R-41a: RegCopyTreeW virtual to virtual succeeds");

        wchar_t buf[64]{};
        DWORD cb = sizeof(buf);

        st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\CopyDst", L"Top",
            RRF_RT_ANY, nullptr, buf, &cb);

        ASSERT(st == ERROR_SUCCESS && wcscmp(buf, top) == 0,
            L"R-41b: the root's values arrive at the destination");

        wchar_t deepBuf[64]{};
        cb = sizeof(deepBuf);

        st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\CopyDst\\Child",
            L"Deep", RRF_RT_ANY, nullptr, deepBuf, &cb);

        ASSERT(st == ERROR_SUCCESS && wcscmp(deepBuf, deep) == 0,
            L"R-41c: and so does the subkey one level down");
    }

    // R-42: real -> virtual. The genuinely useful direction: seeding a virtual key
    // from whatever the machine already has. kRealProbeKey is outside the virtual
    // space, so the source walk goes through the trampolines.
    {
        const wchar_t* probe = L"FromRealRegistry";

        RegSetKeyValueW(HKEY_CURRENT_USER, kRealProbeKey, L"RealProbe", REG_SZ, probe,
            static_cast<DWORD>((wcslen(probe) + 1) * sizeof(wchar_t)));

        HKEY hDst = openVirtual(L"SOFTWARE\\TestGame\\Seeded");

        LSTATUS st = hDst
            ? RegCopyTreeW(HKEY_CURRENT_USER, kRealProbeKey, hDst)
            : ERROR_INVALID_HANDLE;

        if (hDst) RegCloseKey(hDst);

        ASSERT(st == ERROR_SUCCESS, L"R-42a: RegCopyTreeW real to virtual succeeds");

        wchar_t buf[64]{};
        DWORD cb = sizeof(buf);

        st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Seeded",
            L"RealProbe", RRF_RT_ANY, nullptr, buf, &cb);

        ASSERT(st == ERROR_SUCCESS && wcscmp(buf, probe) == 0,
            L"R-42b: a real value is readable out of the virtual store afterwards");
    }

    // R-43: virtual -> real, and then RegDeleteTreeW over the real result, which is
    // the passthrough branch of the new delete hook.
    {
        std::wstring realDest = std::wstring(kRealProbeKey) + L"\\CopyDest";

        HKEY hDst = nullptr;
        LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, realDest.c_str(), 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &hDst, nullptr);

        if (st == ERROR_SUCCESS && hDst)
        {
            st = RegCopyTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\CopySrc", hDst);

            RegCloseKey(hDst);
        }

        ASSERT(st == ERROR_SUCCESS, L"R-43a: RegCopyTreeW virtual to real succeeds");

        wchar_t buf[64]{};
        DWORD cb = sizeof(buf);

        st = RegGetValueW(HKEY_CURRENT_USER, realDest.c_str(), L"Top",
            RRF_RT_ANY, nullptr, buf, &cb);

        ASSERT(st == ERROR_SUCCESS && wcscmp(buf, L"CopiedTop") == 0,
            L"R-43b: the value really landed in the real registry");

        st = RegDeleteTreeW(HKEY_CURRENT_USER, realDest.c_str());

        ASSERT(st == ERROR_SUCCESS,
            L"R-43c: RegDeleteTreeW removes it again through the passthrough branch");

        cb = sizeof(buf);

        st = RegGetValueW(HKEY_CURRENT_USER, realDest.c_str(), L"Top",
            RRF_RT_ANY, nullptr, buf, &cb);

        ASSERT(st != ERROR_SUCCESS, L"R-43d: and it is gone");
    }
}

// ============================================================
// Registry overlay tests  (run while DLL is loaded → hooks active)
// ============================================================
// Registry.Files stacks RegistryBase.reg (read-only) under Registry.reg
// (writable). These cover the read-side merge and set up the mutations that
// RunOverlayPersistenceTests checks against both files after unload.

static void RunOverlayTests()
{
    wprintf(L"\n--- Registry Overlay Tests ---\n");

    HKEY hk = nullptr;

    LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\TestGame\\Overlay", 0, KEY_READ | KEY_WRITE, &hk);

    ASSERT(st == ERROR_SUCCESS && hk != nullptr,
        L"O-01: a key defined only by the base layer opens as virtual");

    if (!hk)
    {
        wprintf(L"  SKIP remaining overlay tests: key open failed\n");
        return;
    }

    auto readString = [&](const wchar_t* name, std::wstring& out) -> LSTATUS
    {
        wchar_t buf[256]{};
        DWORD cb = sizeof(buf);
        LSTATUS r = RegQueryValueExW(hk, name, nullptr, nullptr,
            reinterpret_cast<LPBYTE>(buf), &cb);
        if (r == ERROR_SUCCESS) out = buf;
        return r;
    };

    // O-02: a value only the base layer defines is still readable.
    {
        std::wstring value;
        LSTATUS r = readString(L"BaseOnly", value);
        ASSERT(r == ERROR_SUCCESS && value == L"FromBase",
            L"O-02: a value only the base layer defines reads back");
    }

    // O-03: the whole point of the ordering -- the last file wins.
    {
        std::wstring value;
        LSTATUS r = readString(L"Shared", value);
        ASSERT(r == ERROR_SUCCESS && value == L"FromWrite",
            L"O-03: a value both layers define reads back from the last file");
    }

    // O-04: and the writable layer's own values are unaffected by the merge.
    {
        std::wstring value;
        LSTATUS r = readString(L"WriteOnly", value);
        ASSERT(r == ERROR_SUCCESS && value == L"OnlyInWriteLayer",
            L"O-04: a value only the write layer defines reads back");
    }

    // O-05: a key that exists in no other layer is virtual too.
    {
        HKEY hBase = nullptr;
        LSTATUS r = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\BaseKey", 0, KEY_READ, &hBase);
        ASSERT(r == ERROR_SUCCESS && hBase != nullptr,
            L"O-05: a key present only in the base layer is in the virtual space");
        if (hBase) RegCloseKey(hBase);
    }

    // Mutations checked after unload by RunOverlayPersistenceTests.
    {
        const wchar_t* written = L"WrittenIntoOverlay";
        DWORD cb = static_cast<DWORD>((wcslen(written) + 1) * sizeof(wchar_t));
        LSTATUS r = RegSetValueExW(hk, L"RuntimeValue", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(written), cb);
        ASSERT(r == ERROR_SUCCESS,
            L"O-06: writing into a base-layer key succeeds");
    }

    // O-07: deleting a base-layer value hides it for the rest of the session.
    {
        LSTATUS r = RegDeleteValueW(hk, L"Doomed");
        ASSERT(r == ERROR_SUCCESS,
            L"O-07a: deleting a value inherited from the base layer succeeds");

        DWORD sz = 0;
        r = RegQueryValueExW(hk, L"Doomed", nullptr, nullptr, nullptr, &sz);
        ASSERT(r == ERROR_FILE_NOT_FOUND,
            L"O-07b: the deleted base-layer value no longer reads back");
    }

    // O-13: the write layer's pre-seeded [-Key] header. No hook is involved --
    // this is purely the reader honouring regedit's delete-the-key marker.
    {
        HKEY hTomb = nullptr;
        LSTATUS r = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\Tomb\\Preseeded", 0, KEY_READ, &hTomb);

        if (r == ERROR_SUCCESS && hTomb)
        {
            wchar_t buf[256]{};
            DWORD cb = sizeof(buf);

            r = RegQueryValueExW(hTomb, L"Marker", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(buf), &cb);

            RegCloseKey(hTomb);
        }

        ASSERT(r == ERROR_FILE_NOT_FOUND,
            L"O-13a: a key deleted by a [-Key] header in the write layer is gone");
    }

    // O-13b: and it is gone from the enumeration too, not just from reads.
    {
        HKEY hTomb = nullptr;
        bool sawPreseeded = false, sawKept = false;

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Tomb",
                0, KEY_READ, &hTomb) == ERROR_SUCCESS && hTomb)
        {
            wchar_t name[256];

            for (DWORD i = 0; ; ++i)
            {
                DWORD cch = ARRAYSIZE(name);

                if (RegEnumKeyExW(hTomb, i, name, &cch, nullptr, nullptr, nullptr,
                        nullptr) != ERROR_SUCCESS)
                    break;

                if (_wcsicmp(name, L"Preseeded") == 0) sawPreseeded = true;
                if (_wcsicmp(name, L"Kept") == 0)      sawKept      = true;
            }

            RegCloseKey(hTomb);
        }

        ASSERT(!sawPreseeded && sawKept,
            L"O-13b: the tombstoned key is not enumerated, its sibling still is");
    }

    // ---- Key deletes against a read-only base layer. --------------------------

    // O-15: a childless key that only the base layer defines can still be deleted.
    {
        LSTATUS r = RegDeleteKeyW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\Tomb\\Doomed");

        ASSERT(r == ERROR_SUCCESS,
            L"O-15: deleting a key defined only by the base layer succeeds");
    }

    // O-16: and it is gone from the store, not merely absent from the real
    // registry. Tomb's own bare header keeps the branch in the virtual space, so
    // this read cannot fall through and pass for the wrong reason.
    {
        wchar_t buf[64]{};
        DWORD cb = sizeof(buf);

        LSTATUS r = RegGetValueW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\Tomb\\Doomed", L"Marker",
            RRF_RT_ANY, nullptr, buf, &cb);

        ASSERT(r == ERROR_FILE_NOT_FOUND,
            L"O-16: the deleted base-layer key no longer reads back");
    }

    // O-17: the base-layer variant of R-29 -- Tomb still has Kept under it.
    {
        LSTATUS r = RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\Tomb");

        ASSERT(r == ERROR_ACCESS_DENIED,
            L"O-17: a base-layer key with a surviving child cannot be deleted");
    }

    // O-18: re-create it and write something of our own. The tombstone has to
    // survive this, or the base layer's other values would come back with the key.
    {
        const wchar_t* reborn = L"yes";

        LSTATUS r = RegSetKeyValueW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\TestGame\\Tomb\\Doomed", L"Reborn", REG_SZ, reborn,
            static_cast<DWORD>((wcslen(reborn) + 1) * sizeof(wchar_t)));

        ASSERT(r == ERROR_SUCCESS,
            L"O-18: a tombstoned key can be re-created and written to");
    }

    RegCloseKey(hk);
}

// Post-unload: the base layer must be untouched and the write layer must carry
// the session's changes -- and only those.
static void RunOverlayPersistenceTests(const std::wstring& basePath,
                                       const std::wstring& regPath)
{
    wprintf(L"\n--- Registry Overlay Persistence Tests ---\n");

    std::wstring base  = ReadFileAsWide(basePath);
    std::wstring write = ReadFileAsWide(regPath);

    ASSERT(base.find(L"\"Doomed\"") != std::wstring::npos
        && base.find(L"FromBase") != std::wstring::npos,
        L"O-08: the base layer is left exactly as it was found");

    ASSERT(base.find(L"RuntimeValue") == std::wstring::npos,
        L"O-09: nothing written at runtime lands in the base layer");

    ASSERT(write.find(L"WRITTENINTOOVERLAY") != std::wstring::npos
        || write.find(L"WrittenIntoOverlay") != std::wstring::npos,
        L"O-10: the runtime write lands in the last file");

    // The merged store still holds BaseOnly, but serializing it here would
    // freeze a stale duplicate on top of the file it came from.
    ASSERT(write.find(L"BASEONLY") == std::wstring::npos,
        L"O-11: a value inherited from the base layer is not copied down");

    // Dropping it silently would not stick -- the base file hands it back on
    // the next load -- so it is recorded as a regedit delete marker instead.
    ASSERT(write.find(L"\"DOOMED\"=-") != std::wstring::npos,
        L"O-12: deleting a base-layer value writes a tombstone to the last file");

    // The key-level counterpart: the pre-seeded [-Key] header has to survive
    // being read in and written back out, or the delete would last exactly one
    // session and the base layer would hand the key back on the next launch.
    ASSERT(write.find(L"[-HKEY_LOCAL_MACHINE\\SOFTWARE\\TESTGAME\\TOMB\\PRESEEDED]")
        != std::wstring::npos,
        L"O-14: a [-Key] tombstone read from the write layer is written back out");

    // O-19: O-15's delete of a base-layer key is recorded the same way.
    const std::wstring doomedKey =
        L"HKEY_LOCAL_MACHINE\\SOFTWARE\\TESTGAME\\TOMB\\DOOMED";

    ASSERT(write.find(L"[-" + doomedKey + L"]") != std::wstring::npos,
        L"O-19: deleting a base-layer key writes a [-Key] tombstone");

    // O-20: and O-18's re-creation puts a section back, *after* the tombstone.
    // The reader applies them in file order, so the delete wipes what the base
    // layer put there and the section re-adds only what this layer owns. Note
    // that "[-X]" does not contain "[X]", so the two searches cannot collide.
    {
        size_t tomb    = write.find(L"[-" + doomedKey + L"]");
        size_t section = write.find(L"[" + doomedKey + L"]");

        ASSERT(tomb != std::wstring::npos && section != std::wstring::npos && tomb < section,
            L"O-20: the [-Key] header precedes the section that re-creates the key");
    }

    ASSERT(write.find(L"REBORN") != std::wstring::npos,
        L"O-21a: the value written into the re-created key is in the write layer");

    ASSERT(write.find(L"TOMBSTONEME") == std::wstring::npos,
        L"O-21b: the base layer's value did not come back with the key");

    ASSERT(base.find(L"TombstoneMe") != std::wstring::npos,
        L"O-22: and the base layer still defines it, untouched");
}

// ============================================================
// Identity hook tests  (run while DLL is loaded → hooks active)
// ============================================================

static void RunIdentityTests()
{
    wprintf(L"\n--- Identity Hook Tests ---\n");

    // I-01: GetUserNameW returns the configured username
    {
        wchar_t buf[256]{};
        DWORD len = _countof(buf);
        BOOL ok = GetUserNameW(buf, &len);
        ASSERT(ok == TRUE,
            L"I-01a: GetUserNameW returns TRUE");
        ASSERT(wcscmp(buf, L"TestPlayer") == 0,
            L"I-01b: GetUserNameW returns configured username");
        // len should include the null terminator
        ASSERT(len == static_cast<DWORD>(wcslen(L"TestPlayer") + 1),
            L"I-01c: GetUserNameW sets pcbBuffer to length including null terminator");
    }

    // I-02: GetUserNameA returns the configured username
    {
        char buf[256]{};
        DWORD len = sizeof(buf);
        BOOL ok = GetUserNameA(buf, &len);
        ASSERT(ok == TRUE,
            L"I-02a: GetUserNameA returns TRUE");
        ASSERT(strcmp(buf, "TestPlayer") == 0,
            L"I-02b: GetUserNameA returns configured username");
    }

    // I-03: Size query (NULL buffer) reports ERROR_INSUFFICIENT_BUFFER
    {
        DWORD len = 0;
        SetLastError(0);
        BOOL ok = GetUserNameW(nullptr, &len);
        ASSERT(ok == FALSE,
            L"I-03a: GetUserNameW size query returns FALSE");
        ASSERT(GetLastError() == ERROR_INSUFFICIENT_BUFFER,
            L"I-03b: GetLastError is ERROR_INSUFFICIENT_BUFFER");
        ASSERT(len == static_cast<DWORD>(wcslen(L"TestPlayer") + 1),
            L"I-03c: reported length includes null terminator");
    }

    // I-04: GetComputerNameW returns the configured computer name
    {
        wchar_t buf[256]{};
        DWORD len = _countof(buf);
        BOOL ok = GetComputerNameW(buf, &len);
        ASSERT(ok == TRUE,
            L"I-04a: GetComputerNameW returns TRUE");
        ASSERT(wcscmp(buf, L"TestMachine") == 0,
            L"I-04b: GetComputerNameW returns configured computer name");
        // On success *nSize = character count WITHOUT null terminator
        ASSERT(len == static_cast<DWORD>(wcslen(L"TestMachine")),
            L"I-04c: GetComputerNameW sets nSize to length without null terminator");
    }

    // I-05: GetComputerNameA returns the configured computer name
    {
        char buf[256]{};
        DWORD len = sizeof(buf);
        BOOL ok = GetComputerNameA(buf, &len);
        ASSERT(ok == TRUE,
            L"I-05a: GetComputerNameA returns TRUE");
        ASSERT(strcmp(buf, "TestMachine") == 0,
            L"I-05b: GetComputerNameA returns configured computer name");
    }

    // I-06: Size query (NULL buffer) reports ERROR_BUFFER_OVERFLOW
    {
        DWORD len = 0;
        SetLastError(0);
        BOOL ok = GetComputerNameW(nullptr, &len);
        ASSERT(ok == FALSE,
            L"I-06a: GetComputerNameW size query returns FALSE");
        ASSERT(GetLastError() == ERROR_BUFFER_OVERFLOW,
            L"I-06b: GetLastError is ERROR_BUFFER_OVERFLOW");
        ASSERT(len == static_cast<DWORD>(wcslen(L"TestMachine") + 1),
            L"I-06c: reported length includes null terminator");
    }
}

// ============================================================
// OS version hook tests  (run while DLL is loaded → hooks active)
// ============================================================
//
// The fixture asks for "windows server 2003" — spaced and lowercased, so this
// also covers the loose name matching — and overrides ServicePack alone. Every
// entry point therefore has to agree on 5.2.3790, server product type, with the
// numbers coming from the preset and the service pack from the override.

// RtlGetVersion is not declared in <windows.h>; RTL_OSVERSIONINFOEXW is
// layout-identical to OSVERSIONINFOEXW, which is what the hook relies on too.
using TestPfnRtlGetVersion = LONG(NTAPI*)(OSVERSIONINFOEXW*);

// GetVersion and GetVersionEx are deprecated, and this project builds warnings
// as errors. Spoofing them is the entire point of the subsystem under test.
#pragma warning(push)
#pragma warning(disable : 4996)

static void RunOsVersionTests()
{
    wprintf(L"\n--- OS Version Hook Tests ---\n");

    // V-01: GetVersionExA fills a plain OSVERSIONINFOA
    {
        OSVERSIONINFOA info{};
        info.dwOSVersionInfoSize = sizeof(info);

        BOOL ok = GetVersionExA(&info);

        ASSERT(ok == TRUE,
            L"V-01a: GetVersionExA returns TRUE");
        ASSERT(info.dwMajorVersion == 5 && info.dwMinorVersion == 2,
            L"V-01b: GetVersionExA reports 5.2");
        ASSERT(info.dwBuildNumber == 3790,
            L"V-01c: GetVersionExA reports build 3790 from the preset");
        ASSERT(info.dwPlatformId == VER_PLATFORM_WIN32_NT,
            L"V-01d: GetVersionExA reports VER_PLATFORM_WIN32_NT");
        ASSERT(strcmp(info.szCSDVersion, "Service Pack 1") == 0,
            L"V-01e: GetVersionExA reports the overridden service pack name");
    }

    // V-02: GetVersionExA fills the EX fields when given the larger struct.
    // This is the shape the crash in BFME2 came from - a caller that reads
    // szCSDVersion out of a struct the OS never wrote.
    {
        OSVERSIONINFOEXA info{};
        info.dwOSVersionInfoSize = sizeof(info);

        BOOL ok = GetVersionExA(reinterpret_cast<LPOSVERSIONINFOA>(&info));

        ASSERT(ok == TRUE,
            L"V-02a: GetVersionExA (EX) returns TRUE");
        ASSERT(info.wServicePackMajor == 1 && info.wServicePackMinor == 0,
            L"V-02b: GetVersionExA (EX) derives wServicePackMajor from the overridden name");
        ASSERT(info.wProductType == VER_NT_SERVER,
            L"V-02c: GetVersionExA (EX) reports the configured product type");
    }

    // V-03: GetVersionExW agrees with the ANSI variant
    {
        OSVERSIONINFOEXW info{};
        info.dwOSVersionInfoSize = sizeof(info);

        BOOL ok = GetVersionExW(reinterpret_cast<LPOSVERSIONINFOW>(&info));

        ASSERT(ok == TRUE,
            L"V-03a: GetVersionExW returns TRUE");
        ASSERT(info.dwMajorVersion == 5 && info.dwMinorVersion == 2 && info.dwBuildNumber == 3790,
            L"V-03b: GetVersionExW reports 5.2.3790");
        ASSERT(wcscmp(info.szCSDVersion, L"Service Pack 1") == 0,
            L"V-03c: GetVersionExW reports the overridden service pack name");
        ASSERT(info.wServicePackMajor == 1,
            L"V-03d: GetVersionExW reports service pack major 1");
    }

    // V-04: GetVersion returns the packed form of the same version.
    // 3790 = 0x0ECE, so the expected value is 0x0ECE0205 with the top bit
    // clear to mark the platform as NT.
    {
        DWORD packed = GetVersion();

        ASSERT(LOBYTE(LOWORD(packed)) == 5,
            L"V-04a: GetVersion packs major version 5");
        ASSERT(HIBYTE(LOWORD(packed)) == 2,
            L"V-04b: GetVersion packs minor version 2");
        ASSERT(HIWORD(packed) == 3790,
            L"V-04c: GetVersion packs build 3790");
        ASSERT((packed & 0x80000000) == 0,
            L"V-04d: GetVersion leaves the top bit clear (NT platform)");
    }

    // V-05: RtlGetVersion is spoofed too, which an AppCompat layer does not do
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto rtlGetVersion = reinterpret_cast<TestPfnRtlGetVersion>(
            ntdll ? GetProcAddress(ntdll, "RtlGetVersion") : nullptr);

        ASSERT(rtlGetVersion != nullptr,
            L"V-05a: RtlGetVersion resolves from ntdll");

        if (rtlGetVersion)
        {
            OSVERSIONINFOEXW info{};
            info.dwOSVersionInfoSize = sizeof(info);

            LONG status = rtlGetVersion(&info);

            ASSERT(status == 0,
                L"V-05b: RtlGetVersion returns STATUS_SUCCESS");
            ASSERT(info.dwMajorVersion == 5 && info.dwMinorVersion == 2 && info.dwBuildNumber == 3790,
                L"V-05c: RtlGetVersion reports 5.2.3790");
        }
    }

    // V-06: a struct sized for neither shape falls through to the real API
    // rather than being filled with a guess.
    {
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = 12; // neither sizeof(OSVERSIONINFOW) nor the EX size

        SetLastError(0);
        BOOL ok = GetVersionExW(&info);

        ASSERT(ok == FALSE,
            L"V-06a: GetVersionExW rejects a bad dwOSVersionInfoSize");
        ASSERT(info.dwMajorVersion == 0,
            L"V-06b: GetVersionExW leaves a rejected struct untouched");
    }
}

#pragma warning(pop)

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

    // --- Redirect diagnostics (Logging.Level: Trace in the fixture) ---

    // F-02 redirects C:\TestGame\Saves\profile.dat via the single configured rule.
    ASSERT(log.find("[REDIRECT HIT]") != std::string::npos,
        L"L-13: Log contains [REDIRECT HIT] entry");
    ASSERT(log.find("rule #1") != std::string::npos,
        L"L-14: [REDIRECT HIT] names the rule that fired");

    // F-01 and F-03 open paths no rule matches.
    ASSERT(log.find("[REDIRECT MISS]") != std::string::npos,
        L"L-15: Log contains [REDIRECT MISS] entry");
    ASSERT(log.find("none matched") != std::string::npos,
        L"L-16: [REDIRECT MISS] reports how many rules were evaluated");

    // Trace level lists each pattern evaluated and rejected.
    ASSERT(log.find("[REDIRECT RULE]") != std::string::npos,
        L"L-17: Log contains [REDIRECT RULE] entry at Trace level");

    // A pattern rewritten by %TOKEN% substitution reports both forms, so the
    // author can see what they wrote next to what the regex actually compiled.
    ASSERT(log.find("as written:") != std::string::npos,
        L"L-25: [REDIRECT HIT] shows the pattern as written when a token expanded");
    ASSERT(log.find("%INTERPOSER_TEST_ROOT%") != std::string::npos,
        L"L-26: the as-written form still carries the unexpanded token");

    // The fixture's last rule names a token nothing can resolve. LoadConfig
    // defers the warning until the log exists, then emits it regardless of the
    // logging flags.
    ASSERT(log.find("[FILEREDIRECT]") != std::string::npos,
        L"L-27: Log contains [FILEREDIRECT] entry");
    ASSERT(log.find("unresolved %NOSUCHTOKEN%") != std::string::npos,
        L"L-28: unresolved token is reported by name");

    // --- Hook placement (KernelBase-first) ---
    //
    // These assume Windows 7 or later, where KernelBase exists and the kernel32
    // and advapi32 exports are thunks into it. On an older system the hooks fall
    // back and these four would read kernel32!/advapi32! instead -- the same
    // machine assumption the L-DI and L-AU blocks already make.

    ASSERT(log.find("kernelbase!CreateFileW") != std::string::npos,
        L"L-29: CreateFileW is hooked in KernelBase, not on the kernel32 thunk");
    ASSERT(log.find("kernelbase!CreateFile2") != std::string::npos,
        L"L-30: CreateFile2 is hooked");
    ASSERT(log.find("kernelbase!GetFileAttributesExW") != std::string::npos,
        L"L-31: GetFileAttributesExW is hooked");
    ASSERT(log.find("kernelbase!RegOpenKeyExW") != std::string::npos,
        L"L-32: RegOpenKeyExW is hooked in KernelBase, not on the advapi32 thunk");

    // Pinned deliberately. KernelBase's LoadLibraryW tail-jumps onto the exported
    // LoadLibraryExW, so hooking the loader family there would fire [DLL LOAD],
    // the plugin callbacks and OnLibraryLoaded twice per load. A future
    // consistency refactor should trip this test rather than ship that quietly.
    ASSERT(log.find("kernel32!LoadLibraryW") != std::string::npos,
        L"L-33: the loader family stays on kernel32");

    // F-16 probed two paths no rule matches, so each must appear exactly once.
    ASSERT(CountLinesContaining(log, "[FILE ATTR]", "dupe-attr.dat") == 1,
        L"L-34: GetFileAttributesA logs one [FILE ATTR] line, not two");
    ASSERT(CountLinesContaining(log, "[FILE DELETE]", "dupe-del.dat") == 1,
        L"L-35: DeleteFileA logs one [FILE DELETE] line, not two");

    // R-01 opens HKLM\SOFTWARE\TestGame\1.0, which is in the virtual store.
    ASSERT(log.find("[REG HIT]") != std::string::npos,
        L"L-18: Log contains [REG HIT] entry");

    // R-18 opens HKLM\SOFTWARE\Microsoft, which is not in the virtual store.
    ASSERT(log.find("[REG MISS]") != std::string::npos,
        L"L-19: Log contains [REG MISS] entry");
    ASSERT(log.find("not in virtual space") != std::string::npos,
        L"L-20: [REG MISS] reports why the key was not redirected");

    // --- DirectInput (DirectInput.FixLegacyDeviceEnumeration + DeviceFilter + Logging.DirectInput) ---

    ASSERT(log.find("dinput.dll!DirectInputCreateA") != std::string::npos,
        L"L-DI-1: Log records the dinput.dll!DirectInputCreateA hook installation");
    ASSERT(log.find("[DINPUT BRIDGE]") != std::string::npos,
        L"L-DI-2: Log contains [DINPUT BRIDGE] entry");
    ASSERT(log.find("[DINPUT ENUM]") != std::string::npos,
        L"L-DI-3: Log contains [DINPUT ENUM] entry");
    ASSERT(log.find("[DINPUT DEVICE]") != std::string::npos,
        L"L-DI-4: Log contains [DINPUT DEVICE] entry");
    ASSERT(log.find("dinput8.dll!DirectInput8Create") != std::string::npos,
        L"L-DI-5: Log records the dinput8.dll!DirectInput8Create hook installation");

    // Logging.Level is Trace in the fixture, so the per-device listing is on.
    ASSERT(log.find("[DINPUT FOUND]") != std::string::npos,
        L"L-DI-6: Log lists devices found in enumeration at Debug level");
    ASSERT(log.find("class=Mouse") != std::string::npos,
        L"L-DI-7: [DINPUT FOUND] reports the device class");
    ASSERT(log.find("found,") != std::string::npos,
        L"L-DI-8: [DINPUT ENUM] summarises how many devices were found and shown");

    // M-03 set a data format declaring X/Y/Z at 0/4/8; the SetDataFormat detour
    // reads those back out so a plugin transform can identify the axes.
    ASSERT(log.find("[DINPUT FORMAT]") != std::string::npos,
        L"L-DI-9: Log records the mouse data format at Debug level");
    ASSERT(log.find("X=0  Y=4  Z=8") != std::string::npos,
        L"L-DI-10: [DINPUT FORMAT] reports the axis offsets the game declared");

    ASSERT(log.find("kernel32!GetVersionExA") != std::string::npos,
        L"L-OV-1: Log records the kernel32!GetVersionExA hook installation");
    ASSERT(log.find("ntdll!RtlGetVersion") != std::string::npos,
        L"L-OV-2: Log records the ntdll!RtlGetVersion hook installation");
    ASSERT(log.find("[OSVERSION]") != std::string::npos,
        L"L-OV-3: Log contains [OSVERSION] entry");
    // The startup line names the preset the loose name resolved to, which is
    // the only place that resolution is visible to the user.
    ASSERT(log.find("WindowsServer2003") != std::string::npos,
        L"L-OV-4: [OSVERSION] names the resolved preset");
    ASSERT(log.find("5.2.3790 Service Pack 1") != std::string::npos,
        L"L-OV-5: [OSVERSION] reports the version the game is being given");

    // R-06 queries NoSuchValue under a key that IS virtual — the partial-miss case
    // that is indistinguishable from a successful read at Info level.
    ASSERT(log.find("[REG PARTIAL]") != std::string::npos,
        L"L-21: Log contains [REG PARTIAL] entry for a missing value in a virtual key");

    // ---- The composite hooks log exactly once per call. -----------------------
    // A composite whose passthrough branch reaches a leaf hook has to stay silent
    // there, or one call by the game would produce two lines. These two counts are
    // what pin that, one per branch.

    // R-19 reads GetOnce once, on the virtual branch.
    ASSERT(CountLinesContaining(log, "[REG READ]", "GetOnce") == 1,
        L"L-36: one RegGetValueW on a virtual value logs exactly one [REG READ]");

    // R-23c reads RealExpand once, on the passthrough branch, where the nested
    // RegQueryValueExW hook is the one that logs. A value only this suite creates,
    // so no unrelated read in the process can make the count flaky.
    ASSERT(CountLinesContaining(log, "[REG READ]", "RealExpand") == 1,
        L"L-37: one passthrough RegGetValueW also logs exactly one [REG READ]");

    // The composites live in KernelBase, not on advapi32's jmp [IAT] thunk --
    // the same placement rule L-32 pins for RegOpenKeyExW.
    ASSERT(log.find("kernelbase!RegGetValueW") != std::string::npos,
        L"L-38: RegGetValueW is hooked in KernelBase, not on the advapi32 thunk");

    ASSERT(log.find("kernelbase!RegQueryMultipleValuesW") != std::string::npos,
        L"L-39: RegQueryMultipleValuesW is hooked in KernelBase");

    // RegDeleteKeyW is the one entry point KernelBase does not export at all, so
    // it is the one that has to take HookRegistryApi's advapi32 fallback.
    ASSERT(log.find("advapi32!RegDeleteKeyW") != std::string::npos,
        L"L-40: RegDeleteKeyW is hooked on advapi32, which is the only module with it");

    // A key delete logs one line naming the key, with no value name appended --
    // which is how a .NET or plugin consumer tells it from a value delete.
    ASSERT(CountLinesContaining(log, "[REG DELETE]", "TOMB\\DOOMED") == 1,
        L"L-41: deleting a key logs exactly one [REG DELETE] naming the key");

    // R-36 opened CurrentControlSet through a duplicated -- and therefore
    // untracked -- handle. A resolved path in the log can only have come from
    // BuildPath's NtQueryKey tier; before it, this line read "(unknown)".
    ASSERT(log.find("HKEY_LOCAL_MACHINE\\SYSTEM\\CURRENTCONTROLSET") != std::string::npos,
        L"L-42: an untracked handle is resolved through NtQueryKey, not logged as unknown");

    // R-36b did the same under HKCU, where NtQueryKey reports HKEY_USERS\<SID>.
    // Seeing the HKEY_CURRENT_USER form is what proves the rewrite ran -- and
    // without it the tier would never match the hive most games write to.
    ASSERT(log.find("HKEY_CURRENT_USER\\SOFTWARE\\LANCOMMANDERINTERPOSERTEST\\TIERFOUR")
        != std::string::npos,
        L"L-43: HKEY_USERS\\<SID> is rewritten to HKEY_CURRENT_USER for the store");

    // R-37's flush. The new diagnostic verbs are log-only: a flush is not an
    // access, so it never reaches the plugin or .NET consumers.
    ASSERT(log.find("[REG FLUSH]") != std::string::npos,
        L"L-44: Log contains the [REG FLUSH] diagnostic");

    // R-41..R-43's copies. One line naming source and destination, deliberately
    // not one [REG WRITE] per copied value -- a tree copy would flood both the log
    // and the plugin callbacks.
    ASSERT(log.find("[REG COPY]") != std::string::npos,
        L"L-45: Log contains the [REG COPY] diagnostic");
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

    // R-26 wrote through RegSetKeyValueW, which reaches the writable layer by the
    // same route RegSetValueExW does.
    ASSERT(reg.find(L"SETKEYVALUE") != std::wstring::npos,
        L"P-06: a RegSetKeyValueW write is persisted to the write layer");

    // R-27 created Get\Made and R-28 emptied it again. An empty key still needs
    // its bare header, or it would not exist on the next launch.
    ASSERT(reg.find(L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\TESTGAME\\GET\\MADE]")
        != std::wstring::npos,
        L"P-07: a created key whose last value was deleted still round-trips");

    // ...and no key tombstone for it: nothing in a base layer would hand it back,
    // so there is nothing to suppress. This is what gates the g_baseKeys check.
    ASSERT(reg.find(L"[-HKEY_LOCAL_MACHINE\\SOFTWARE\\TESTGAME\\GET") == std::wstring::npos,
        L"P-08: a key no base layer defines needs no [-Key] tombstone");

    // R-32 deleted a whole tree it had created itself. Same gate, whole subtree:
    // no base layer defines any of it, so none of it needs suppressing.
    ASSERT(reg.find(L"[-HKEY_LOCAL_MACHINE\\SOFTWARE\\TESTGAME\\TREE") == std::wstring::npos,
        L"P-09: RegDeleteTreeW over runtime-created keys writes no tombstones");
}


// ============================================================
// DirectInput bridge tests
// ============================================================

// Minimal slices of the DirectInput ABI. The full SDK header is not needed and
// dinput.h would collide with the DLL's own subsystem header.
struct TestDeviceInstanceA
{
    DWORD dwSize;
    GUID  guidInstance;
    GUID  guidProduct;
    DWORD dwDevType;
    CHAR  tszInstanceName[MAX_PATH];
    CHAR  tszProductName[MAX_PATH];
    GUID  guidFFDriver;
    WORD  wUsagePage;
    WORD  wUsage;
};

struct TestDeviceInstanceW
{
    DWORD dwSize;
    GUID  guidInstance;
    GUID  guidProduct;
    DWORD dwDevType;
    WCHAR tszInstanceName[MAX_PATH];
    WCHAR tszProductName[MAX_PATH];
    GUID  guidFFDriver;
    WORD  wUsagePage;
    WORD  wUsage;
};

using TestPfnCreateA  = HRESULT (WINAPI*)(HINSTANCE, DWORD, void**, void*);
using TestPfnCreate8  = HRESULT (WINAPI*)(HINSTANCE, DWORD, const GUID*, void**, void*);
using TestPfnRelease  = ULONG   (WINAPI*)(void*);
using TestPfnEnumCb   = BOOL    (WINAPI*)(const TestDeviceInstanceA*, void*);
using TestPfnEnum     = HRESULT (WINAPI*)(void*, DWORD, TestPfnEnumCb, void*, DWORD);
using TestPfnEnumCbW  = BOOL    (WINAPI*)(const TestDeviceInstanceW*, void*);
using TestPfnEnumW    = HRESULT (WINAPI*)(void*, DWORD, TestPfnEnumCbW, void*, DWORD);
using TestPfnCreateDev = HRESULT (WINAPI*)(void*, const GUID*, void**, void*);
using TestPfnQI       = HRESULT (WINAPI*)(void*, const GUID*, void**);

static const GUID kTestGuidSysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID kTestGuidSysKeyboard =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
// The IID a DirectInput 7 game asks a fresh device for, because that is where
// Poll() lives. A DirectInput 8 object answers E_NOINTERFACE without the bridge.
static const GUID kTestIidDevice2A =
    { 0x5944E682, 0xC92E, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID kTestIidDirectInput8A =
    { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUID kTestIidDirectInput8W =
    { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };

struct EnumTally
{
    int  count               = 0;
    bool sawMouse            = false;
    bool sawKeyboard         = false;
    bool onlyMouseOrKeyboard = true;
};

// Low byte of dwDevType: DirectInput 3/7 numbers mouse 2 / keyboard 3, and
// DirectInput 8 numbers them 0x12 / 0x13.
static bool IsMouseOrKeyboardType(DWORD devType)
{
    const DWORD type = devType & 0xFF;

    return type == 2 || type == 3 || type == 0x12 || type == 0x13;
}

static BOOL WINAPI TestEnumCallback(const TestDeviceInstanceA* instance, void* ref)
{
    EnumTally* tally = static_cast<EnumTally*>(ref);

    if (!instance || instance->dwSize != sizeof(TestDeviceInstanceA))
        return TRUE;

    ++tally->count;

    if (memcmp(&instance->guidInstance, &kTestGuidSysMouse, sizeof(GUID)) == 0)
        tally->sawMouse = true;
    else if (memcmp(&instance->guidInstance, &kTestGuidSysKeyboard, sizeof(GUID)) == 0)
        tally->sawKeyboard = true;

    if (!IsMouseOrKeyboardType(instance->dwDevType))
        tally->onlyMouseOrKeyboard = false;

    return TRUE; // DIENUM_CONTINUE
}

static BOOL WINAPI TestEnumCallbackW(const TestDeviceInstanceW* instance, void* ref)
{
    EnumTally* tally = static_cast<EnumTally*>(ref);

    if (!instance || instance->dwSize != sizeof(TestDeviceInstanceW))
        return TRUE;

    ++tally->count;

    if (!IsMouseOrKeyboardType(instance->dwDevType))
        tally->onlyMouseOrKeyboard = false;

    return TRUE;
}

static void* TestVtSlot(void* object, int index)
{
    return (*static_cast<void***>(object))[index];
}

static void RunDirectInputTests(HMODULE hDInput)
{
    wprintf(L"\n--- DirectInput Bridge Tests ---\n");

    if (!hDInput)
    {
        wprintf(L"  SKIP  dinput.dll could not be loaded; bridge tests skipped\n");
        return;
    }

    TestPfnCreateA create = reinterpret_cast<TestPfnCreateA>(
        reinterpret_cast<void*>(GetProcAddress(hDInput, "DirectInputCreateA")));

    if (!create)
    {
        wprintf(L"  SKIP  dinput.dll has no DirectInputCreateA; bridge tests skipped\n");
        return;
    }

    // D-01: a DirectInput 7 request is served by the bridge instead of the
    // legacy implementation. Without the hook this call is what hangs.
    void* di = nullptr;
    HRESULT hr = create(GetModuleHandleW(nullptr), 0x0700, &di, nullptr);

    ASSERT(SUCCEEDED(hr) && di != nullptr, L"D-01: DirectInputCreateA(0x0700) succeeds");

    if (FAILED(hr) || !di)
        return;

    // D-02/D-03: EnumDevices is synthesized, so it returns immediately with
    // exactly the system mouse and keyboard rather than walking the HID stack.
    EnumTally tally;
    TestPfnEnum enumDevices = reinterpret_cast<TestPfnEnum>(TestVtSlot(di, 4));

    hr = enumDevices(di, 0 /* all classes */, TestEnumCallback, &tally, 0);

    ASSERT(SUCCEEDED(hr), L"D-02: EnumDevices succeeds");
    ASSERT(tally.sawMouse && tally.sawKeyboard && tally.count == 2,
        L"D-03: EnumDevices reports exactly the system mouse and keyboard");

    // D-04: CreateDevice still goes to DirectInput 8 for real.
    void* device = nullptr;
    TestPfnCreateDev createDevice = reinterpret_cast<TestPfnCreateDev>(TestVtSlot(di, 3));

    hr = createDevice(di, &kTestGuidSysMouse, &device, nullptr);

    ASSERT(SUCCEEDED(hr) && device != nullptr, L"D-04: CreateDevice(GUID_SysMouse) succeeds");

    if (SUCCEEDED(hr) && device)
    {
        // D-05: the pre-DX8 device IID is aliased back to the same object. If
        // this regresses, a real game keeps working menus and loses all
        // in-game input.
        void* device2 = nullptr;
        TestPfnQI deviceQI = reinterpret_cast<TestPfnQI>(TestVtSlot(device, 0));

        hr = deviceQI(device, &kTestIidDevice2A, &device2);

        ASSERT(SUCCEEDED(hr) && device2 == device,
            L"D-05: QueryInterface(IID_IDirectInputDevice2A) aliases the same object");

        if (SUCCEEDED(hr) && device2)
            reinterpret_cast<TestPfnRelease>(TestVtSlot(device2, 2))(device2);

        reinterpret_cast<TestPfnRelease>(TestVtSlot(device, 2))(device);
    }

    // Released before the Interposer unloads: the bridge reverts its vtable
    // patches on detach, but the object itself belongs to dinput8.
    reinterpret_cast<TestPfnRelease>(TestVtSlot(di, 2))(di);
}

// ─── Mouse transform API ──────────────────────────────────────────────────────

typedef struct TestInputEvent {
    DWORD dwOfs;
    LONG  data;
    DWORD timeStamp;
    DWORD sequence;
} TestInputEvent;

#define TEST_MOUSE_AXIS_NONE 0xFFFFFFFFu

typedef struct TestMouseBatch {
    DWORD            structSize;
    DWORD            axisOffsetX;
    DWORD            axisOffsetY;
    DWORD            axisOffsetZ;
    TestInputEvent*  events;
    DWORD            count;
    DWORD            capacity;
} TestMouseBatch;

// The transform the Interposer calls back INTO is __stdcall; the registration
// export it provides is __cdecl, like every other Interposer export. Declaring
// the latter __stdcall leaks its arguments on every call on x86 and eventually
// trips /GS with STATUS_STACK_BUFFER_OVERRUN.
using TestPfnMouseTransform    = void (WINAPI*)(TestMouseBatch*, void*);
using TestPfnRegisterTransform = BOOL (*)(TestPfnMouseTransform, void*);
using TestPfnSetDataFormat     = HRESULT (WINAPI*)(void*, const void*);

static void WINAPI TestMouseTransformCallback(TestMouseBatch*, void*) {}

// A minimal but valid DIDATAFORMAT describing the standard mouse layout:
// three axes at 0/4/8 and four buttons at 12..15. Hand-built because the SDK's
// c_dfDIMouse lives in dinput8.lib, which the test EXE does not link.
static const GUID kTestGuidXAxis =
    { 0xA36D02E0, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID kTestGuidYAxis =
    { 0xA36D02E1, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID kTestGuidZAxis =
    { 0xA36D02E2, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

struct TestObjectDataFormat
{
    const GUID* pguid;
    DWORD       dwOfs;
    DWORD       dwType;
    DWORD       dwFlags;
};

struct TestDataFormat
{
    DWORD                 dwSize;
    DWORD                 dwObjSize;
    DWORD                 dwFlags;
    DWORD                 dwDataSize;
    DWORD                 dwNumObjs;
    TestObjectDataFormat* rgodf;
};

// DIDFT_AXIS | DIDFT_ANYINSTANCE, DIDFT_BUTTON | DIDFT_ANYINSTANCE
static constexpr DWORD kTestAxisType   = 0x00000003 | 0x00FFFF00;
static constexpr DWORD kTestButtonType = 0x0000000C | 0x00FFFF00;

// The Interposer routes the system mouse through a plugin transform, learning
// the axis offsets from whatever data format the game sets. That plumbing is
// what these cover; the transform's own behaviour lives in the Mouse plugin.
static void RunMouseTransformTests(HMODULE hDll, HMODULE hDInput)
{
    wprintf(L"\n--- Mouse Transform API Tests ---\n");

    auto pfnRegister = reinterpret_cast<TestPfnRegisterTransform>(
        reinterpret_cast<void*>(GetProcAddress(hDll, "InterposerRegisterMouseTransform")));

    ASSERT(pfnRegister != nullptr, L"M-01: InterposerRegisterMouseTransform is exported");

    if (!pfnRegister)
        return;

    ASSERT(pfnRegister(TestMouseTransformCallback, nullptr) != FALSE,
        L"M-02: registering a mouse transform succeeds");

    // Drive a real bridged mouse device so the SetDataFormat detour runs and
    // the axis offsets are learned from the format below.
    if (!hDInput)
    {
        wprintf(L"  SKIP  dinput.dll unavailable; axis learning not exercised\n");
        pfnRegister(nullptr, nullptr);
        return;
    }

    auto create = reinterpret_cast<TestPfnCreateA>(
        reinterpret_cast<void*>(GetProcAddress(hDInput, "DirectInputCreateA")));

    void* di = nullptr;

    if (!create || FAILED(create(GetModuleHandleW(nullptr), 0x0700, &di, nullptr)) || !di)
    {
        wprintf(L"  SKIP  could not create a DirectInput object\n");
        pfnRegister(nullptr, nullptr);
        return;
    }

    void* device = nullptr;
    TestPfnCreateDev createDevice = reinterpret_cast<TestPfnCreateDev>(TestVtSlot(di, 3));
    HRESULT hr = createDevice(di, &kTestGuidSysMouse, &device, nullptr);

    if (SUCCEEDED(hr) && device)
    {
        TestObjectDataFormat objects[] = {
            { &kTestGuidXAxis, 0,  kTestAxisType,   0 },
            { &kTestGuidYAxis, 4,  kTestAxisType,   0 },
            { &kTestGuidZAxis, 8,  kTestAxisType,   0 },
            { nullptr,         12, kTestButtonType, 0 },
            { nullptr,         13, kTestButtonType, 0 },
            { nullptr,         14, kTestButtonType, 0 },
            { nullptr,         15, kTestButtonType, 0 },
        };

        TestDataFormat format{};
        format.dwSize     = sizeof(TestDataFormat);
        format.dwObjSize  = sizeof(TestObjectDataFormat);
        format.dwFlags    = 2;  // DIDF_ABSAXIS
        format.dwDataSize = 16; // sizeof(DIMOUSESTATE)
        format.dwNumObjs  = ARRAYSIZE(objects);
        format.rgodf      = objects;

        auto setFormat = reinterpret_cast<TestPfnSetDataFormat>(TestVtSlot(device, 11));

        hr = setFormat(device, &format);

        ASSERT(SUCCEEDED(hr), L"M-03: SetDataFormat on the bridged mouse succeeds");

        reinterpret_cast<TestPfnRelease>(TestVtSlot(device, 2))(device);
    }
    else
    {
        ASSERT(false, L"M-03: CreateDevice(GUID_SysMouse) for the format test succeeds");
    }

    reinterpret_cast<TestPfnRelease>(TestVtSlot(di, 2))(di);

    // Leave the input path clean for anything that runs after this.
    pfnRegister(nullptr, nullptr);
}

// A game that uses DirectInput 8 natively never touches the bridge, but the
// device filter still applies to it — DirectInput8Create is hooked separately.
static void RunDirectInput8Tests(HMODULE hDInput8)
{
    wprintf(L"\n--- DirectInput 8 Filter Tests ---\n");

    if (!hDInput8)
    {
        wprintf(L"  SKIP  dinput8.dll could not be loaded; filter tests skipped\n");
        return;
    }

    TestPfnCreate8 create = reinterpret_cast<TestPfnCreate8>(
        reinterpret_cast<void*>(GetProcAddress(hDInput8, "DirectInput8Create")));

    if (!create)
    {
        wprintf(L"  SKIP  dinput8.dll has no DirectInput8Create; filter tests skipped\n");
        return;
    }

    // DF-01/DF-02: ANSI. The fixture's filter admits only Mouse and Keyboard, so
    // the enumeration is answered directly rather than run.
    void* di = nullptr;
    HRESULT hr = create(GetModuleHandleW(nullptr), 0x0800, &kTestIidDirectInput8A, &di, nullptr);

    ASSERT(SUCCEEDED(hr) && di != nullptr, L"DF-01: DirectInput8Create(IID_IDirectInput8A) succeeds");

    if (SUCCEEDED(hr) && di)
    {
        EnumTally tally;
        TestPfnEnum enumDevices = reinterpret_cast<TestPfnEnum>(TestVtSlot(di, 4));

        hr = enumDevices(di, 0, TestEnumCallback, &tally, 0);

        ASSERT(SUCCEEDED(hr) && tally.sawMouse && tally.sawKeyboard && tally.count == 2,
            L"DF-02: native DX8 ANSI enumeration is filtered to mouse and keyboard");

        reinterpret_cast<TestPfnRelease>(TestVtSlot(di, 2))(di);
    }

    // DF-03/DF-04: Unicode. There is no shortcut for the W interface, so this
    // runs a real DirectInput 8 enumeration and filters the results — the path
    // that exercises class matching against live devices.
    //
    // This is the slow test. A real enumeration opens and classifies the whole
    // HID stack and can take ~20 s on a machine with many devices, which is
    // precisely the cost the mouse/keyboard-only shortcut exists to avoid. It
    // is kept because it is the only end-to-end proof that filtering works
    // against real hardware rather than a synthesised pair.
    void* diW = nullptr;

    hr = create(GetModuleHandleW(nullptr), 0x0800, &kTestIidDirectInput8W, &diW, nullptr);

    ASSERT(SUCCEEDED(hr) && diW != nullptr, L"DF-03: DirectInput8Create(IID_IDirectInput8W) succeeds");

    if (SUCCEEDED(hr) && diW)
    {
        EnumTally tally;
        TestPfnEnumW enumDevices = reinterpret_cast<TestPfnEnumW>(TestVtSlot(diW, 4));

        hr = enumDevices(diW, 0, TestEnumCallbackW, &tally, 0);

        // Every surviving device must be a mouse or keyboard. The machine may
        // legitimately have none of one kind, so the count is not asserted.
        ASSERT(SUCCEEDED(hr) && tally.onlyMouseOrKeyboard,
            L"DF-04: native DX8 Unicode enumeration yields only mouse/keyboard class devices");

        wprintf(L"        (%d devices passed the filter)\n", tally.count);

        reinterpret_cast<TestPfnRelease>(TestVtSlot(diW, 2))(diW);
    }
}

// ============================================================
// Isolated-mode tests  (separate process)
// ============================================================
// Registry.Isolated is process-global: it makes every key virtual, which is
// flatly incompatible with R-18's real-registry passthrough. Two DLL instances
// cannot share a process either -- both would detour the same advapi32 exports
// -- so the mode is exercised by re-running this EXE with --isolated against a
// second copy of the DLL in a directory of its own, and therefore an
// .interposer tree of its own.

// A key that exists in the real registry on every Windows install, with a value
// under it. Isolated mode must hide both.
static const wchar_t* kRealKey   = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion";
static const wchar_t* kRealValue = L"ProgramFilesDir";

// A key no machine has, used to prove a write went to the file and not to HKCU.
static const wchar_t* kNovelKey  = L"Software\\LANCommanderInterposerIsolationProbe";

// The same, for the composite write that goes through RegSetKeyValueW.
static const wchar_t* kNovelKey2 = L"Software\\LANCommanderInterposerIsolationProbe2";

// A key the child creates in the *real* registry before it loads the DLL, so that
// the delete hooks have something real to fail to destroy.
static const wchar_t* kVictimKey = L"Software\\LANCommanderInterposerIsolationVictim";

// Written by the parent before the child starts. Files names one relative path,
// so the child's store resolves to <childDir>\\.interposer\\Registry.reg.
static void WriteIsolatedFixtures(const std::wstring& childInterposerDir)
{
    WriteTextFile(childInterposerDir + L"Config.yml",
        "Logging:\n"
        "  Registry: true\n"
        "  Level: Debug\n"
        "\n"
        "Registry:\n"
        "  Isolated: true\n"
        "  Files:\n"
        "    - '.interposer\\Registry.reg'\n");

    // Deliberately does not mention either key the child probes: the point is
    // what happens to keys the file has never heard of.
    WriteTextFile(childInterposerDir + L"Registry.reg",
        "Windows Registry Editor Version 5.00\r\n"
        "\r\n"
        "[HKEY_CURRENT_USER\\Software\\IsolationSeed]\r\n"
        "\"Seeded\"=\"yes\"\r\n");
}

// Runs in the child process. Returns the number of failed assertions.
static int RunIsolatedChild(const wchar_t* childDirArg)
{
    // The parent passes the directory without a trailing separator: one at the
    // end of a quoted command-line argument escapes the closing quote instead.
    std::wstring childDir(childDirArg);

    if (!childDir.empty() && childDir.back() != L'\\')
        childDir += L'\\';

    std::wstring dllPath = childDir + L"LANCommander.Interposer.dll";
    std::wstring regPath = childDir + L".interposer\\Registry.reg";

    wprintf(L"\n--- Registry Isolation Tests (child process) ---\n");

    // Created while nothing is hooked, so it is unambiguously real. S-11 and S-12
    // then try to delete it from inside isolation, and S-11b checks it survived.
    {
        HKEY hVictim = nullptr;

        if (RegCreateKeyExW(HKEY_CURRENT_USER, kVictimKey, 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &hVictim, nullptr)
                    == ERROR_SUCCESS && hVictim)
        {
            const wchar_t* keep = L"intact";

            RegSetValueExW(hVictim, L"Keep", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(keep),
                static_cast<DWORD>((wcslen(keep) + 1) * sizeof(wchar_t)));

            RegCloseKey(hVictim);
        }
    }

    HMODULE hDll = LoadLibraryW(dllPath.c_str());

    if (!hDll)
    {
        wprintf(L"  FAIL  S-00: child could not load %ls (GLE=%lu)\n",
            dllPath.c_str(), GetLastError());
        return 1;
    }

    // S-01: the store has never heard of this key, but isolation makes it
    // virtual anyway, so the open is served rather than passed through.
    {
        HKEY hk = nullptr;
        LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRealKey, 0, KEY_READ, &hk);
        ASSERT(st == ERROR_SUCCESS,
            L"S-01: a key absent from the file still opens under Isolated");

        // S-02: and nothing leaks out of the real hive behind it.
        if (hk)
        {
            wchar_t buf[512]{};
            DWORD cb = sizeof(buf);
            LSTATUS r = RegQueryValueExW(hk, kRealValue, nullptr, nullptr,
                reinterpret_cast<LPBYTE>(buf), &cb);
            ASSERT(r == ERROR_FILE_NOT_FOUND,
                L"S-02: a real registry value is not served under Isolated");
            RegCloseKey(hk);
        }
        else
            ASSERT(false, L"S-02: a real registry value is not served under Isolated");
    }

    // S-03: the seeded key still reads normally -- isolation widens the virtual
    // space, it does not empty it.
    {
        HKEY hk = nullptr;
        RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\IsolationSeed", 0, KEY_READ, &hk);

        wchar_t buf[64]{};
        DWORD cb = sizeof(buf);
        LSTATUS r = hk ? RegQueryValueExW(hk, L"Seeded", nullptr, nullptr,
            reinterpret_cast<LPBYTE>(buf), &cb) : ERROR_INVALID_HANDLE;

        ASSERT(r == ERROR_SUCCESS && wcscmp(buf, L"yes") == 0,
            L"S-03: a key the file does define still reads back under Isolated");

        if (hk) RegCloseKey(hk);
    }

    // S-04: a key nothing has ever defined is created in the store, and its
    // value reads back.
    {
        HKEY hk = nullptr;
        DWORD disp = 0;
        LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, kNovelKey, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &hk, &disp);

        ASSERT(st == ERROR_SUCCESS && hk != nullptr,
            L"S-04a: creating a brand-new key succeeds under Isolated");

        if (hk)
        {
            const wchar_t* written = L"IsolatedWrite";
            DWORD cb = static_cast<DWORD>((wcslen(written) + 1) * sizeof(wchar_t));

            LSTATUS r = RegSetValueExW(hk, L"Probe", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(written), cb);
            ASSERT(r == ERROR_SUCCESS,
                L"S-04b: writing into that key succeeds");

            wchar_t buf[64]{};
            DWORD rcb = sizeof(buf);
            r = RegQueryValueExW(hk, L"Probe", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(buf), &rcb);
            ASSERT(r == ERROR_SUCCESS && wcscmp(buf, written) == 0,
                L"S-04c: the written value reads back");

            RegCloseKey(hk);
        }
    }

    // S-07: the headline regression. RegGetValue reaches the real registry through
    // an internal open, so before it was hooked it handed back the real value even
    // under isolation -- the one thing isolation exists to prevent.
    {
        wchar_t buf[512]{};
        DWORD cb = sizeof(buf);

        LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, kRealKey, kRealValue,
            RRF_RT_ANY, nullptr, buf, &cb);

        ASSERT(st == ERROR_FILE_NOT_FOUND,
            L"S-07: RegGetValueW does not serve a real value under Isolated");
    }

    // S-08: and the composite write lands in the store, like RegSetValueEx does.
    {
        const wchar_t* written = L"IsolatedKeyValue";

        LSTATUS st = RegSetKeyValueW(HKEY_CURRENT_USER, kNovelKey2, L"Probe", REG_SZ,
            written, static_cast<DWORD>((wcslen(written) + 1) * sizeof(wchar_t)));

        ASSERT(st == ERROR_SUCCESS,
            L"S-08: RegSetKeyValueW succeeds under Isolated");
    }

    // S-11/S-12: the delete hooks cannot escape isolation. The victim key is real
    // and was created before the DLL loaded; isolation makes every path virtual, so
    // a key the store has never heard of is ERROR_FILE_NOT_FOUND rather than a
    // passthrough -- which is exactly what keeps a delete from reaching HKCU.
    {
        LSTATUS st = RegDeleteKeyW(HKEY_CURRENT_USER, kVictimKey);

        ASSERT(st == ERROR_FILE_NOT_FOUND,
            L"S-11a: RegDeleteKeyW on a real key is ERROR_FILE_NOT_FOUND under Isolated");

        st = RegDeleteTreeW(HKEY_CURRENT_USER, kVictimKey);

        ASSERT(st == ERROR_FILE_NOT_FOUND,
            L"S-12: and so is RegDeleteTreeW");
    }

    FreeLibrary(hDll);

    // Hooks are gone now, so the last two assertions see the real world.

    // S-05: the write went to the file.
    {
        std::wstring persisted = ReadFileAsWide(regPath);
        ASSERT(persisted.find(L"IsolatedWrite") != std::wstring::npos,
            L"S-05: the isolated write is persisted to the configured file");
    }

    // S-06: and not to HKCU.
    {
        HKEY hk = nullptr;
        LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, kNovelKey, 0, KEY_READ, &hk);
        ASSERT(st != ERROR_SUCCESS,
            L"S-06: the isolated write never reached the real registry");

        if (hk)
        {
            RegCloseKey(hk);
            RegDeleteKeyW(HKEY_CURRENT_USER, kNovelKey);
        }
    }

    // S-09: S-08's composite write went to the file.
    {
        std::wstring persisted = ReadFileAsWide(regPath);

        ASSERT(persisted.find(L"IsolatedKeyValue") != std::wstring::npos,
            L"S-09: the RegSetKeyValueW write is persisted to the configured file");
    }

    // S-10: and not to HKCU either.
    {
        HKEY hk = nullptr;
        LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, kNovelKey2, 0, KEY_READ, &hk);

        ASSERT(st != ERROR_SUCCESS,
            L"S-10: the composite write never reached the real registry");

        if (hk)
        {
            RegCloseKey(hk);
            RegDeleteKeyW(HKEY_CURRENT_USER, kNovelKey2);
        }
    }

    // S-11b: the real key the delete hooks were pointed at is still there, value
    // intact. This is the assertion that actually proves nothing escaped.
    {
        HKEY hk = nullptr;
        std::wstring survived;

        if (RegOpenKeyExW(HKEY_CURRENT_USER, kVictimKey, 0, KEY_READ, &hk) == ERROR_SUCCESS
                && hk)
        {
            wchar_t buf[64]{};
            DWORD cb = sizeof(buf);

            if (RegQueryValueExW(hk, L"Keep", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS)
                survived = buf;

            RegCloseKey(hk);
        }

        ASSERT(survived == L"intact",
            L"S-11b: the real key and its value survived both delete attempts");

        RegDeleteKeyW(HKEY_CURRENT_USER, kVictimKey);
    }

    return g_fail;
}

// Runs in the parent. Stages the child's directory, runs it, and folds the
// outcome into this process's tally so there is still one summary at the end.
static void RunIsolatedTests(const std::wstring& exeDir, const std::wstring& testTmpDir)
{
    std::wstring childDir        = testTmpDir + L"Isolated\\";
    std::wstring childInterposer = childDir + L".interposer\\";

    CreateDirs(childInterposer);

    if (!CopyFileW((exeDir + L"LANCommander.Interposer.dll").c_str(),
                   (childDir + L"LANCommander.Interposer.dll").c_str(), FALSE))
    {
        ASSERT(false, L"S-00: staging the isolated child's copy of the DLL");
        return;
    }

    WriteIsolatedFixtures(childInterposer);

    std::wstring exePath = exeDir + L"LANCommander.Interposer.Tests.exe";
    // childDir's trailing separator has to come off: inside a quoted argument it
    // would escape the closing quote. RunIsolatedChild puts it back.
    std::wstring childArg = childDir.substr(0, childDir.size() - 1);
    std::wstring command  = L"\"" + exePath + L"\" --isolated \"" + childArg + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // Handles are inherited so the child's PASS/FAIL lines land on this console.
    BOOL launched = CreateProcessW(exePath.c_str(), command.data(), nullptr, nullptr,
        TRUE, 0, nullptr, nullptr, &si, &pi);

    if (!launched)
    {
        ASSERT(false, L"S-00: launching the isolated-mode child process");
        return;
    }

    WaitForSingleObject(pi.hProcess, 120 * 1000);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    ASSERT(exitCode == 0, L"S-00: the isolated-mode child process reported no failures");

    DeleteFileW((childInterposer + L"Config.yml").c_str());
    DeleteFileW((childInterposer + L"Registry.reg").c_str());
    DeleteFileW((childDir + L"LANCommander.Interposer.dll").c_str());
    ClearLogFiles(childInterposer + L"Logs\\");
    RemoveDirectoryW((childInterposer + L"Logs").c_str());
    RemoveDirectoryW(childInterposer.c_str());
    RemoveDirectoryW(childDir.c_str());
}

// ============================================================
// Entry point
// ============================================================

int wmain(int argc, wchar_t** argv)
{
    // Unbuffered: a crash mid-run otherwise discards everything still sitting in
    // the buffer, which is exactly the output needed to locate it.
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Re-entry from RunIsolatedTests. Registry.Isolated cannot share a process
    // with the rest of the suite, so it gets one of its own.
    if (argc >= 3 && wcscmp(argv[1], L"--isolated") == 0)
        return RunIsolatedChild(argv[2]);

    std::wstring exeDir        = GetExeDir();
    std::wstring testTmpDir    = GetTestTempDir();
    std::wstring interposerDir = exeDir + L".interposer\\";
    std::wstring logsDir       = interposerDir + L"Logs\\";
    std::wstring yamlPath      = interposerDir + L"Config.yml";
    std::wstring regPath       = interposerDir + L"Registry.reg";
    std::wstring baseRegPath   = interposerDir + L"RegistryBase.reg";
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

    // The fixture's %INTERPOSER_TEST_ROOT% pattern resolves against the process
    // environment, which LoadConfig reads during DLL_PROCESS_ATTACH -- so this
    // has to be set before LoadLibraryW, not just before the test that uses it.
    SetEnvironmentVariableW(kTokenEnvVar, testTmpDir.c_str());

    // .interposer\Config.yml must be in place before LoadLibraryW
    // (LoadConfig runs in DLL_PROCESS_ATTACH, before MH_EnableHook)
    CreateDirs(interposerDir);
    WriteInterposerYaml(yamlPath, redirectDir);

    // .interposer\Registry.reg must be in place before LoadLibraryW
    // (InstallRegistryHooks → LoadRegFile runs before MH_EnableHook)
    WriteVirtualReg(regPath);

    // The read-only layer Registry.Files stacks underneath it.
    WriteRegistryBaseReg(baseRegPath);

    // dinput.dll has to be in the process before the Interposer loads, or
    // InstallDirectInputHooks finds no module to hook. A real game gets this
    // for free by importing it statically.
    HMODULE hDInput = LoadLibraryW(L"dinput.dll");

    // Likewise for dinput8.dll, whose DirectInput8Create carries the device
    // filter for games that never touch the legacy API.
    HMODULE hDInput8 = LoadLibraryW(L"dinput8.dll");
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
    RunOverlayTests();
    RunIdentityTests();
    RunOsVersionTests();
    RunDirectInputTests(hDInput);
    RunDirectInput8Tests(hDInput8);
    RunMouseTransformTests(hDll, hDInput);

    // ── Unload DLL ───────────────────────────────────────────────────────────
    // DLL_PROCESS_DETACH fires: RemoveRegistryHooks (SaveRegFile if dirty),
    // then MH_DisableHook + MH_Uninitialize, then CloseLog.
    FreeLibrary(hDll);
    hDll = nullptr;

    if (hDInput)
    {
        FreeLibrary(hDInput);
        hDInput = nullptr;
    }

    if (hDInput8)
    {
        FreeLibrary(hDInput8);
        hDInput8 = nullptr;
    }

    // ── Post-unload tests ─────────────────────────────────────────────────────
    // Discover the timestamped log created by LoadConfig during DLL_PROCESS_ATTACH.
    std::wstring logPath = FindFirstLogFile(logsDir);
    RunLogTests(logPath);
    RunPersistenceTests(regPath);
    RunOverlayPersistenceTests(baseRegPath, regPath);

    // Last, because it spawns a second process that loads its own copy of the
    // DLL -- nothing after it should depend on this process's hooks.
    RunIsolatedTests(exeDir, testTmpDir);

    // ── Cleanup ──────────────────────────────────────────────────────────────
    DeleteFileW(redirectTarget.c_str());
    RemoveDirectoryW(redirectDir.c_str());

    // R-23c and the RegCopyTree tests create a real HKCU key to exercise the
    // passthrough branches against something this suite owns. Removed here rather
    // than in the test, which still needs it while the hooks are live.
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LANCommanderInterposerTest");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\LANCommanderInterposerTest");

    // F-09 wrote through the %SAVEDGAMES% rule into the real Saved Games folder.
    {
        std::wstring savedGames = GetSavedGamesDir();

        if (!savedGames.empty())
        {
            std::wstring probeDir = savedGames + L"\\LANCommanderTest";
            DeleteFileW((probeDir + L"\\probe.dat").c_str());
            RemoveDirectoryW(probeDir.c_str());
        }
    }

    SetEnvironmentVariableW(kTokenEnvVar, nullptr);
    if (!logPath.empty()) DeleteFileW(logPath.c_str());
    RemoveDirectoryW(logsDir.c_str());
    RemoveDirectoryW(testTmpDir.c_str());
    // Leave .interposer/Config.yml, Registry.reg and RegistryBase.reg for
    // post-run inspection

    // ── Summary ──────────────────────────────────────────────────────────────
    wprintf(L"\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
