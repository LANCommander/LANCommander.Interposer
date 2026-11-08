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
// The relay tests call gethostbyname deliberately: it is what the games do.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <windows.h>
#include <winsock2.h>        // network relay tests (N-)
#include <ws2tcpip.h>
#include <wsipx.h>
#include <wsnwlink.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <shlobj.h>          // SHGetKnownFolderPath / FOLDERID_SavedGames (F-09)
#include <string>
#include <vector>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <delayimp.h>       // __FUnloadDelayLoadedDLL2 (SDK-)
#include "LANCommander.Interposer.Comms.Server.h"   // voice (VC-, MI-07/08)
#include "LANCommander.Interposer.h"                 // match statistics structs (MS-, MI-09)

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

// ============================================================
// Test infrastructure
// ============================================================

static int g_pass = 0, g_fail = 0;

// Real audio device counts, read before the DLL is loaded -- afterwards there
// is no way to see the unfiltered list from inside this process. Declared up
// here because the log tests consult them too: a machine with no capture
// device gives the filter nothing to hide, so there is no line to assert on.
static UINT g_realWaveOut = 0;
static UINT g_realWaveIn  = 0;
static int  g_realDsOut   = -1;   // -1 = enumeration unavailable, test skipped
static int  g_realDsIn    = -1;

// Endpoint ID of the first render device, discovered before the DLL loads and
// written into the fixture as the sole AudioDevices.Output entry.
static std::wstring g_pinnedEndpointId;

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
        "  Audio: true\n"
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
        // AudioDevices is appended after this literal rather than written into
        // it: the Output list has to name a real endpoint on this machine, and
        // that is only known once Core Audio has been asked.
        //
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
        // FastDL with no BaseUrl and no probing never reaches the network, so the
        // overlay half is testable offline. Local is built from %GAMEDIR%, the way
        // a config shipped with a game patch has to name the game's folders. FD-
        // tests open files under it by absolute and by relative path.
        "FastDL:\n"
        "  Enabled: true\n"
        "  ProbeConnections: false\n"
        "  UseDownloadDirectory: true\n"
        "  Paths:\n"
        "    - Local: '%GAMEDIR%\\FastDLProbe'\n"
        "      Remote: maps\n"
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
    // AudioDevices, assembled from what this machine actually has.
    //
    // Output names the first render endpoint by its Core Audio endpoint ID --
    // an identifier winmm and DirectSound cannot report, so the only way they
    // can agree about it is by resolving through the identity table. Input is
    // present but empty, which is the allow-list that admits nothing.
    content += "AudioDevices:\n";
    content += "  Enabled: true\n";

    if (!g_pinnedEndpointId.empty())
    {
        content += "  Output:\n";
        content += "    - '" + WideToUtf8(g_pinnedEndpointId) + "'\n";
    }

    content += "  Input: []\n";

    // Plugin sections for the C- tests. Tester is block style, with a comment
    // the in-place patch has to keep; Flow is a flow map the patcher declines,
    // so writing into it exercises the re-render fallback -- which must stay
    // inside Flow and leave Tester's comment alone.
    content +=
        "\n"
        "Plugins:\n"
        "  Tester:\n"
        "    Existing: 'old'   # kept across the write\n"
        "    Nulled:\n"
        "    Nested:\n"
        "      Deep: 1\n"
        "  Flow: {A: '1'}\n"
        "# trailing comment\n";

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

    // --- Audio devices (AudioDevices filter + Logging.Audio) ---

    ASSERT(log.find("winmm.dll!waveOutGetNumDevs") != std::string::npos,
        L"L-AU-1: Log records the winmm.dll!waveOutGetNumDevs hook installation");
    ASSERT(log.find("dsound.dll!DirectSoundEnumerateA") != std::string::npos,
        L"L-AU-2: Log records the dsound.dll!DirectSoundEnumerateA hook installation");
    ASSERT(log.find("ole32.dll!CoCreateInstance") != std::string::npos,
        L"L-AU-3: Log records the ole32.dll!CoCreateInstance hook installation");
    ASSERT(log.find("[AUDIO ENUM]") != std::string::npos,
        L"L-AU-4: Log contains [AUDIO ENUM] entry");
    ASSERT(log.find("output devices shown") != std::string::npos,
        L"L-AU-5: [AUDIO ENUM] reports how many devices survived the filter");

    // Logging.Level is Trace in the fixture, so the per-device listing is on.
    // A machine with no capture device has nothing to hide, so the hidden-device
    // verb is only required when one was actually present before the DLL loaded.
    if (g_realWaveIn > 0)
    {
        ASSERT(log.find("[AUDIO HIDE]") != std::string::npos,
            L"L-AU-6: Log lists devices the filter hid at Debug level");
        ASSERT(log.find("no devices are allowed in this direction") != std::string::npos,
            L"L-AU-7: [AUDIO HIDE] reports that the empty Input list allows nothing");
    }

    // The identity table is what lets winmm honour an entry written as a Core
    // Audio endpoint ID, so its construction is worth seeing in the log.
    if (!g_pinnedEndpointId.empty())
    {
        ASSERT(log.find("identity table built") != std::string::npos,
            L"L-AU-8: Log records the audio identity table being built");
        ASSERT(log.find("matched ") != std::string::npos,
            L"L-AU-9: [AUDIO FOUND] names the entry that kept the device");
    }

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

// ============================================================
// Plugin config tests (InterposerGetConfigString / InterposerSetConfigString)
// ============================================================

using FnTestGetConfig = BOOL (*)(const wchar_t*, wchar_t*, DWORD);
using FnTestSetConfig = BOOL (*)(const wchar_t*, const wchar_t*);

static std::wstring GetConfig(FnTestGetConfig get, const wchar_t* path)
{
    wchar_t buf[256]{};
    return get(path, buf, ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring(L"<missing>");
}

static void RunPluginConfigTests(HMODULE hDll)
{
    wprintf(L"\n--- Plugin Config Tests ---\n");

    auto get = reinterpret_cast<FnTestGetConfig>(
        reinterpret_cast<void*>(GetProcAddress(hDll, "InterposerGetConfigString")));
    auto set = reinterpret_cast<FnTestSetConfig>(
        reinterpret_cast<void*>(GetProcAddress(hDll, "InterposerSetConfigString")));

    ASSERT(get != nullptr && set != nullptr,
        L"C-01: InterposerGetConfigString and InterposerSetConfigString are exported");
    if (!get || !set) return;

    // Each read used to assign every child over its parent inside g_configRoot,
    // so the first lookup left the root holding the value it ended on and every
    // read after it -- through any path -- failed.
    ASSERT(GetConfig(get, L"Plugins.Tester.Existing") == L"old" &&
           GetConfig(get, L"Plugins.Tester.Existing") == L"old" &&
           GetConfig(get, L"Player.Username") == L"TestPlayer",
        L"C-02: repeated reads leave the config tree intact");

    ASSERT(set(L"Plugins.Tester.Existing", L"new") &&
           GetConfig(get, L"Plugins.Tester.Existing") == L"new",
        L"C-03: overwriting an existing value is visible to the next read");

    ASSERT(set(L"Plugins.Tester.Added.Leaf", L"leaf") &&
           GetConfig(get, L"Plugins.Tester.Added.Leaf") == L"leaf",
        L"C-04: a missing key and its intermediate map are created");

    ASSERT(set(L"Plugins.Tester.Nulled", L"filled") &&
           GetConfig(get, L"Plugins.Tester.Nulled") == L"filled",
        L"C-05: a key with a null value takes a scalar");

    ASSERT(set(L"Plugins.Tester.Empty", L"") &&
           GetConfig(get, L"Plugins.Tester.Empty").empty(),
        L"C-06: an empty string round-trips as empty, not null");

    ASSERT(!set(L"Player.Username", L"Intruder") &&
           GetConfig(get, L"Player.Username") == L"TestPlayer",
        L"C-07: a write outside Plugins is refused and changes nothing");

    ASSERT(!set(L"Plugins.Tester", L"x"),
        L"C-08: a write to a whole plugin section is refused");

    ASSERT(!set(L"Plugins.Tester.Nested", L"x") &&
           GetConfig(get, L"Plugins.Tester.Nested.Deep") == L"1",
        L"C-09: a scalar is not written over an existing map");

    ASSERT(!set(L"Plugins.Tester.Existing.Sub", L"x") &&
           GetConfig(get, L"Plugins.Tester.Existing") == L"new",
        L"C-10: a map is not written over an existing scalar");

    ASSERT(set(L"Plugins.Flow.B", L"2") &&
           GetConfig(get, L"Plugins.Flow.A") == L"1" &&
           GetConfig(get, L"Plugins.Flow.B") == L"2",
        L"C-11: a write into a flow-style section succeeds and keeps its siblings");

    ASSERT(set(L"Plugins.NewPlugin.Key", L"true") &&
           GetConfig(get, L"Plugins.NewPlugin.Key") == L"true",
        L"C-12: a write creates a plugin section that did not exist");
}

// Config.yml as InterposerSetConfigString left it.
static void RunPluginConfigPersistenceTests(const std::wstring& yamlPath)
{
    wprintf(L"\n--- Plugin Config Persistence Tests ---\n");

    std::string yml = ReadFileAsUtf8(yamlPath);
    auto has = [&](const char* s) { return yml.find(s) != std::string::npos; };

    ASSERT(has("    Existing: new   # kept across the write\n"),
        L"CP-01: an overwritten value is patched in place, keeping its comment");

    ASSERT(has("    Nulled: filled\n"),
        L"CP-02: a null value is filled in on its own line");

    ASSERT(has("    Added:\n      Leaf: leaf\n"),
        L"CP-03: a created key is written as a nested block");

    ASSERT(has("    Empty: ''\n"),
        L"CP-04: an empty string is written quoted");

    // The fallback re-rendered Flow, and only Flow
    ASSERT(has("  Flow:\n    A: 1\n    B: 2\n") && !has("{A:"),
        L"CP-05: the flow-style section is re-rendered as a block");

    ASSERT(has("  NewPlugin:\n    Key: true\n"),
        L"CP-06: a new plugin section is appended under Plugins");

    ASSERT(has("# trailing comment\n") && has("  Username: TestPlayer\n"),
        L"CP-07: content outside the written keys is untouched");
}

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
// Audio device filter tests
// ============================================================
// The fixture pins one specific device: it discovers the first render endpoint
// before the DLL loads and writes that endpoint's ID as the only entry in
// AudioDevices.Output, alongside an empty AudioDevices.Input.
//
// That shape is what makes the assertions machine-independent while still
// exercising the parts that matter. An empty Input list must hide every
// microphone. A one-entry Output list must leave exactly that device visible --
// and crucially it must do so through winmm too, which knows nothing about
// endpoint IDs and reports names truncated to 31 characters. winmm can only
// agree with Core Audio here if the device really was resolved through the
// identity table, so A-03 is the test for that whole chain.
//
// The real counts are captured before the Interposer loads, which is the only
// way to see the unfiltered lists from inside the same process.

struct TestAudioDevice
{
    wchar_t name[256];
    wchar_t description[256];
    wchar_t id[256];
    wchar_t instanceId[256];
    wchar_t hardwareId[256];
    int     direction;   // 1 = output, 2 = input
    int     isDefault;
    int     allowed;
};

using TestPfnEnumAudio    = DWORD (*)(TestAudioDevice*, DWORD, DWORD*);
using TestPfnWaveNumDevs  = UINT (WINAPI*)(void);
using TestPfnDsCallbackA  = BOOL (CALLBACK*)(GUID*, const char*, const char*, void*);
using TestPfnDsEnumerateA = HRESULT (WINAPI*)(TestPfnDsCallbackA, void*);

// Core Audio, declared locally for the same reason the DirectInput ABI is: only
// a few vtable slots are needed, and the SDK GUIDs would add a link dependency
// the test does not otherwise have.
static const CLSID kTestClsidMMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID kTestIidMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };

using TestPfnCoInitializeEx   = HRESULT (__stdcall*)(void*, DWORD);
using TestPfnCoUninitialize   = void (__stdcall*)(void);
using TestPfnCoCreateInstance = HRESULT (__stdcall*)(const CLSID*, void*, DWORD, const IID*, void**);
using TestPfnCoTaskMemFree    = void (__stdcall*)(void*);
using TestPfnEnumEndpoints    = HRESULT (__stdcall*)(void*, int, DWORD, void**);
using TestPfnCollectionCount  = HRESULT (__stdcall*)(void*, UINT*);
using TestPfnCollectionItem   = HRESULT (__stdcall*)(void*, UINT, void**);
using TestPfnDeviceGetId      = HRESULT (__stdcall*)(void*, wchar_t**);

static BOOL CALLBACK TestDsCountCallback(GUID*, const char*, const char*, void* ref)
{
    ++*static_cast<int*>(ref);
    return TRUE;
}

// Count what a DirectSound enumeration yields. Returns -1 when the export is
// unavailable, so a machine without dsound skips rather than fails.
static int CountDsDevices(HMODULE hDSound, const char* exportName)
{
    if (!hDSound)
        return -1;

    auto enumerate = reinterpret_cast<TestPfnDsEnumerateA>(
        reinterpret_cast<void*>(GetProcAddress(hDSound, exportName)));

    if (!enumerate)
        return -1;

    int count = 0;

    if (FAILED(enumerate(TestDsCountCallback, &count)))
        return -1;

    return count;
}

// Read the unfiltered device counts. Called before LoadLibraryW of the DLL.
static void CaptureRealAudioCounts(HMODULE hWinmm, HMODULE hDSound)
{
    if (hWinmm)
    {
        auto waveOutGetNumDevs = reinterpret_cast<TestPfnWaveNumDevs>(
            reinterpret_cast<void*>(GetProcAddress(hWinmm, "waveOutGetNumDevs")));
        auto waveInGetNumDevs = reinterpret_cast<TestPfnWaveNumDevs>(
            reinterpret_cast<void*>(GetProcAddress(hWinmm, "waveInGetNumDevs")));

        if (waveOutGetNumDevs) g_realWaveOut = waveOutGetNumDevs();
        if (waveInGetNumDevs)  g_realWaveIn  = waveInGetNumDevs();
    }

    g_realDsOut = CountDsDevices(hDSound, "DirectSoundEnumerateA");
    g_realDsIn  = CountDsDevices(hDSound, "DirectSoundCaptureEnumerateA");
}

// Endpoint ID of the first active render device, read before the DLL loads so
// the fixture can pin it by name. Empty on a machine with no output device, in
// which case the Output list is left out of the fixture entirely.
static void DiscoverPinnedEndpoint(HMODULE hOle32)
{
    if (!hOle32)
        return;

    auto coInitialize = reinterpret_cast<TestPfnCoInitializeEx>(
        reinterpret_cast<void*>(GetProcAddress(hOle32, "CoInitializeEx")));
    auto coUninitialize = reinterpret_cast<TestPfnCoUninitialize>(
        reinterpret_cast<void*>(GetProcAddress(hOle32, "CoUninitialize")));
    auto coCreateInstance = reinterpret_cast<TestPfnCoCreateInstance>(
        reinterpret_cast<void*>(GetProcAddress(hOle32, "CoCreateInstance")));
    auto coTaskMemFree = reinterpret_cast<TestPfnCoTaskMemFree>(
        reinterpret_cast<void*>(GetProcAddress(hOle32, "CoTaskMemFree")));

    if (!coInitialize || !coCreateInstance || !coTaskMemFree)
        return;

    const bool initialized = SUCCEEDED(coInitialize(nullptr, 0));   // COINIT_MULTITHREADED

    void* enumerator = nullptr;

    // CLSCTX_INPROC_SERVER
    if (SUCCEEDED(coCreateInstance(&kTestClsidMMDeviceEnumerator, nullptr, 1,
            &kTestIidMMDeviceEnumerator, &enumerator)) && enumerator)
    {
        void* collection = nullptr;

        // eRender, DEVICE_STATE_ACTIVE
        if (SUCCEEDED(reinterpret_cast<TestPfnEnumEndpoints>(TestVtSlot(enumerator, 3))(
                enumerator, 0, 0x1, &collection)) && collection)
        {
            UINT count = 0;

            reinterpret_cast<TestPfnCollectionCount>(TestVtSlot(collection, 3))(collection, &count);

            void* device = nullptr;

            if (count > 0 &&
                SUCCEEDED(reinterpret_cast<TestPfnCollectionItem>(TestVtSlot(collection, 4))(
                    collection, 0, &device)) && device)
            {
                wchar_t* id = nullptr;

                if (SUCCEEDED(reinterpret_cast<TestPfnDeviceGetId>(TestVtSlot(device, 5))(device, &id)) && id)
                {
                    g_pinnedEndpointId = id;
                    coTaskMemFree(id);
                }

                reinterpret_cast<TestPfnRelease>(TestVtSlot(device, 2))(device);
            }

            reinterpret_cast<TestPfnRelease>(TestVtSlot(collection, 2))(collection);
        }

        reinterpret_cast<TestPfnRelease>(TestVtSlot(enumerator, 2))(enumerator);
    }

    if (initialized && coUninitialize)
        coUninitialize();
}

static void RunAudioTests(HMODULE hDll, HMODULE hWinmm, HMODULE hDSound, HMODULE hOle32)
{
    wprintf(L"\n--- Audio Device Filter Tests ---\n");

    if (g_pinnedEndpointId.empty())
        wprintf(L"        (no render endpoint to pin; Output was left unfiltered)\n");
    else
        wprintf(L"        (pinned %s)\n", g_pinnedEndpointId.c_str());

    // A-01: an Input key with an empty list is an allow-list that admits
    // nothing, which is how every microphone is hidden.
    auto waveOutGetNumDevs = reinterpret_cast<TestPfnWaveNumDevs>(
        hWinmm ? GetProcAddress(hWinmm, "waveOutGetNumDevs") : nullptr);
    auto waveInGetNumDevs = reinterpret_cast<TestPfnWaveNumDevs>(
        hWinmm ? GetProcAddress(hWinmm, "waveInGetNumDevs") : nullptr);

    if (waveInGetNumDevs)
    {
        ASSERT(waveInGetNumDevs() == 0,
            L"A-01: an empty Input list hides every input device");

        wprintf(L"        (%u real input devices)\n", g_realWaveIn);
    }
    else
    {
        wprintf(L"  SKIP  winmm.dll unavailable; A-01 skipped\n");
    }

    // A-02: the export reports the real device set with the filter's verdict on
    // each device. Exactly the pinned endpoint should survive.
    UINT allowedOutputs = 0;
    UINT allowedInputs  = 0;
    bool pinnedAllowed  = false;
    bool listIsComplete = false;

    {
        auto enumAudio = reinterpret_cast<TestPfnEnumAudio>(
            reinterpret_cast<void*>(GetProcAddress(hDll, "InterposerEnumAudioDevices")));

        ASSERT(enumAudio != nullptr,
            L"A-02a: InterposerEnumAudioDevices is exported");

        if (enumAudio)
        {
            DWORD written = 0;
            DWORD total   = enumAudio(nullptr, 0, &written);

            std::vector<TestAudioDevice> devices(total);

            if (total > 0)
                enumAudio(devices.data(), total, &written);

            for (DWORD i = 0; i < written; ++i)
            {
                const TestAudioDevice& d = devices[i];

                if (d.allowed && d.direction == 1) ++allowedOutputs;
                if (d.allowed && d.direction == 2) ++allowedInputs;

                if (!g_pinnedEndpointId.empty() && g_pinnedEndpointId == d.id && d.allowed)
                    pinnedAllowed = true;
            }

            ASSERT(allowedInputs == 0,
                L"A-02b: no input endpoint passes an empty Input list");

            if (!g_pinnedEndpointId.empty())
            {
                ASSERT(pinnedAllowed && allowedOutputs == 1,
                    L"A-02c: exactly the endpoint named in Output passes the filter");
            }

            wprintf(L"        (%lu endpoints reported, %u outputs allowed)\n", total, allowedOutputs);

            listIsComplete = (total == written);
        }
    }

    // A-03: THE test for the identity table. The fixture names the device by its
    // Core Audio endpoint ID -- something winmm cannot report and does not know
    // exists. winmm can only arrive at the same answer by resolving its device
    // index to an endpoint and looking it up, so agreement here means the whole
    // chain worked. Falling back to the truncated name would yield 0.
    if (waveOutGetNumDevs)
    {
        ASSERT(waveOutGetNumDevs() == allowedOutputs,
            L"A-03: winmm agrees with Core Audio on which output devices survive");

        wprintf(L"        (%u of %u real output devices shown)\n",
            waveOutGetNumDevs(), g_realWaveOut);
    }

    // A-04/A-05: DirectSound. Its leading "Primary Sound Driver" entry has no
    // name or GUID an entry would match, so it is hidden too -- which is why the
    // count must come out below the unfiltered one.
    if (g_realDsIn >= 0)
    {
        ASSERT(CountDsDevices(hDSound, "DirectSoundCaptureEnumerateA") == 0,
            L"A-04: DirectSoundCaptureEnumerate yields no devices");
    }
    else
    {
        wprintf(L"  SKIP  DirectSoundCaptureEnumerateA unavailable; A-04 skipped\n");
    }

    if (g_realDsOut > 0)
    {
        const int shown = CountDsDevices(hDSound, "DirectSoundEnumerateA");

        ASSERT(shown >= 0 && shown < g_realDsOut,
            L"A-05a: DirectSoundEnumerate hides the devices the filter excludes");

        if (!g_pinnedEndpointId.empty())
        {
            ASSERT(shown == static_cast<int>(allowedOutputs),
                L"A-05b: DirectSound shows the same devices Core Audio does");
        }

        wprintf(L"        (%d of %d DirectSound output devices shown)\n", shown, g_realDsOut);
    }
    else
    {
        wprintf(L"  SKIP  DirectSoundEnumerateA unavailable; A-05 skipped\n");
    }

    // A-06: Core Audio end to end. EnumAudioEndpoints must hand back a
    // collection holding exactly the endpoints the export marked allowed.
    if (!hOle32 || !listIsComplete)
    {
        wprintf(L"  SKIP  Core Audio unavailable or device list incomplete; A-06 skipped\n");
        return;
    }

    auto coInitialize = reinterpret_cast<TestPfnCoInitializeEx>(
        reinterpret_cast<void*>(GetProcAddress(hOle32, "CoInitializeEx")));
    auto coUninitialize = reinterpret_cast<TestPfnCoUninitialize>(
        reinterpret_cast<void*>(GetProcAddress(hOle32, "CoUninitialize")));
    auto coCreateInstance = reinterpret_cast<TestPfnCoCreateInstance>(
        reinterpret_cast<void*>(GetProcAddress(hOle32, "CoCreateInstance")));

    if (!coInitialize || !coCreateInstance)
    {
        wprintf(L"  SKIP  ole32 entry points unavailable; A-06 skipped\n");
        return;
    }

    const bool initialized = SUCCEEDED(coInitialize(nullptr, 0));

    void* enumerator = nullptr;

    // CLSCTX_INPROC_SERVER
    HRESULT hr = coCreateInstance(&kTestClsidMMDeviceEnumerator, nullptr, 1,
        &kTestIidMMDeviceEnumerator, &enumerator);

    ASSERT(SUCCEEDED(hr) && enumerator != nullptr,
        L"A-06a: CoCreateInstance(CLSID_MMDeviceEnumerator) succeeds");

    if (SUCCEEDED(hr) && enumerator)
    {
        void* collection = nullptr;

        // eAll, DEVICE_STATE_ACTIVE
        hr = reinterpret_cast<TestPfnEnumEndpoints>(TestVtSlot(enumerator, 3))(
            enumerator, 2, 0x1, &collection);

        ASSERT(SUCCEEDED(hr) && collection != nullptr,
            L"A-06b: EnumAudioEndpoints succeeds");

        if (SUCCEEDED(hr) && collection)
        {
            UINT count = 0;

            reinterpret_cast<TestPfnCollectionCount>(TestVtSlot(collection, 3))(collection, &count);

            ASSERT(count == allowedOutputs + allowedInputs,
                L"A-06c: EnumAudioEndpoints yields only the allowed endpoints");

            // Released before the DLL unloads: the filtered collection's vtable
            // lives in the Interposer's image.
            reinterpret_cast<TestPfnRelease>(TestVtSlot(collection, 2))(collection);
        }

        reinterpret_cast<TestPfnRelease>(TestVtSlot(enumerator, 2))(enumerator);
    }

    if (initialized && coUninitialize)
        coUninitialize();
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
// Launcher config tests  (separate processes)
// ============================================================
// A launcher that injects one copy of the DLL into many games hands each its own
// base directory (Local\InterposerBaseDirectory_<pid>) and a config overlay
// (Local\InterposerConfig_<pid>). Both are read once, in DLL_PROCESS_ATTACH, and
// change where every .interposer file lives, so they cannot share this process:
// each mode re-runs the EXE with --launcher-config against a DLL copy of its
// own. The child creates the mappings itself, under its own PID, before it loads
// the DLL -- exactly what InterposerService does for the game it starts.
//
//   merge         Config.yml in the base directory, overlay merged over it.
//   overlay-only  No Config.yml anywhere the DLL should look; the base directory
//                 does not exist yet. The bundled-launcher case.
//
// Both modes put a decoy Config.yml beside the DLL, which must be ignored.

static HANDLE CreateLauncherMapping(const wchar_t* prefix, const std::string& utf8)
{
    wchar_t name[96]{};
    swprintf_s(name, L"Local\\%ls_%lu", prefix, GetCurrentProcessId());

    const DWORD size = static_cast<DWORD>(utf8.size() + 1);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name);

    if (!mapping)
        return nullptr;

    if (void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, size))
    {
        memcpy(view, utf8.c_str(), size);
        UnmapViewOfFile(view);
    }

    return mapping;
}

static std::wstring QueryUserName()
{
    wchar_t buf[256]{};
    DWORD cch = ARRAYSIZE(buf);
    return GetUserNameW(buf, &cch) ? std::wstring(buf) : std::wstring();
}

static std::wstring QueryComputerName()
{
    wchar_t buf[256]{};
    DWORD cch = ARRAYSIZE(buf);
    return GetComputerNameW(buf, &cch) ? std::wstring(buf) : std::wstring();
}

static std::wstring WithSeparator(std::wstring path)
{
    if (!path.empty() && path.back() != L'\\')
        path += L'\\';
    return path;
}

// Runs in the child process. Returns the number of failed assertions.
static int RunLauncherConfigChild(const wchar_t* dllDirArg, const wchar_t* baseDirArg, const wchar_t* modeArg)
{
    // Passed without trailing separators; see RunIsolatedChild.
    const std::wstring dllDir  = WithSeparator(dllDirArg);
    const std::wstring baseDir = WithSeparator(baseDirArg);
    const bool merge = wcscmp(modeArg, L"merge") == 0;

    const std::wstring baseInterposer = baseDir + L".interposer\\";
    const std::wstring dllInterposer  = dllDir + L".interposer\\";

    wprintf(merge ? L"\n--- Launcher Config Tests: overlay over Config.yml (child process) ---\n"
                  : L"\n--- Launcher Config Tests: overlay only (child process) ---\n");

    std::string baseUtf8 = WideToUtf8(baseDir);

    const std::string overlay = merge
        ? "Player:\n"
          "  Username: OverlayUser\n"
          "OsVersion: ~\n"
          "Plugins:\n"
          "  Probe:\n"
          "    Shared: overlay\n"
          "    FromOverlay: overlay\n"
        : "Player:\n"
          "  Username: OnlyOverlay\n"
          "Plugins:\n"
          "  Probe:\n"
          "    Key: value\n";

    HANDLE baseMapping    = CreateLauncherMapping(L"InterposerBaseDirectory", baseUtf8);
    HANDLE overlayMapping = CreateLauncherMapping(L"InterposerConfig", overlay);

    if (!baseMapping || !overlayMapping)
    {
        wprintf(L"  FAIL  LC-00: creating the launcher mappings (GLE=%lu)\n", GetLastError());
        return 1;
    }

    HMODULE hDll = LoadLibraryW((dllDir + L"LANCommander.Interposer.dll").c_str());

    if (!hDll)
    {
        wprintf(L"  FAIL  LC-00: child could not load the DLL (GLE=%lu)\n", GetLastError());
        return 1;
    }

    auto get = reinterpret_cast<FnTestGetConfig>(
        reinterpret_cast<void*>(GetProcAddress(hDll, "InterposerGetConfigString")));
    auto set = reinterpret_cast<FnTestSetConfig>(
        reinterpret_cast<void*>(GetProcAddress(hDll, "InterposerSetConfigString")));

    if (merge)
    {
        ASSERT(QueryUserName() == L"OverlayUser",
            L"LC-01: a key the overlay sets wins over Config.yml");

        // FileComputer, not DecoyComputer: the base directory's Config.yml was
        // read, the one beside the DLL was not, and a sibling key the overlay did
        // not name survived the merge.
        ASSERT(QueryComputerName() == L"FileComputer",
            L"LC-02: Config.yml comes from the base directory and keys the overlay omits survive");

        // Config.yml asks for Windows XP; the overlay's null removes the section.
        {
            using FnRtlGetVersion = LONG (WINAPI*)(OSVERSIONINFOW*);
            auto rtlGetVersion = reinterpret_cast<FnRtlGetVersion>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));

            OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);

            ASSERT(rtlGetVersion && rtlGetVersion(&info) == 0 && info.dwMajorVersion >= 6,
                L"LC-03: a null in the overlay removes the section Config.yml set");
        }

        ASSERT(get && GetConfig(get, L"Plugins.Probe.Shared") == L"overlay" &&
                      GetConfig(get, L"Plugins.Probe.FromFile") == L"file" &&
                      GetConfig(get, L"Plugins.Probe.FromOverlay") == L"overlay",
            L"LC-04: maps merge key by key, all the way down");

        ASSERT(set && set(L"Plugins.Probe.Written", L"yes") &&
                      GetConfig(get, L"Plugins.Probe.Written") == L"yes",
            L"LC-05: a plugin write succeeds under an overlay");

        // The base directory's store is seeded with this key, so it is virtual.
        {
            const wchar_t* written = L"LauncherWrite";
            LSTATUS st = RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\LauncherConfigSeed", L"Probe",
                REG_SZ, written, static_cast<DWORD>((wcslen(written) + 1) * sizeof(wchar_t)));

            ASSERT(st == ERROR_SUCCESS, L"LC-06: a write to a key the base directory's store defines succeeds");
        }

        // %INTERPOSERDIR% is the base directory, not the DLL's.
        {
            HANDLE h = CreateFileW((dllDir + L"lc_redirect_probe.txt").c_str(), GENERIC_WRITE, 0,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

            // (No percent signs in ASSERT messages: they become part of a wprintf format.)
            ASSERT(h != INVALID_HANDLE_VALUE, L"LC-07: a write through an INTERPOSERDIR redirect opens");

            if (h != INVALID_HANDLE_VALUE)
            {
                DWORD w = 0;
                ::WriteFile(h, "x", 1, &w, nullptr);
                CloseHandle(h);
            }
        }
    }
    else
    {
        ASSERT(QueryUserName() == L"OnlyOverlay",
            L"LO-01: the overlay applies with no Config.yml at all");

        ASSERT(QueryComputerName() != L"DecoyComputer",
            L"LO-02: the Config.yml beside the DLL is not read");

        ASSERT(get && GetConfig(get, L"Plugins.Probe.Key") == L"value",
            L"LO-03: the overlay is what config reads see");
    }

    FreeLibrary(hDll);
    CloseHandle(baseMapping);
    CloseHandle(overlayMapping);

    // Hooks are gone; what follows sees the real file system.
    const std::string log = ReadFileAsUtf8(FindFirstLogFile(baseInterposer + L"Logs\\"));

    if (merge)
    {
        ASSERT(ReadFileAsWide(baseInterposer + L"Registry.reg").find(L"LauncherWrite") != std::wstring::npos &&
               GetFileAttributesW((dllInterposer + L"Registry.reg").c_str()) == INVALID_FILE_ATTRIBUTES,
            L"LC-08: the virtual registry persists to the base directory, not beside the DLL");

        ASSERT(log.find("applied the launcher's config overlay") != std::string::npos &&
               FindFirstLogFile(dllInterposer + L"Logs\\").empty(),
            L"LC-09: the session log is written under the base directory and records the overlay");

        // Probe is a flow map in the file, so the patcher declines and the
        // fallback re-renders the section -- from the file's own tree, so none of
        // the overlay's values leak into the file.
        {
            const std::string yml = ReadFileAsUtf8(baseInterposer + L"Config.yml");

            ASSERT(yml.find("Written: yes") != std::string::npos &&
                   yml.find("FromFile: file") != std::string::npos &&
                   yml.find("overlay") == std::string::npos &&
                   yml.find("OverlayUser") == std::string::npos,
                L"LC-10: a plugin write reaches Config.yml without baking the overlay into it");
        }

        ASSERT(GetFileAttributesW((baseDir + L"redirected.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
            L"LC-11: the INTERPOSERDIR token expands to the base directory");
    }
    else
    {
        ASSERT(log.find("no Config.yml") != std::string::npos,
            L"LO-04: the base directory is created, and the log there records the overlay");
    }

    return g_fail;
}

// Runs in the parent. Stages one child per mode and folds the outcomes into
// this process's tally.
static void RunLauncherConfigTests(const std::wstring& exeDir, const std::wstring& testTmpDir)
{
    const std::wstring root    = testTmpDir + L"LauncherConfig\\";
    const std::wstring exePath = exeDir + L"LANCommander.Interposer.Tests.exe";

    for (const wchar_t* mode : { L"merge", L"overlay-only" })
    {
        const bool merge = wcscmp(mode, L"merge") == 0;

        const std::wstring childDir = root + mode + L"\\";
        const std::wstring dllDir   = childDir + L"Dll\\";

        // The overlay-only base directory is left uncreated, two levels deep.
        const std::wstring baseDir  = merge ? childDir + L"Base\\" : childDir + L"Base\\Nested\\Deeper\\";

        CreateDirs(dllDir + L".interposer\\");

        if (!CopyFileW((exeDir + L"LANCommander.Interposer.dll").c_str(),
                       (dllDir + L"LANCommander.Interposer.dll").c_str(), FALSE))
        {
            ASSERT(false, L"LC-00: staging a launcher-config child's copy of the DLL");
            continue;
        }

        // The decoy: read only if the base directory override were ignored.
        WriteTextFile(dllDir + L".interposer\\Config.yml",
            "Player:\n"
            "  ComputerName: DecoyComputer\n");

        if (merge)
        {
            CreateDirs(baseDir + L".interposer\\");

            WriteTextFile(baseDir + L".interposer\\Config.yml",
                "Logging:\n"
                "  Identity: true\n"
                "\n"
                "Player:\n"
                "  Username: FileUser\n"
                "  ComputerName: FileComputer\n"
                "\n"
                "OsVersion:\n"
                "  Version: 'Windows XP'\n"
                "\n"
                "FileRedirects:\n"
                "  - Pattern: '.*\\\\lc_redirect_probe\\.txt$'\n"
                "    Replacement: '%INTERPOSERDIR%\\redirected.txt'\n"
                "\n"
                "Plugins:\n"
                "  Probe: { FromFile: file, Shared: file }\n");

            WriteTextFile(baseDir + L".interposer\\Registry.reg",
                "Windows Registry Editor Version 5.00\r\n"
                "\r\n"
                "[HKEY_CURRENT_USER\\Software\\LauncherConfigSeed]\r\n"
                "\"Seeded\"=\"yes\"\r\n");
        }

        // Trailing separators come off for the command line; see RunIsolatedTests.
        const std::wstring command = L"\"" + exePath + L"\" --launcher-config \"" +
            dllDir.substr(0, dllDir.size() - 1) + L"\" \"" +
            baseDir.substr(0, baseDir.size() - 1) + L"\" " + mode;

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        std::wstring mutableCommand = command;

        if (!CreateProcessW(exePath.c_str(), mutableCommand.data(), nullptr, nullptr,
                TRUE, 0, nullptr, nullptr, &si, &pi))
        {
            ASSERT(false, L"LC-00: launching a launcher-config child process");
            continue;
        }

        WaitForSingleObject(pi.hProcess, 120 * 1000);

        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (merge)
            ASSERT(exitCode == 0, L"LC-00: the overlay-over-Config.yml child reported no failures");
        else
            ASSERT(exitCode == 0, L"LO-00: the overlay-only child reported no failures");
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// ============================================================
// FastDL tests  (overlay only -- no server)
// ============================================================
// The fixture maps %GAMEDIR%\FastDLProbe to the remote folder "maps" with no
// BaseUrl, so nothing is ever downloaded; what is tested is that a file already
// in the download overlay is served in place of the missing original.

static void RunFastDLTests(const std::wstring& exeDir)
{
    wprintf(L"\n--- FastDL Tests (overlay) ---\n");

    const std::wstring overlayDir  = exeDir + L".interposer\\Downloads\\maps\\";
    const std::wstring overlayFile = overlayDir + L"map.txt";

    CreateDirs(overlayDir);
    WriteTextFile(overlayFile, "from overlay");

    ASSERT(ReadFileAsUtf8(exeDir + L"FastDLProbe\\map.txt") == "from overlay",
        L"FD-01: a Local prefix built from the GAMEDIR token serves the overlay copy of a missing file");

    // A game that opens files relative to its working directory never passes the
    // absolute path the prefix names; the full-path fallback has to catch it.
    {
        wchar_t previous[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, previous);
        SetCurrentDirectoryW(exeDir.c_str());

        ASSERT(ReadFileAsUtf8(L"FastDLProbe\\map.txt") == "from overlay",
            L"FD-02: a relative open under the prefix is served from the overlay too");

        SetCurrentDirectoryW(previous);
    }

    ASSERT(ReadFileAsUtf8(exeDir + L"NotFastDL\\map.txt").empty(),
        L"FD-03: a path outside every Local prefix is not served from the overlay");

    DeleteFileW(overlayFile.c_str());
    RemoveDirectoryW(overlayDir.c_str());
    RemoveDirectoryW((exeDir + L".interposer\\Downloads").c_str());
}

// ============================================================
// SDK tests  (the import library, in this process)
// ============================================================
// sdk.c calls the API the way a game patch does: the public header and the
// import library, compiled as C with stdcall as the default convention. The
// import is delay-loaded, so it binds on the first call here -- after wmain has
// written the fixtures and loaded the DLL -- and must bind to that same copy.

extern "C" BOOL  __cdecl SdkGetConfigString(const wchar_t* dotPath, wchar_t* buffer, DWORD bufferSize);
extern "C" BOOL  __cdecl SdkGetUsername(wchar_t* buffer, DWORD bufferSize);
extern "C" void  __cdecl SdkLog(const wchar_t* verb, const wchar_t* message);
extern "C" DWORD __cdecl SdkSetRegistryValueBySuffix(const wchar_t* keySuffix, const wchar_t* valueName, const wchar_t* value);
extern "C" DWORD __cdecl SdkEnumNetworkAdapterCount(void);
extern "C" void* __cdecl SdkAddressOfGetConfigString(void);
extern "C" unsigned __cdecl SdkVoiceApiVersion(void);

static const wchar_t* kSdkLogVerb    = L"SDK PROBE";
static const wchar_t* kSdkLogMessage = L"written through the import library";

// ── Rich presence events (RP-) ────────────────────────────────────────────────
// The DLL reports presence to the launcher through InterposerPresenceCallback
// (and the event pipe beside it), whether or not a Discord backend connected --
// the fixture configures none.

static std::vector<std::pair<std::wstring, std::wstring>> g_presenceEvents;

static void __cdecl OnPresenceEvent(const wchar_t* kind, const wchar_t* json)
{
    g_presenceEvents.emplace_back(kind ? kind : L"", json ? json : L"<null>");
}

static void RunPresenceTests(HMODULE hDll)
{
    wprintf(L"\n--- Rich Presence Event Tests ---\n");

    using FnRegister   = void (__cdecl*)(void (__cdecl*)(const wchar_t*, const wchar_t*));
    using FnSetString  = void (__cdecl*)(const char*);
    using FnSetParty   = void (__cdecl*)(const char*, int, int);
    using FnSetTimes   = void (__cdecl*)(long long, long long);
    using FnVoid       = void (__cdecl*)();

    auto registerCb = reinterpret_cast<FnRegister>(GetProcAddress(hDll, "InterposerRegisterPresenceCallback"));
    auto setDetails = reinterpret_cast<FnSetString>(GetProcAddress(hDll, "InterposerSetPresenceDetails"));
    auto setState   = reinterpret_cast<FnSetString>(GetProcAddress(hDll, "InterposerSetPresenceState"));
    auto setParty   = reinterpret_cast<FnSetParty>(GetProcAddress(hDll, "InterposerSetPresenceParty"));
    auto setTimes   = reinterpret_cast<FnSetTimes>(GetProcAddress(hDll, "InterposerSetPresenceTimestamps"));
    auto update     = reinterpret_cast<FnVoid>(GetProcAddress(hDll, "InterposerUpdatePresence"));
    auto clear      = reinterpret_cast<FnVoid>(GetProcAddress(hDll, "InterposerClearPresence"));

    ASSERT(registerCb != nullptr, L"RP-01: InterposerRegisterPresenceCallback is exported");

    if (!registerCb || !setDetails || !setState || !setParty || !setTimes || !update || !clear)
        return;

    registerCb(OnPresenceEvent);

    // Quotes, a backslash, a newline and non-ASCII UTF-8 (U+00E9) must survive as JSON.
    setDetails("de_dust2 \"Competitive\" \\ A\nB caf\xC3\xA9");
    setState("Score: 7 - 3");
    setParty("lobby-1", 3, 8);
    setTimes(1700000000LL, 0);

    ASSERT(g_presenceEvents.empty(), L"RP-02: setters stage changes without firing an event");

    update();

    ASSERT(g_presenceEvents.size() == 1 && g_presenceEvents[0].first == L"UPDATE",
        L"RP-03: InterposerUpdatePresence fires one UPDATE event");

    if (g_presenceEvents.size() == 1)
    {
        const std::wstring& json = g_presenceEvents[0].second;

        ASSERT(json.find(L"\"details\":\"de_dust2 \\\"Competitive\\\" \\\\ A\\nB caf\xE9\"") != std::wstring::npos,
            L"RP-04: details is escaped JSON and decoded from UTF-8");
        ASSERT(json.find(L"\"state\":\"Score: 7 - 3\"") != std::wstring::npos,
            L"RP-05: state is carried");
        ASSERT(json.find(L"\"partySize\":3") != std::wstring::npos && json.find(L"\"partyMax\":8") != std::wstring::npos,
            L"RP-06: party size and max are numbers");
        ASSERT(json.find(L"\"timestampStart\":1700000000") != std::wstring::npos,
            L"RP-07: the start timestamp is carried as a 64-bit number");
        ASSERT(json.front() == L'{' && json.back() == L'}' && json.find(L",}") == std::wstring::npos,
            L"RP-08: the payload is one JSON object with no trailing comma");
    }

    clear();

    ASSERT(g_presenceEvents.size() == 2 && g_presenceEvents[1].first == L"CLEAR" && g_presenceEvents[1].second == L"<null>",
        L"RP-09: InterposerClearPresence fires CLEAR with no payload");

    registerCb(nullptr);
    update();

    ASSERT(g_presenceEvents.size() == 2, L"RP-10: no event fires once the callback is unregistered");
}

static void RunSdkTests(HMODULE hDll)
{
    wprintf(L"\n--- SDK Tests (import library) ---\n");

    wchar_t buf[256]{};

    ASSERT(SdkGetConfigString(L"Player.Username", buf, ARRAYSIZE(buf)) && wcscmp(buf, L"TestPlayer") == 0,
        L"SDK-01: InterposerGetConfigString through the import library reads the fixture");

    // The delay-load helper resolves by module name. It must have found the copy
    // already in the process rather than loading another.
    ASSERT(SdkAddressOfGetConfigString() ==
               reinterpret_cast<void*>(GetProcAddress(hDll, "InterposerGetConfigString")),
        L"SDK-02: the import binds to the Interposer already loaded");

    buf[0] = L'\0';

    ASSERT(SdkGetUsername(buf, ARRAYSIZE(buf)) && wcscmp(buf, L"TestPlayer") == 0,
        L"SDK-03: InterposerGetUsername through the import library");

    // Three arguments and a return value: the call a convention mismatch would
    // corrupt first on x86.
    {
        const DWORD updated = SdkSetRegistryValueBySuffix(L"TestGame\\1.0\\SubKey", L"SdkProbe", L"FromSdk");

        wchar_t value[64]{};
        DWORD   cb = sizeof(value);
        HKEY    key = nullptr;

        const bool read =
            RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TestGame\\1.0\\SubKey", 0, KEY_READ, &key) == ERROR_SUCCESS &&
            RegQueryValueExW(key, L"SdkProbe", nullptr, nullptr, reinterpret_cast<BYTE*>(value), &cb) == ERROR_SUCCESS;

        if (key)
            RegCloseKey(key);

        ASSERT(updated == 1 && read && wcscmp(value, L"FromSdk") == 0,
            L"SDK-04: InterposerSetRegistryValueBySuffix through the import library reaches the virtual store");
    }

    {
        using FnEnum = DWORD (*)(void*, DWORD, DWORD*);
        auto direct = reinterpret_cast<FnEnum>(
            reinterpret_cast<void*>(GetProcAddress(hDll, "InterposerEnumNetworkAdapters")));

        DWORD written = 0;

        ASSERT(direct && SdkEnumNetworkAdapterCount() == direct(nullptr, 0, &written),
            L"SDK-05: InterposerEnumNetworkAdapters through the import library");
    }

    // The voice API is in the same import library.
    ASSERT(SdkVoiceApiVersion() == INTERPOSER_COMMS_API_VERSION,
        L"SDK-07: interposer_comms_get_api_version through the import library");

    // Checked in the session log once the DLL has unloaded; see RunSdkLogTests.
    SdkLog(kSdkLogVerb, kSdkLogMessage);

    // The delay-load helper took a reference of its own when it bound. Drop it,
    // or wmain's FreeLibrary would leave the DLL loaded and its log open.
    __FUnloadDelayLoadedDLL2("LANCommander.Interposer.dll");
}

static void RunSdkLogTests(const std::wstring& logPath)
{
    const std::string log = ReadFileAsUtf8(logPath);

    ASSERT(CountLinesContaining(log, "[SDK PROBE]", WideToUtf8(kSdkLogMessage).c_str()) == 1,
        L"SDK-06: InterposerLog through the import library writes one session log line");
}

// ============================================================
// Voice tests
// ============================================================
// The voice library (core/voice/, the interposer_comms_* API) is exported beside
// Interposer*. A server started on loopback logs one line, which reaches the
// session log only once the patch asks for it (interposer_comms_set_session_logging).

template <typename Fn>
static Fn VoiceExport(HMODULE module, const char* name)
{
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// A voice server on loopback, an ephemeral port, through the given copy's
// exports. Starting one logs "voice server listening on port".
static interposer_comms_server* StartVoiceServer(HMODULE module)
{
    auto init    = VoiceExport<decltype(&interposer_comms_server_config_init)>(module, "interposer_comms_server_config_init");
    auto create  = VoiceExport<decltype(&interposer_comms_server_create)>(module, "interposer_comms_server_create");
    auto start   = VoiceExport<decltype(&interposer_comms_server_start)>(module, "interposer_comms_server_start");
    auto destroy = VoiceExport<decltype(&interposer_comms_server_destroy)>(module, "interposer_comms_server_destroy");

    if (!init || !create || !start || !destroy)
        return nullptr;

    interposer_comms_server_config config;
    init(&config);
    config.port         = 0;
    config.bind_address = "127.0.0.1";

    interposer_comms_server* server = nullptr;

    if (create(&config, &server) != INTERPOSER_COMMS_OK)
        return nullptr;

    if (start(server) != INTERPOSER_COMMS_OK)
    {
        destroy(server);
        return nullptr;
    }

    return server;
}

static void StopVoiceServer(HMODULE module, interposer_comms_server* server)
{
    auto destroy = VoiceExport<decltype(&interposer_comms_server_destroy)>(module, "interposer_comms_server_destroy");

    if (destroy && server)
        destroy(server);
}

// An interposer_comms_log_fn that counts the server's "listening" line.
static void INTERPOSER_CALL CountVoiceListening(interposer_comms_log_level, const char* message, void* userData)
{
    if (strstr(message, "voice server listening"))
        ++*static_cast<int*>(userData);
}

static void RunVoiceTests(HMODULE hDll)
{
    wprintf(L"\n--- Voice Tests ---\n");

    auto apiVersion = VoiceExport<decltype(&interposer_comms_get_api_version)>(hDll, "interposer_comms_get_api_version");

    ASSERT(apiVersion && apiVersion() == INTERPOSER_COMMS_API_VERSION,
        L"VC-01: interposer_comms_get_api_version is exported and returns the API version");

    auto versionString = VoiceExport<decltype(&interposer_comms_get_version_string)>(hDll, "interposer_comms_get_version_string");

    ASSERT(versionString && versionString() && versionString()[0] != '\0',
        L"VC-02: interposer_comms_get_version_string is exported");

    ASSERT(GetProcAddress(hDll, "interposer_comms_server_create") && GetProcAddress(hDll, "interposer_comms_client_create"),
        L"VC-03: the server and client voice API are both present");

    ASSERT(!GetProcAddress(hDll, "comms_get_api_version") && !GetProcAddress(hDll, "comms_server_create"),
        L"VC-04: the un-prefixed comms_ names are not exported");

    // Checked in the session log once the DLL has unloaded; see RunVoiceLogTests.
    // The first server starts before session logging is on, and must not be in it.
    StopVoiceServer(hDll, StartVoiceServer(hDll));

    auto setSession = VoiceExport<decltype(&interposer_comms_set_session_logging)>(hDll, "interposer_comms_set_session_logging");

    if (setSession)
        setSession(INTERPOSER_COMMS_LOG_INFO);

    interposer_comms_server* server = StartVoiceServer(hDll);

    ASSERT(server != nullptr, L"VC-05: a voice server starts on loopback");

    StopVoiceServer(hDll, server);

    if (setSession)
        setSession(INTERPOSER_COMMS_LOG_OFF);
}

static void RunVoiceLogTests(const std::wstring& logPath)
{
    const std::string log = ReadFileAsUtf8(logPath);

    ASSERT(CountLinesContaining(log, "[VOICE]", "voice server listening on port") == 1,
        L"VC-06: voice output reaches the session log once, and only, after interposer_comms_set_session_logging");
}

// ============================================================
// Second instance tests  (separate process)
// ============================================================
// A game patch that links the Interposer can land in a game LANCommander has
// also deployed it to, so two copies end up in one process. The first to load
// must own every hook; the second must install none and forward its exports to
// the first. Two copies of the DLL would detour the same KernelBase exports,
// which this process has already done once, so it gets a child of its own.
//
// Each copy has its own .interposer tree. Their FileRedirects send one probe
// file to different places, so where the probe lands says whose hooks ran: the
// second copy's rules cover both orders MinHook could chain the two detours in.

static bool PathExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool TouchFile(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    CloseHandle(h);
    return true;
}

// The session log is still open for append while the primary is loaded, so it
// has to be read sharing write access -- which ReadFileAsUtf8 does not.
static std::string ReadOpenLog(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return {};

    std::string out;
    char  chunk[4096];
    DWORD read = 0;

    while (::ReadFile(h, chunk, sizeof(chunk), &read, nullptr) && read)
        out.append(chunk, read);

    CloseHandle(h);
    return out;
}

// Runs in the child process. Returns the number of failed assertions.
static int RunSecondInstanceChild(const wchar_t* primaryDirArg, const wchar_t* secondaryDirArg)
{
    // Passed without trailing separators; see RunIsolatedChild.
    const std::wstring primaryDir   = WithSeparator(primaryDirArg);
    const std::wstring secondaryDir = WithSeparator(secondaryDirArg);

    wprintf(L"\n--- Second Instance Tests (child process) ---\n");

    HMODULE primary   = LoadLibraryW((primaryDir + L"LANCommander.Interposer.dll").c_str());
    HMODULE secondary = primary ? LoadLibraryW((secondaryDir + L"LANCommander.Interposer.dll").c_str()) : nullptr;

    if (!primary || !secondary || primary == secondary)
    {
        wprintf(L"  FAIL  MI-00: child could not load both copies of the DLL (GLE=%lu)\n", GetLastError());
        return 1;
    }

    using FnLog = void (*)(const wchar_t*, const wchar_t*);

    auto get = reinterpret_cast<FnTestGetConfig>(
        reinterpret_cast<void*>(GetProcAddress(secondary, "InterposerGetConfigString")));
    auto log = reinterpret_cast<FnLog>(
        reinterpret_cast<void*>(GetProcAddress(secondary, "InterposerLog")));

    ASSERT(get && GetConfig(get, L"Plugins.Probe.Owner") == L"primary",
        L"MI-01: the second copy's exports answer from the first copy's configuration");

    ASSERT(!PathExists(secondaryDir + L".interposer\\Logs"),
        L"MI-02: the second copy loads no configuration and opens no session log");

    const std::wstring probe        = primaryDir + L"instance_probe.txt";
    const std::wstring fromPrimary  = primaryDir + L"from_primary.txt";
    const std::wstring fromSecondary = primaryDir + L"from_secondary.txt";

    TouchFile(probe);

    ASSERT(PathExists(fromPrimary) && !PathExists(fromSecondary),
        L"MI-03: only the first copy's file hooks run");

    if (log)
        log(L"INSTANCE", L"forwarded by the second copy");

    {
        const std::string text = ReadOpenLog(FindFirstLogFile(primaryDir + L".interposer\\Logs\\"));

        ASSERT(CountLinesContaining(text, "[INSTANCE]", "forwarded by the second copy") == 1,
            L"MI-04: InterposerLog through the second copy writes to the first copy's log");
    }

    // Voice forwards like the rest of the API. A handler set through the second
    // copy is only heard by a server started through the first if the handler
    // was forwarded; a server started through the second copy is only in the
    // first copy's log if the server functions were.
    {
        int heard = 0;
        auto setHandler = VoiceExport<decltype(&interposer_comms_set_log_handler)>(secondary, "interposer_comms_set_log_handler");
        auto setSession = VoiceExport<decltype(&interposer_comms_set_session_logging)>(secondary, "interposer_comms_set_session_logging");

        // Through the second copy too, so MI-08 also proves it forwards: the
        // second copy has no session log of its own to write to.
        if (setSession)
            setSession(INTERPOSER_COMMS_LOG_INFO);

        if (setHandler)
            setHandler(CountVoiceListening, INTERPOSER_COMMS_LOG_INFO, &heard);

        StopVoiceServer(primary, StartVoiceServer(primary));

        ASSERT(heard == 1, L"MI-07: a voice log handler set through the second copy hears the first copy's server");

        if (setHandler)
            setHandler(nullptr, INTERPOSER_COMMS_LOG_OFF, nullptr);

        StopVoiceServer(secondary, StartVoiceServer(secondary));

        const std::string text = ReadOpenLog(FindFirstLogFile(primaryDir + L".interposer\\Logs\\"));

        ASSERT(CountLinesContaining(text, "[VOICE]", "voice server listening") == 2,
            L"MI-08: voice servers started through either copy run in the first copy and log to its session log");
    }

    // Match statistics forward like the rest: a handle made through either
    // copy names the same match in both.
    {
        using FnStart  = DWORD (__cdecl*)(const InterposerMatchInfo*);
        using FnEvent  = BOOL (__cdecl*)(DWORD, const char*, long long, long long, const char*, const char*, const char*);
        using FnStatus = BOOL (__cdecl*)(DWORD, InterposerMatchStatus*);

        auto start  = reinterpret_cast<FnStart>(reinterpret_cast<void*>(GetProcAddress(secondary, "InterposerMatchStart")));
        auto event  = reinterpret_cast<FnEvent>(reinterpret_cast<void*>(GetProcAddress(primary, "InterposerMatchEvent")));
        auto status = reinterpret_cast<FnStatus>(reinterpret_cast<void*>(GetProcAddress(primary, "InterposerMatchGetStatus")));

        InterposerMatchInfo info{};
        info.structSize = sizeof(info);
        info.externalId = "mi-09";

        const DWORD match = start ? start(&info) : 0;

        InterposerMatchStatus st{};
        st.size = sizeof(st);

        ASSERT(match != 0 && event && event(match, "probe", 1, -1, nullptr, nullptr, nullptr) &&
               status && status(match, &st),
            L"MI-09: a match started through the second copy is recorded by the first");
    }

    // Unloading the second copy must not take any hooks with it.
    FreeLibrary(secondary);
    DeleteFileW(fromPrimary.c_str());
    TouchFile(probe);

    ASSERT(PathExists(fromPrimary) && !PathExists(fromSecondary),
        L"MI-05: the first copy's hooks survive the second copy unloading");

    // The second copy pinned the first, so nothing it forwarded to can be
    // unmapped. The pin outlives the second copy.
    FreeLibrary(primary);

    ASSERT(GetModuleHandleW((primaryDir + L"LANCommander.Interposer.dll").c_str()) == primary,
        L"MI-06: the first copy stays loaded once a second copy has pinned it");

    return g_fail;
}

static void RunSecondInstanceTests(const std::wstring& exeDir, const std::wstring& testTmpDir)
{
    wprintf(L"\n--- Second Instance Tests ---\n");

    const std::wstring root         = testTmpDir + L"SecondInstance\\";
    const std::wstring primaryDir   = root + L"Primary\\";
    const std::wstring secondaryDir = root + L"Secondary\\";
    const std::wstring sourceDll    = exeDir + L"LANCommander.Interposer.dll";

    const std::string fromPrimary   = WideToUtf8(primaryDir) + "from_primary.txt";
    const std::string fromSecondary = WideToUtf8(primaryDir) + "from_secondary.txt";

    CreateDirs(primaryDir + L".interposer\\");
    CreateDirs(secondaryDir + L".interposer\\");

    if (!CopyFileW(sourceDll.c_str(), (primaryDir + L"LANCommander.Interposer.dll").c_str(), FALSE) ||
        !CopyFileW(sourceDll.c_str(), (secondaryDir + L"LANCommander.Interposer.dll").c_str(), FALSE))
    {
        ASSERT(false, L"MI-00: staging two copies of the DLL");
        return;
    }

    WriteTextFile(primaryDir + L".interposer\\Config.yml",
        "FileRedirects:\n"
        "  - Pattern: '.*\\\\instance_probe\\.txt$'\n"
        "    Replacement: '" + fromPrimary + "'\n"
        "\n"
        "Plugins:\n"
        "  Probe:\n"
        "    Owner: primary\n");

    // Matches the probe itself, for a detour chained outside the first copy's,
    // and the first copy's output, for one chained inside it.
    WriteTextFile(secondaryDir + L".interposer\\Config.yml",
        "FileRedirects:\n"
        "  - Pattern: '.*\\\\instance_probe\\.txt$'\n"
        "    Replacement: '" + fromSecondary + "'\n"
        "  - Pattern: '.*\\\\from_primary\\.txt$'\n"
        "    Replacement: '" + fromSecondary + "'\n"
        "\n"
        "Plugins:\n"
        "  Probe:\n"
        "    Owner: secondary\n");

    wchar_t exePathBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    const std::wstring exePath = exePathBuf;

    // Trailing separators come off for the command line; see RunIsolatedTests.
    std::wstring command = L"\"" + exePath + L"\" --second-instance \"" +
        primaryDir.substr(0, primaryDir.size() - 1) + L"\" \"" +
        secondaryDir.substr(0, secondaryDir.size() - 1) + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(exePath.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
    {
        ASSERT(false, L"MI-00: launching the second-instance child process");
        return;
    }

    WaitForSingleObject(pi.hProcess, 120 * 1000);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    ASSERT(exitCode == 0, L"MI-00: the second-instance child reported no failures");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// ============================================================
// Embedded tests  (separate processes)
// ============================================================
// A patch can carry the Interposer inside its own DLL (LANCommander.Interposer.
// Static, see LANCommander.Interposer.Embedded.h). LANCommander.Interposer.Tests.
// Patch.dll is such a patch, staged as dinput.dll the way a DirectInput patch
// ships. Each order of "patch, then LANCommander.Interposer.dll" gets a child of
// its own, because whichever loads first installs hooks this process cannot
// take back. Each image has its own .interposer tree, and as in the MI- tests
// their FileRedirects send one probe file to different places.

using FnPatchIsPrimary    = BOOL (*)();
using FnPatchCalls        = LONG (*)();
using FnDirectInputCreate = HRESULT (WINAPI*)(HINSTANCE, DWORD, void**, void*);

template <typename Fn>
static Fn ExportOf(HMODULE module, const char* name)
{
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// Creates a DirectInput 7 object through the patch's own export and releases
// it again. True if the export produced an object.
static bool CreateDirectInputThrough(HMODULE patch)
{
    auto create = ExportOf<FnDirectInputCreate>(patch, "DirectInputCreateA");

    void* di = nullptr;

    if (!create || FAILED(create(GetModuleHandleW(nullptr), 0x0700, &di, nullptr)) || !di)
        return false;

    using FnRelease = ULONG (WINAPI*)(void*);
    (*reinterpret_cast<FnRelease**>(di))[2](di);
    return true;
}

// Runs in the child process. Returns the number of failed assertions.
static int RunEmbeddedChild(const wchar_t* modeArg, const wchar_t* patchDirArg, const wchar_t* dllDirArg)
{
    // Passed without trailing separators; see RunIsolatedChild.
    const std::wstring patchDir = WithSeparator(patchDirArg);
    const std::wstring dllDir   = WithSeparator(dllDirArg);
    const bool patchFirst = wcscmp(modeArg, L"patch-first") == 0;

    wprintf(L"\n--- Embedded Tests: %ls (child process) ---\n", modeArg);

    const std::wstring patchPath = patchDir + L"dinput.dll";
    const std::wstring dllPath   = dllDir + L"LANCommander.Interposer.dll";

    const std::wstring probe     = patchDir + L"embedded_probe.txt";
    const std::wstring fromPatch = patchDir + L"from_patch.txt";
    const std::wstring fromDll   = patchDir + L"from_dll.txt";

    if (patchFirst)
    {
        HMODULE patch = LoadLibraryW(patchPath.c_str());

        if (!patch)
        {
            wprintf(L"  FAIL  EM-00: child could not load the patch (GLE=%lu)\n", GetLastError());
            return 1;
        }

        auto isPrimary = ExportOf<FnPatchIsPrimary>(patch, "PatchProbeIsPrimary");
        auto direct    = ExportOf<FnTestGetConfig>(patch, "PatchProbeGetConfig");
        auto exported  = ExportOf<FnTestGetConfig>(patch, "InterposerGetConfigString");

        ASSERT(isPrimary && isPrimary(),
            L"EM-01: an embedded Interposer loaded first owns the process");

        ASSERT(exported && GetConfig(exported, L"Plugins.Probe.Owner") == L"patch",
            L"EM-02: the patch's image exports the Interposer API, answering from the Config.yml beside the patch");

        ASSERT(direct && GetConfig(direct, L"Plugins.Probe.Owner") == L"patch",
            L"EM-03: the patch's own calls into the embedded Interposer read the same configuration");

        ASSERT(!FindFirstLogFile(patchDir + L".interposer\\Logs\\").empty(),
            L"EM-04: the embedded Interposer opens its session log beside the patch");

        TouchFile(probe);

        ASSERT(PathExists(fromPatch) && !PathExists(fromDll),
            L"EM-05: the embedded Interposer's file hooks run");

        ASSERT(CreateDirectInputThrough(patch),
            L"EM-06: the patch's DirectInputCreateA creates an object through InterposerDirectInputCreateA");

        HMODULE dll = LoadLibraryW(dllPath.c_str());

        if (!dll)
        {
            wprintf(L"  FAIL  EM-00: child could not load LANCommander.Interposer.dll (GLE=%lu)\n", GetLastError());
            return g_fail + 1;
        }

        auto dllGet = ExportOf<FnTestGetConfig>(dll, "InterposerGetConfigString");

        DeleteFileW(fromPatch.c_str());
        TouchFile(probe);

        ASSERT(dllGet && GetConfig(dllGet, L"Plugins.Probe.Owner") == L"patch" &&
               PathExists(fromPatch) && !PathExists(fromDll) &&
               !PathExists(dllDir + L".interposer\\Logs"),
            L"EM-07: LANCommander.Interposer.dll loaded after the patch defers to the embedded copy");

        return g_fail;
    }

    HMODULE dll   = LoadLibraryW(dllPath.c_str());
    HMODULE patch = dll ? LoadLibraryW(patchPath.c_str()) : nullptr;

    if (!dll || !patch)
    {
        wprintf(L"  FAIL  EM-00: child could not load both images (GLE=%lu)\n", GetLastError());
        return 1;
    }

    auto isPrimary = ExportOf<FnPatchIsPrimary>(patch, "PatchProbeIsPrimary");
    auto direct    = ExportOf<FnTestGetConfig>(patch, "PatchProbeGetConfig");
    auto calls     = ExportOf<FnPatchCalls>(patch, "PatchProbeDirectInputCalls");

    ASSERT(isPrimary && !isPrimary(),
        L"EM-08: an embedded Interposer loaded second defers to the one already there");

    ASSERT(direct && GetConfig(direct, L"Plugins.Probe.Owner") == L"dll",
        L"EM-09: the patch's own calls forward to the Interposer that owns the process");

    TouchFile(probe);

    ASSERT(PathExists(fromDll) && !PathExists(fromPatch) && !PathExists(patchDir + L".interposer\\Logs"),
        L"EM-10: only the first Interposer's hooks run, and the embedded copy opens no log");

    // The DLL has FixLegacyDeviceEnumeration on, which detours dinput.dll's
    // DirectInputCreateA when it loads -- and the patch is what loaded under
    // that name. Detoured, the call would reach the bridge without the patch.
    ASSERT(CreateDirectInputThrough(patch) && calls && calls() == 1,
        L"EM-11: another Interposer leaves the patch's own DirectInputCreateA undetoured");

    return g_fail;
}

static void RunEmbeddedTests(const std::wstring& exeDir, const std::wstring& testTmpDir)
{
    wprintf(L"\n--- Embedded Tests ---\n");

    wchar_t exePathBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    const std::wstring exePath = exePathBuf;

    for (const wchar_t* mode : { L"patch-first", L"dll-first" })
    {
        const std::wstring root     = testTmpDir + L"Embedded\\" + mode + L"\\";
        const std::wstring patchDir = root + L"Patch\\";
        const std::wstring dllDir   = root + L"Dll\\";

        const std::string fromPatch = WideToUtf8(patchDir) + "from_patch.txt";
        const std::string fromDll   = WideToUtf8(patchDir) + "from_dll.txt";

        CreateDirs(patchDir + L".interposer\\");
        CreateDirs(dllDir + L".interposer\\");

        if (!CopyFileW((exeDir + L"LANCommander.Interposer.Tests.Patch.dll").c_str(),
                (patchDir + L"dinput.dll").c_str(), FALSE) ||
            !CopyFileW((exeDir + L"LANCommander.Interposer.dll").c_str(),
                (dllDir + L"LANCommander.Interposer.dll").c_str(), FALSE))
        {
            ASSERT(false, L"EM-00: staging the patch and the DLL");
            return;
        }

        const bool patchFirst = wcscmp(mode, L"patch-first") == 0;

        // As in the MI- tests, the image loaded second also redirects the first
        // one's output, which covers a detour chained inside the first's. Only
        // the second: the first's hooks run, and such a rule there would
        // redirect the test's own existence checks.
        auto config = [](const std::string& target, const std::string& firstOutput, const char* owner)
        {
            std::string text =
                "FileRedirects:\n"
                "  - Pattern: '.*\\\\embedded_probe\\.txt$'\n"
                "    Replacement: '" + target + "'\n";

            if (!firstOutput.empty())
                text += "  - Pattern: '.*\\\\" + firstOutput + "\\.txt$'\n"
                        "    Replacement: '" + target + "'\n";

            return text +
                "\n"
                "DirectInput:\n"
                "  FixLegacyDeviceEnumeration: true\n"
                "\n"
                "Plugins:\n"
                "  Probe:\n"
                "    Owner: " + owner + "\n";
        };

        WriteTextFile(patchDir + L".interposer\\Config.yml",
            config(fromPatch, patchFirst ? "" : "from_dll", "patch"));
        WriteTextFile(dllDir + L".interposer\\Config.yml",
            config(fromDll, patchFirst ? "from_patch" : "", "dll"));

        // Trailing separators come off for the command line; see RunIsolatedTests.
        std::wstring command = L"\"" + exePath + L"\" --embedded " + mode + L" \"" +
            patchDir.substr(0, patchDir.size() - 1) + L"\" \"" +
            dllDir.substr(0, dllDir.size() - 1) + L"\"";

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (!CreateProcessW(exePath.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
        {
            ASSERT(false, L"EM-00: launching the embedded child process");
            return;
        }

        WaitForSingleObject(pi.hProcess, 120 * 1000);

        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (patchFirst)
            ASSERT(exitCode == 0, L"EM-00a: the patch-first child reported no failures");
        else
            ASSERT(exitCode == 0, L"EM-00b: the dll-first child reported no failures");

        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
}

// ============================================================
// Network relay tests  (two peer processes + a relay server)
// ============================================================
// The relay needs a server and two members to be worth testing, so the parent
// stages two child processes, each with its own copy of the DLL and its own
// .interposer tree -- the same arrangement as the isolated-mode tests -- and
// points both at one relay server: the proof-of-concept host in
// LANCommander.Interposer.Relay.Server. INTERPOSER_RELAY_SERVER_EXE names its
// executable (or its .dll, run through dotnet), started here on a free port;
// INTERPOSER_RELAY_SERVER names a host:port that is already running one. With
// neither set the tests are skipped, not failed.
//
// Both peers run on this one machine, which a real LAN never does: they cannot
// share a port, so each uses its own, and LoopbackBroadcasts is off -- a
// broadcast one process looped back to itself would also land on the other's
// socket, from a portal the other knows nothing about.

static const char*    kRelayAddressA = "10.13.37.10";
static const char*    kRelayAddressB = "10.13.37.20";
static const u_short  kRelayUdpPortA = 27960;
static const u_short  kRelayUdpPortB = 27961;
static const u_short  kRelayTcpPortA = 27970;
static const u_short  kRelayIpxSocketA = 0x5000;
static const u_short  kRelayIpxSocketB = 0x5001;

// Mirrors InterposerRelayStatus in dll\src\relay.h.
struct TestRelayStatus
{
    DWORD            size;
    int              state;
    DWORD            clientId;
    wchar_t          virtualAddress[16];
    wchar_t          netmask[16];
    wchar_t          network[128];
    wchar_t          server[256];
    int              udpActive;
    DWORD            peerCount;
    unsigned __int64 bytesSent;
    unsigned __int64 bytesReceived;
    unsigned __int64 datagramsSent;
    unsigned __int64 datagramsReceived;
    DWORD            streamsOpen;
    wchar_t          lastError[256];
};

static HANDLE RelayTestEvent(const std::wstring& tag, const wchar_t* name)
{
    return CreateEventW(nullptr, TRUE, FALSE, (L"Local\\InterposerRelayTest_" + tag + L"_" + name).c_str());
}

static bool WaitReadable(SOCKET s, int ms)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{ ms / 1000, (ms % 1000) * 1000 };
    return select(0, &rd, nullptr, nullptr, &tv) == 1;
}

static std::string AddressText(const in_addr& a)
{
    char buf[16]{};
    inet_ntop(AF_INET, const_cast<in_addr*>(&a), buf, sizeof(buf));
    return buf;
}

static sockaddr_in RelayEndpoint(const char* ip, u_short port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

// The IPX node the DLL derives from a virtual address: 02:00 then the address.
static bool NodeIs(const char node[6], const char* ip)
{
    in_addr a{};
    inet_pton(AF_INET, ip, &a);
    const auto* b = reinterpret_cast<const unsigned char*>(&a);
    const auto* n = reinterpret_cast<const unsigned char*>(node);
    return n[0] == 0x02 && n[1] == 0x00 && memcmp(n + 2, b, 4) == 0;
}

// Runs in a child process. Returns the number of failed assertions.
static int RunRelayChild(const wchar_t* dirArg, const wchar_t* roleArg, const wchar_t* tagArg)
{
    std::wstring dir(dirArg);
    if (!dir.empty() && dir.back() != L'\\')
        dir += L'\\';

    const bool        isA   = roleArg[0] == L'A';
    const std::wstring tag  = tagArg;
    const char*       own   = isA ? kRelayAddressA : kRelayAddressB;

    HANDLE myReady    = RelayTestEvent(tag, isA ? L"AReady" : L"BReady");
    HANDLE otherReady = RelayTestEvent(tag, isA ? L"BReady" : L"AReady");
    HANDLE myDone     = RelayTestEvent(tag, isA ? L"ADone"  : L"BDone");
    HANDLE otherDone  = RelayTestEvent(tag, isA ? L"BDone"  : L"ADone");

    // The one-shot signals between the two peers. Opened here, before either
    // peer signals "ready", and held for the child's whole life: a named event
    // dies with its last handle, so a peer that opened, set and closed one
    // before the other had opened it would lose the signal outright -- and the
    // waiter would sit out its full timeout and knock every later step out of
    // step.
    HANDLE aPosted = RelayTestEvent(tag, L"APosted");
    HANDLE bIocp   = RelayTestEvent(tag, L"BIocp");

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    wprintf(L"\n--- Network Relay Tests (peer %ls) ---\n", roleArg);

    HMODULE hDll = LoadLibraryW((dir + L"LANCommander.Interposer.dll").c_str());
    if (!hDll)
    {
        wprintf(L"  FAIL  N-00: peer could not load its DLL (GLE=%lu)\n", GetLastError());
        return 1;
    }

    // N-01: our own name resolves to the requested virtual address. The hook
    // waits for the relay's Welcome, so this is also the "connected" check.
    {
        char host[256]{};
        gethostname(host, sizeof(host));
        hostent* he = gethostbyname(host);

        std::string first = (he && he->h_addr_list && he->h_addr_list[0])
            ? AddressText(*reinterpret_cast<in_addr*>(he->h_addr_list[0]))
            : std::string();

        ASSERT(first == own, L"N-01: gethostbyname of our own name returns the virtual address");
    }

    // N-02: the status export reports Connected, with the other peer present.
    {
        using FnStatus = BOOL (*)(TestRelayStatus*);
        auto getStatus = reinterpret_cast<FnStatus>(GetProcAddress(hDll, "InterposerRelayGetStatus"));

        bool peered = false;
        const DWORD start = GetTickCount();

        while (getStatus && GetTickCount() - start < 20000)
        {
            TestRelayStatus st{};
            st.size = sizeof(st);
            if (getStatus(&st) && st.state == 2 && st.peerCount >= 1)
            {
                peered = true;
                break;
            }
            Sleep(100);
        }

        ASSERT(peered, L"N-02: InterposerRelayGetStatus reports Connected with the other peer present");

        // N-21: datagrams start on the TCP connection and move to UDP once the
        // server has answered a UdpHello -- which, over loopback, it always does.
        bool udp = false;
        const DWORD udpStart = GetTickCount();

        while (getStatus && GetTickCount() - udpStart < 10000)
        {
            TestRelayStatus st{};
            st.size = sizeof(st);
            if (getStatus(&st) && st.udpActive)
            {
                udp = true;
                break;
            }
            Sleep(100);
        }

        ASSERT(udp, L"N-21: datagrams move from the TCP fallback to UDP");
    }

    const char* other     = isA ? kRelayAddressB : kRelayAddressA;
    const char* otherName = isA ? "Peer B" : "Peer A";

    // N-23: the async lookup of our own name, answered by a posted message.
    {
        HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        const UINT done = WM_APP + 23;

        char host[256]{};
        gethostname(host, sizeof(host));

        alignas(8) char buf[MAXGETHOSTSTRUCT]{};
        HANDLE request = WSAAsyncGetHostByName(window, done, host, buf, sizeof(buf));

        bool answered = false;
        LPARAM reply  = 0;
        const DWORD start = GetTickCount();

        while (request && !answered && GetTickCount() - start < 10000)
        {
            MSG msg;
            while (PeekMessageW(&msg, window, 0, 0, PM_REMOVE))
            {
                if (msg.message == done && reinterpret_cast<HANDLE>(msg.wParam) == request)
                {
                    answered = true;
                    reply    = msg.lParam;
                }
            }
            if (!answered)
                Sleep(20);
        }

        auto* he = reinterpret_cast<hostent*>(buf);
        ASSERT(answered && WSAGETASYNCERROR(reply) == 0 && he->h_addr_list && he->h_addr_list[0] &&
               AddressText(*reinterpret_cast<in_addr*>(he->h_addr_list[0])) == own,
            L"N-23: WSAAsyncGetHostByName of our own name answers with the virtual address");

        DestroyWindow(window);
    }

    // N-24/N-25: the IP address table leads with the virtual address, under an
    // interface index GetIfEntry knows.
    {
        ULONG len = 0;
        GetIpAddrTable(nullptr, &len, FALSE);
        std::vector<BYTE> buf(len ? len : 1);
        auto* table = reinterpret_cast<MIB_IPADDRTABLE*>(buf.data());

        const bool ok = len && GetIpAddrTable(table, &len, FALSE) == NO_ERROR && table->dwNumEntries > 0;
        in_addr first{};
        if (ok) first.s_addr = table->table[0].dwAddr;

        ASSERT(ok && AddressText(first) == own, L"N-24: GetIpAddrTable lists the virtual address first");

        MIB_IFROW row{};
        row.dwIndex = ok ? table->table[0].dwIndex : 0;
        ASSERT(ok && GetIfEntry(&row) == NO_ERROR && row.dwPhysAddrLen == 6,
            L"N-25: GetIfEntry knows the virtual address's interface index");

        // N-31: and a route to a peer goes out through that interface.
        in_addr peer{};
        inet_pton(AF_INET, other, &peer);
        DWORD bestIndex = 0;
        MIB_IPFORWARDROW route{};
        ASSERT(ok && GetBestInterface(peer.s_addr, &bestIndex) == NO_ERROR &&
               bestIndex == table->table[0].dwIndex &&
               GetBestRoute(peer.s_addr, 0, &route) == NO_ERROR && route.dwForwardIfIndex == bestIndex,
            L"N-31: GetBestInterface/GetBestRoute send a peer's address through the virtual interface");
    }

    // N-26/N-27: reverse lookups of a peer answer with its player name instead
    // of asking real DNS about a 10.13.37.x address.
    {
        in_addr a{};
        inet_pton(AF_INET, other, &a);
        hostent* he = gethostbyaddr(reinterpret_cast<const char*>(&a), 4, AF_INET);
        ASSERT(he && he->h_name && strcmp(he->h_name, otherName) == 0,
            L"N-26: gethostbyaddr of a peer's virtual address returns its player name");

        sockaddr_in sa = RelayEndpoint(other, 1234);
        char host[256]{}, serv[32]{};
        const int r = getnameinfo(reinterpret_cast<sockaddr*>(&sa), sizeof(sa), host, sizeof(host),
            serv, sizeof(serv), NI_NUMERICSERV);
        ASSERT(r == 0 && strcmp(host, otherName) == 0 && strcmp(serv, "1234") == 0,
            L"N-27: getnameinfo of a peer's virtual address returns its name and the service");
    }

    // N-28: a ping to a peer is answered, though ICMP is not relayed.
    {
        HANDLE icmp = IcmpCreateFile();
        in_addr a{};
        inet_pton(AF_INET, other, &a);

        char request[] = "ping";
        BYTE reply[sizeof(ICMP_ECHO_REPLY) + sizeof(request) + 8 + 64]{};
        const DWORD n = IcmpSendEcho(icmp, a.s_addr, request, sizeof(request), nullptr, reply, sizeof(reply), 2000);
        const auto* r = reinterpret_cast<ICMP_ECHO_REPLY*>(reply);

        ASSERT(n == 1 && r->Status == IP_SUCCESS && r->Address == a.s_addr && r->DataSize == sizeof(request),
            L"N-28: IcmpSendEcho to a peer's virtual address gets a reply");
        IcmpCloseHandle(icmp);
    }

    // N-29: GetAddrInfoExW of our own name, like getaddrinfo.
    {
        wchar_t host[256]{};
        DWORD hostLen = ARRAYSIZE(host);
        GetComputerNameExW(ComputerNameDnsHostname, host, &hostLen);

        ADDRINFOEXW hints{};
        hints.ai_family = AF_INET;
        ADDRINFOEXW* result = nullptr;

        const bool ok = GetAddrInfoExW(host, nullptr, NS_ALL, nullptr, &hints, &result,
                                       nullptr, nullptr, nullptr, nullptr) == 0 && result;
        ASSERT(ok && AddressText(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr) == own,
            L"N-29: GetAddrInfoExW of our own name returns the virtual address");
        if (result) FreeAddrInfoExW(result);
    }

    // N-03..N-05: local-address discovery leads with the virtual adapter.
    {
        ULONG len = 0;
        GetAdaptersInfo(nullptr, &len);
        std::vector<BYTE> buf(len ? len : 1);
        auto* info = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());

        ASSERT(len && GetAdaptersInfo(info, &len) == ERROR_SUCCESS &&
               strcmp(info->IpAddressList.IpAddress.String, own) == 0,
            L"N-03: GetAdaptersInfo lists the virtual adapter first");
    }

    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        INTERFACE_INFO list[32]{};
        DWORD bytes = 0;

        const bool ok = WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, list, sizeof(list),
                                 &bytes, nullptr, nullptr) == 0 && bytes >= sizeof(INTERFACE_INFO);

        ASSERT(ok && AddressText(list[0].iiAddress.AddressIn.sin_addr) == own,
            L"N-04: SIO_GET_INTERFACE_LIST lists the virtual address first");
        closesocket(s);
    }

    {
        char host[256]{};
        gethostname(host, sizeof(host));

        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* result = nullptr;

        const bool ok = getaddrinfo(host, nullptr, &hints, &result) == 0 && result;
        ASSERT(ok && AddressText(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr) == own,
            L"N-05: getaddrinfo of our own name returns the virtual address");
        if (result) freeaddrinfo(result);
    }

    // N-06/N-07: IPX exists, as far as the game can tell.
    {
        DWORD length = 0;
        WSAEnumProtocolsW(nullptr, nullptr, &length);
        std::vector<BYTE> buf(length ? length : 1);
        auto* protocols = reinterpret_cast<WSAPROTOCOL_INFOW*>(buf.data());
        const int count = WSAEnumProtocolsW(nullptr, protocols, &length);

        bool found = false;
        for (int i = 0; i < count; ++i)
            if (protocols[i].iAddressFamily == AF_IPX && protocols[i].iProtocol == NSPROTO_IPX)
                found = true;

        ASSERT(found, L"N-06: WSAEnumProtocols lists an IPX protocol");
    }

    SOCKET ipx = socket(AF_IPX, SOCK_DGRAM, NSPROTO_IPX);
    ASSERT(ipx != INVALID_SOCKET, L"N-07: socket(AF_IPX, SOCK_DGRAM, NSPROTO_IPX) succeeds");

    {
        SOCKADDR_IPX local{};
        local.sa_family = AF_IPX;
        local.sa_socket = htons(isA ? kRelayIpxSocketA : kRelayIpxSocketB);
        bind(ipx, reinterpret_cast<sockaddr*>(&local), sizeof(local));

        SOCKADDR_IPX name{};
        int nameLen = sizeof(name);
        const bool ok = getsockname(ipx, reinterpret_cast<sockaddr*>(&name), &nameLen) == 0;

        ASSERT(ok && name.sa_family == AF_IPX && NodeIs(name.sa_nodenum, own) &&
               ntohs(name.sa_socket) == (isA ? kRelayIpxSocketA : kRelayIpxSocketB),
            L"N-08: an IPX socket is named by the virtual node and its bound socket number");
    }

    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    {
        BOOL on = TRUE;
        setsockopt(udp, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_port   = htons(isA ? kRelayUdpPortA : kRelayUdpPortB);
        bind(udp, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    }

    if (isA)
    {
        // N-09: binding to the virtual address works, though no interface has it.
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in at = RelayEndpoint(own, kRelayTcpPortA);
        const bool bound = bind(listener, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0 &&
                           listen(listener, 4) == 0;
        ASSERT(bound, L"N-09: bind() to the virtual address succeeds");

        {
            sockaddr_in name{};
            int nameLen = sizeof(name);
            getsockname(listener, reinterpret_cast<sockaddr*>(&name), &nameLen);
            ASSERT(AddressText(name.sin_addr) == own,
                L"N-10: getsockname() reports the virtual address it was bound to");
        }

        SetEvent(myReady);
        WaitForSingleObject(otherReady, 30000);

        // N-11: B's broadcast arrives, from B's virtual endpoint -- received
        // here with an overlapped WSARecvFrom that is still pending when it
        // returns, so the address is only filled in at completion, which
        // WSAGetOverlappedResult reports. (B's receives cover plain recvfrom.)
        {
            char buf[64]{};
            sockaddr_in from{};
            int fromLen = sizeof(from);

            WSABUF wsaBuf{ sizeof(buf) - 1, buf };
            WSAOVERLAPPED ov{};
            ov.hEvent = WSACreateEvent();
            DWORD received = 0, flags = 0;

            int r = WSARecvFrom(udp, &wsaBuf, 1, &received, &flags,
                reinterpret_cast<sockaddr*>(&from), &fromLen, &ov, nullptr);
            const bool pending = r != 0 && WSAGetLastError() == WSA_IO_PENDING;

            // B broadcasts only once this receive is posted, so it is pending
            // when the datagram lands rather than completing on the spot.
            SetEvent(aPosted);

            int n = -1;
            if (pending || r == 0)
            {
                if (WaitForSingleObject(ov.hEvent, 15000) == WAIT_OBJECT_0 &&
                    WSAGetOverlappedResult(udp, &ov, &received, FALSE, &flags))
                    n = static_cast<int>(received);
            }
            WSACloseEvent(ov.hEvent);

            ASSERT(pending && n == 5 && strcmp(buf, "HELLO") == 0 &&
                   AddressText(from.sin_addr) == kRelayAddressB && ntohs(from.sin_port) == kRelayUdpPortB,
                L"N-11: a broadcast completed by WSAGetOverlappedResult arrives from the peer's virtual endpoint");

            if (n > 0)
                sendto(udp, "REPLY", 5, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
        }

        // N-30, A's half: once B has an IOCP receive pending, send to it.
        if (WaitForSingleObject(bIocp, 15000) == WAIT_OBJECT_0)
        {
            sockaddr_in to = RelayEndpoint(kRelayAddressB, kRelayUdpPortB);
            sendto(udp, "IOCP", 4, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }

        // N-12..N-14: B's TCP connection arrives through the relay.
        {
            sockaddr_in peer{};
            int peerLen = sizeof(peer);
            SOCKET c = WaitReadable(listener, 15000)
                ? accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLen)
                : INVALID_SOCKET;

            ASSERT(c != INVALID_SOCKET && AddressText(peer.sin_addr) == kRelayAddressB,
                L"N-12: accept() reports the other peer's virtual address");

            char buf[64]{};
            int n = (c != INVALID_SOCKET && WaitReadable(c, 15000)) ? recv(c, buf, sizeof(buf) - 1, 0) : -1;
            ASSERT(n == 4 && strcmp(buf, "PING") == 0, L"N-13: data the other peer sent over TCP arrives");

            if (c != INVALID_SOCKET)
            {
                send(c, "PONG", 4, 0);

                n = WaitReadable(c, 15000) ? recv(c, buf, sizeof(buf), 0) : -1;
                ASSERT(n == 0, L"N-14: the other peer closing its end reads as end-of-stream");
                closesocket(c);
            }
        }

        // N-15: B's IPX broadcast arrives from B's node and socket.
        {
            char buf[64]{};
            SOCKADDR_IPX from{};
            int fromLen = sizeof(from);
            int n = WaitReadable(ipx, 15000)
                ? recvfrom(ipx, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen)
                : -1;

            ASSERT(n == 8 && strcmp(buf, "IPXHELLO") == 0 && from.sa_family == AF_IPX &&
                   NodeIs(from.sa_nodenum, kRelayAddressB) && ntohs(from.sa_socket) == kRelayIpxSocketB,
                L"N-15: an IPX broadcast from the other peer arrives from its node and socket");

            if (n > 0)
                sendto(ipx, "IPXREPLY", 8, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
        }

        closesocket(listener);
    }
    else
    {
        SetEvent(myReady);
        WaitForSingleObject(otherReady, 30000);

        // N-16: a broadcast reaches A, and A's unicast answer comes back from A.
        {
            sockaddr_in broadcast = RelayEndpoint("255.255.255.255", kRelayUdpPortA);
            char buf[64]{};
            sockaddr_in from{};
            int fromLen = sizeof(from);
            int n = -1;

            WaitForSingleObject(aPosted, 15000);

            for (int attempt = 0; attempt < 10 && n < 0; ++attempt)
            {
                sendto(udp, "HELLO", 5, 0, reinterpret_cast<sockaddr*>(&broadcast), sizeof(broadcast));
                if (WaitReadable(udp, 1500))
                    n = recvfrom(udp, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            }

            ASSERT(n == 5 && strcmp(buf, "REPLY") == 0 &&
                   AddressText(from.sin_addr) == kRelayAddressA && ntohs(from.sin_port) == kRelayUdpPortA,
                L"N-16: the reply to a broadcast comes back from the other peer's virtual endpoint");
        }

        // N-30: a receive completed through an I/O completion port.
        {
            HANDLE port = CreateIoCompletionPort(reinterpret_cast<HANDLE>(udp), nullptr, 1, 0);

            char buf[64]{};
            sockaddr_in from{};
            int fromLen = sizeof(from);
            WSABUF wsaBuf{ sizeof(buf) - 1, buf };
            WSAOVERLAPPED ov{};
            DWORD received = 0, flags = 0;

            int r = WSARecvFrom(udp, &wsaBuf, 1, &received, &flags,
                reinterpret_cast<sockaddr*>(&from), &fromLen, &ov, nullptr);

            // Read before anything else can overwrite it.
            const int posted = (r == 0 || WSAGetLastError() == WSA_IO_PENDING) ? 1 : WSAGetLastError();

            SetEvent(bIocp);

            DWORD bytes = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED done = nullptr;
            const BOOL dequeued = port && posted == 1 && GetQueuedCompletionStatus(port, &bytes, &key, &done, 15000);
            const DWORD dequeueError = GetLastError();
            const bool ok = dequeued && done == &ov;

            ASSERT(ok && bytes == 4 && strcmp(buf, "IOCP") == 0 &&
                   AddressText(from.sin_addr) == kRelayAddressA && ntohs(from.sin_port) == kRelayUdpPortA,
                L"N-30: a receive completed through an I/O completion port reports the peer's virtual endpoint");

            if (!(ok && bytes == 4))
                wprintf(L"        posted=%d dequeued=%d err=%lu done=%p ov=%p bytes=%lu buf=%hs from=%hs:%u\n",
                    posted, dequeued, dequeueError, done, &ov, bytes, buf,
                    AddressText(from.sin_addr).c_str(), ntohs(from.sin_port));

            // The port stays: closing it would not detach the socket anyway.
        }

        // N-17..N-19: a TCP connection to A's virtual address.
        {
            SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in to = RelayEndpoint(kRelayAddressA, kRelayTcpPortA);

            const bool connected = connect(c, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0;
            ASSERT(connected, L"N-17: connect() to the other peer's virtual address succeeds");

            sockaddr_in peer{};
            int peerLen = sizeof(peer);
            getpeername(c, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            ASSERT(AddressText(peer.sin_addr) == kRelayAddressA && ntohs(peer.sin_port) == kRelayTcpPortA,
                L"N-18: getpeername() reports the virtual address that was connected to");

            send(c, "PING", 4, 0);

            char buf[64]{};
            int n = WaitReadable(c, 15000) ? recv(c, buf, sizeof(buf) - 1, 0) : -1;
            ASSERT(n == 4 && strcmp(buf, "PONG") == 0, L"N-19: the other peer's TCP answer arrives");

            closesocket(c);
        }

        // N-20: an IPX broadcast reaches A, and A's answer comes back from A's node.
        {
            int type = 4; // IPX packet type: PEP, what most games used
            setsockopt(ipx, NSPROTO_IPX, IPX_PTYPE, reinterpret_cast<const char*>(&type), sizeof(type));

            SOCKADDR_IPX broadcast{};
            broadcast.sa_family = AF_IPX;
            memset(broadcast.sa_nodenum, 0xFF, sizeof(broadcast.sa_nodenum));
            broadcast.sa_socket = htons(kRelayIpxSocketA);

            char buf[64]{};
            SOCKADDR_IPX from{};
            int fromLen = sizeof(from);
            int n = -1;

            for (int attempt = 0; attempt < 10 && n < 0; ++attempt)
            {
                sendto(ipx, "IPXHELLO", 8, 0, reinterpret_cast<sockaddr*>(&broadcast), sizeof(broadcast));
                if (WaitReadable(ipx, 1500))
                    n = recvfrom(ipx, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            }

            ASSERT(n == 8 && strcmp(buf, "IPXREPLY") == 0 &&
                   NodeIs(from.sa_nodenum, kRelayAddressA) && ntohs(from.sa_socket) == kRelayIpxSocketA,
                L"N-20: the IPX reply comes back from the other peer's node and socket");
        }
    }

    // Neither peer leaves until both are finished: leaving early would take
    // the other's peer and streams down with it.
    SetEvent(myDone);
    WaitForSingleObject(otherDone, 30000);

    closesocket(udp);
    closesocket(ipx);

    // N-22: unloading with the relay and control threads running. Their
    // shutdown parks them and terminates them from DLL_PROCESS_DETACH, where
    // waiting for a thread to exit would deadlock on the loader lock; getting
    // past this line at all, and back out of the process cleanly, is the test.
    const DWORD unloadStart = GetTickCount();
    FreeLibrary(hDll);
    ASSERT(GetTickCount() - unloadStart < 5000,
        L"N-22: the DLL unloads promptly with the relay thread running");

    return g_fail;
}

// The relay server's console output, kept for when something fails.
static std::string g_relayServerOutput;
static std::mutex  g_relayServerOutputMutex;

static DWORD WINAPI DrainRelayServerOutput(LPVOID pipe)
{
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(static_cast<HANDLE>(pipe), buf, sizeof(buf), &n, nullptr) && n)
    {
        std::lock_guard<std::mutex> lk(g_relayServerOutputMutex);
        g_relayServerOutput.append(buf, n);
    }
    return 0;
}

// Start the proof-of-concept server on a free port. Returns its "host:port", or
// empty on failure; *process receives its handle.
static std::wstring StartRelayServer(const std::wstring& exe, HANDLE* process)
{
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0))
        return {};
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    const bool viaDotnet = exe.size() > 4 && _wcsicmp(exe.c_str() + exe.size() - 4, L".dll") == 0;
    std::wstring command = (viaDotnet ? L"dotnet \"" : L"\"") + exe + L"\" --port 0 --status 0";

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError  = writeEnd;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);

    if (!started)
    {
        CloseHandle(readEnd);
        return {};
    }

    CloseHandle(pi.hThread);
    *process = pi.hProcess;

    // Read until the LISTENING line; everything after it is drained in the
    // background so the server never blocks on a full pipe.
    std::string seen;
    const DWORD start = GetTickCount();

    while (GetTickCount() - start < 30000)
    {
        char c = 0;
        DWORD n = 0;
        if (!ReadFile(readEnd, &c, 1, &n, nullptr) || n == 0)
            break;
        seen.push_back(c);

        const size_t at = seen.find("LISTENING ");
        if (at != std::string::npos && c == '\n')
        {
            const int port = atoi(seen.c_str() + at + 10);
            g_relayServerOutput = seen;
            CloseHandle(CreateThread(nullptr, 0, DrainRelayServerOutput, readEnd, 0, nullptr));
            return port > 0 ? L"127.0.0.1:" + std::to_wstring(port) : std::wstring();
        }
    }

    g_relayServerOutput = seen;
    CloseHandle(readEnd);
    return {};
}

static std::wstring EnvironmentValue(const wchar_t* name)
{
    wchar_t buf[1024]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return (n && n < ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring();
}

static void WriteRelayFixtures(const std::wstring& interposerDir, const std::wstring& server,
                               const std::wstring& network, const char* playerName, const char* address)
{
    auto utf8 = [](const std::wstring& w)
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(n > 1 ? static_cast<size_t>(n - 1) : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
        return out;
    };

    const std::string serverUtf8  = utf8(server);
    const std::string networkUtf8 = utf8(network);

    // The name peers see is the username, so N-26/N-27 read Player.Username
    // back through the other peer's reverse lookups.
    WriteTextFile(interposerDir + L"Config.yml",
        std::string("Player:\n"
        "  Username: '") + playerName + "'\n"
        "\n"
        "Logging:\n"
        "  Relay: true\n"
        "  Network: true\n"
        "  Level: Debug\n"
        "\n"
        "NetworkRelay:\n"
        "  Enabled: true\n"
        "  Server: '" + serverUtf8 + "'\n"
        "  Network: '" + networkUtf8 + "'\n"
        "  Address: '" + address + "'\n"
        "  LoopbackBroadcasts: false\n"
        "  AddressTimeout: 15000\n");
}

// Runs in the parent: starts (or finds) the relay server, runs both peers, and
// folds their outcome into this process's tally.
static void RunRelayTests(const std::wstring& exeDir, const std::wstring& testTmpDir)
{
    std::wstring server    = EnvironmentValue(L"INTERPOSER_RELAY_SERVER");
    std::wstring serverExe = EnvironmentValue(L"INTERPOSER_RELAY_SERVER_EXE");

    wprintf(L"\n--- Network Relay Tests ---\n");

    if (server.empty() && serverExe.empty())
    {
        wprintf(L"  SKIP  N-00: set INTERPOSER_RELAY_SERVER_EXE (or INTERPOSER_RELAY_SERVER) to run them\n");
        return;
    }

    HANDLE serverProcess = nullptr;

    if (server.empty())
    {
        server = StartRelayServer(serverExe, &serverProcess);
        ASSERT(!server.empty(), L"N-00: the relay server started");
        if (server.empty())
        {
            wprintf(L"%hs\n", g_relayServerOutput.c_str());
            if (serverProcess) { TerminateProcess(serverProcess, 1); CloseHandle(serverProcess); }
            return;
        }
    }

    const std::wstring tag     = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount());
    const std::wstring network = L"relay-test-" + tag;

    struct Peer { const wchar_t* role; const char* name; const char* address; std::wstring dir; HANDLE process; };
    Peer peers[2] =
    {
        { L"A", "Peer A", kRelayAddressA, testTmpDir + L"RelayA\\", nullptr },
        { L"B", "Peer B", kRelayAddressB, testTmpDir + L"RelayB\\", nullptr },
    };

    const std::wstring exePath = exeDir + L"LANCommander.Interposer.Tests.exe";

    for (Peer& p : peers)
    {
        CreateDirs(p.dir + L".interposer\\");
        CopyFileW((exeDir + L"LANCommander.Interposer.dll").c_str(),
                  (p.dir + L"LANCommander.Interposer.dll").c_str(), FALSE);
        WriteRelayFixtures(p.dir + L".interposer\\", server, network, p.name, p.address);

        std::wstring command = L"\"" + exePath + L"\" --relay-peer \"" +
            p.dir.substr(0, p.dir.size() - 1) + L"\" " + p.role + L" " + tag;

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (CreateProcessW(exePath.c_str(), command.data(), nullptr, nullptr, TRUE, 0,
                nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            p.process = pi.hProcess;
        }
    }

    DWORD exitA = 1, exitB = 1;

    for (Peer& p : peers)
    {
        if (!p.process)
            continue;

        if (WaitForSingleObject(p.process, 120 * 1000) != WAIT_OBJECT_0)
            TerminateProcess(p.process, 1);

        GetExitCodeProcess(p.process, p.role[0] == L'A' ? &exitA : &exitB);
        CloseHandle(p.process);
    }

    ASSERT(exitA == 0, L"N-00a: relay peer A reported no failures");
    ASSERT(exitB == 0, L"N-00b: relay peer B reported no failures");

    if (exitA != 0 || exitB != 0)
    {
        std::lock_guard<std::mutex> lk(g_relayServerOutputMutex);
        wprintf(L"\n  Relay server output:\n%hs\n", g_relayServerOutput.c_str());
    }

    if (serverProcess)
    {
        TerminateProcess(serverProcess, 0);
        CloseHandle(serverProcess);
    }

    // Kept on failure: the peers' session logs are what explains it.
    if (exitA != 0 || exitB != 0)
    {
        for (Peer& p : peers)
            wprintf(L"  Peer %ls log: %ls.interposer\\Logs\\\n", p.role, p.dir.c_str());
        return;
    }

    for (Peer& p : peers)
    {
        DeleteFileW((p.dir + L".interposer\\Config.yml").c_str());
        DeleteFileW((p.dir + L"LANCommander.Interposer.dll").c_str());
        ClearLogFiles(p.dir + L".interposer\\Logs\\");
        RemoveDirectoryW((p.dir + L".interposer\\Logs").c_str());
        RemoveDirectoryW((p.dir + L".interposer").c_str());
        RemoveDirectoryW(p.dir.c_str());
    }
}

// ============================================================
// Entry point
// ============================================================

// ============================================================
// Match statistics tests  (MS-)
// ============================================================
// In process, the default fixture has no MatchStatistics section and no
// launcher pipe, so every match goes to the callback alone: MS-01..09 check
// what the callback sees and what the API refuses.
//
// The HTTP sink needs a server and a process of its own (the DLL reads its
// config once, at load), so MS-10 and up re-run the EXE as
// --match-http <dir> <phase> against a DLL copy whose Config.yml points at a
// fake LANCommander server this process runs on loopback. The fake records
// every request and answers like /api/Matches does; the parent then checks
// what arrived. Three phases share one fake:
//
//   basic    a whole match: register, participants, events (the first batch
//            is answered 429), end with results and statistics, an attachment
//   crash    events the server will not take (503), then the process is
//            terminated: they must be on disk in .interposer\Matches
//   resume   a new process with the server back up: the spool is resent from
//            after the server's last sequence, the match carries on, and the
//            spool is removed once everything is in

#include <atomic>
#include <functional>
#include <set>
#include <thread>
#include <tuple>
#include <type_traits>

static std::vector<std::tuple<std::wstring, DWORD, std::wstring>> g_matchEvents;

static void __cdecl OnMatchEvent(const wchar_t* kind, DWORD match, const wchar_t* json)
{
    g_matchEvents.emplace_back(kind ? kind : L"", match, json ? json : L"<null>");
}

static bool Contains(const std::wstring& haystack, const wchar_t* needle)
{
    return haystack.find(needle) != std::wstring::npos;
}

static bool Contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

struct MatchApi
{
    DWORD (__cdecl* start)(const InterposerMatchInfo*) = nullptr;
    BOOL  (__cdecl* joined)(DWORD, const char*, const char*, const char*, long long) = nullptr;
    BOOL  (__cdecl* left)(DWORD, const char*, long long) = nullptr;
    BOOL  (__cdecl* event)(DWORD, const char*, long long, long long, const char*, const char*, const char*) = nullptr;
    BOOL  (__cdecl* setStat)(DWORD, const char*, const char*, const char*, double) = nullptr;
    BOOL  (__cdecl* addStat)(DWORD, const char*, const char*, const char*, double) = nullptr;
    BOOL  (__cdecl* setResult)(DWORD, const InterposerMatchResult*) = nullptr;
    BOOL  (__cdecl* end)(DWORD, const char*, BOOL) = nullptr;
    BOOL  (__cdecl* attach)(DWORD, const wchar_t*, int, long long, long long) = nullptr;
    BOOL  (__cdecl* flush)(DWORD, DWORD) = nullptr;
    BOOL  (__cdecl* status)(DWORD, InterposerMatchStatus*) = nullptr;
    void  (__cdecl* registerCb)(InterposerMatchCallback) = nullptr;
    BOOL  (__cdecl* configure)(const InterposerMatchConfig*) = nullptr;

    bool Load(HMODULE dll)
    {
        auto get = [dll](auto& fn, const char* name)
        {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(
                reinterpret_cast<void*>(GetProcAddress(dll, name)));
            return fn != nullptr;
        };

        bool ok = true;
        ok &= get(configure, "InterposerMatchConfigure");
        ok &= get(start, "InterposerMatchStart");
        ok &= get(joined, "InterposerMatchPlayerJoined");
        ok &= get(left, "InterposerMatchPlayerLeft");
        ok &= get(event, "InterposerMatchEvent");
        ok &= get(setStat, "InterposerMatchSetStatistic");
        ok &= get(addStat, "InterposerMatchAddStatistic");
        ok &= get(setResult, "InterposerMatchSetResult");
        ok &= get(end, "InterposerMatchEnd");
        ok &= get(attach, "InterposerMatchAttachFile");
        ok &= get(flush, "InterposerMatchFlush");
        ok &= get(status, "InterposerMatchGetStatus");
        ok &= get(registerCb, "InterposerRegisterMatchCallback");
        return ok;
    }

    InterposerMatchStatus Status(DWORD match) const
    {
        InterposerMatchStatus st{};
        st.size = sizeof(st);
        status(match, &st);
        return st;
    }
};

static void RunMatchTests(HMODULE hDll, const std::wstring& exeDir)
{
    wprintf(L"\n--- Match Statistics Tests ---\n");

    MatchApi api;

    ASSERT(api.Load(hDll), L"MS-01: every InterposerMatch* function is exported");

    if (!api.start)
        return;

    api.registerCb(OnMatchEvent);

    InterposerMatchInfo info{};
    info.structSize   = sizeof(info);
    info.externalId   = "round-7";
    info.map          = "de_dust2";
    info.mode         = "competitive";
    info.settingsJson = "{\"timelimit\":20,\"friendlyfire\":true}";

    const DWORD match = api.start(&info);

    ASSERT(match != 0 && g_matchEvents.size() == 1 && std::get<0>(g_matchEvents[0]) == L"START" &&
           std::get<1>(g_matchEvents[0]) == match,
        L"MS-02: InterposerMatchStart returns a handle and fires START");

    if (!match || g_matchEvents.empty())
        return;

    {
        const std::wstring& json = std::get<2>(g_matchEvents[0]);

        ASSERT(Contains(json, L"\"externalId\":\"round-7\"") && Contains(json, L"\"map\":\"de_dust2\"") &&
               Contains(json, L"\"settings\":{\"timelimit\":20,\"friendlyfire\":true}") &&
               Contains(json, L"\"forward\":false") && Contains(json, L"\"reporterKind\":\"Server\""),
            L"MS-03: START carries the match, its settings verbatim, and forward:false with no launcher");
    }

    ASSERT(api.start(&info) == match && g_matchEvents.size() == 1,
        L"MS-04: starting the same external id again carries on with the open match");

    api.joined(match, "steam:STEAM_0:1:1", "Pat \"the\" caf\xC3\xA9", "CT", 0);
    api.joined(match, "bot:3", "Bot", "T", 1500);

    const BOOL kill = api.event(match, "kill", 4200, 268, "steam:STEAM_0:1:1", "bot:3",
        "{\"weapon\":\"ak47\",\"headshot\":true}");
    api.event(match, "round_won", 90000, -1, nullptr, nullptr, nullptr);

    {
        std::wstring killJson;

        for (const auto& [kind, handle, json] : g_matchEvents)
            if (kind == L"EVENT" && Contains(json, L"\"type\":\"kill\""))
                killJson = json;

        ASSERT(kill && Contains(killJson, L"\"sequence\":1") && Contains(killJson, L"\"gameTimeMilliseconds\":4200") &&
               Contains(killJson, L"\"tick\":268") && Contains(killJson, L"\"actorKey\":\"steam:STEAM_0:1:1\"") &&
               Contains(killJson, L"\"subjectKey\":\"bot:3\"") &&
               Contains(killJson, L"\"payload\":{\"weapon\":\"ak47\",\"headshot\":true}") &&
               Contains(killJson, L"\"id\":\"") && Contains(killJson, L"\"clientTime\":\""),
            L"MS-05: EVENT carries an id, a sequence, the keys and the payload");

        size_t participants = 0;
        bool   escaped      = false;

        for (const auto& [kind, handle, json] : g_matchEvents)
        {
            if (kind != L"PARTICIPANT")
                continue;

            ++participants;
            escaped |= Contains(json, L"\"displayName\":\"Pat \\\"the\\\" caf\xE9\"");
        }

        ASSERT(participants == 2 && escaped, L"MS-06: PARTICIPANT fires per player, names escaped and decoded from UTF-8");
    }

    ASSERT(!api.event(match, "kill", 1, -1, nullptr, nullptr, "{'single':quotes}") &&
           !api.event(match, "kill", 1, -1, nullptr, nullptr, "{\"a\":1,}") &&
           !api.event(match, "kill", 1, -1, nullptr, nullptr, "[1,2]") &&
           !api.event(match, "", 1, -1, nullptr, nullptr, nullptr) &&
           !api.event(match + 1000, "kill", 1, -1, nullptr, nullptr, nullptr) &&
           !api.setStat(match, nullptr, nullptr, "kills", 1),
        L"MS-07: malformed JSON, a missing type, an unknown handle and an ownerless statistic are refused");

    api.addStat(match, "steam:STEAM_0:1:1", nullptr, "kills", 1);
    api.addStat(match, "steam:STEAM_0:1:1", nullptr, "kills", 2);
    api.setStat(match, nullptr, "CT", "rounds_won", 16);

    InterposerMatchResult result{};
    result.structSize = sizeof(result);
    result.playerKey  = "steam:STEAM_0:1:1";
    result.team       = "CT";
    result.rank       = 1;
    result.hasScore   = 1;
    result.score      = 42.5;
    result.timeMs     = -1;
    api.setResult(match, &result);

    const std::wstring demo = exeDir + L".interposer\\Config.yml";

    ASSERT(api.attach(match, demo.c_str(), INTERPOSER_MATCH_ATTACHMENT_LOG, -1, -1) &&
           !api.attach(match, (exeDir + L"no-such-demo.dem").c_str(), INTERPOSER_MATCH_ATTACHMENT_DEMO, -1, -1),
        L"MS-08a: attaching an existing file is accepted, a missing one refused");

    api.end(match, "completed", FALSE);

    {
        std::wstring endJson;

        for (const auto& [kind, handle, json] : g_matchEvents)
            if (kind == L"END")
                endJson = json;

        ASSERT(Contains(endJson, L"\"reason\":\"completed\"") && Contains(endJson, L"\"abandoned\":false") &&
               Contains(endJson, L"{\"playerKey\":\"steam:STEAM_0:1:1\",\"key\":\"kills\",\"value\":3}") &&
               Contains(endJson, L"{\"team\":\"CT\",\"key\":\"rounds_won\",\"value\":16}") &&
               Contains(endJson, L"\"rank\":1,\"score\":42.5,\"status\":\"Finished\""),
            L"MS-08: END carries the results and the summed statistics");
    }

    const InterposerMatchStatus st = api.Status(match);

    ASSERT(!api.event(match, "kill", 1, -1, nullptr, nullptr, nullptr) && !api.end(match, nullptr, FALSE) &&
           st.sink == INTERPOSER_MATCH_SINK_NONE && st.ended == 1 && api.flush(match, 0),
        L"MS-09: an ended match takes no more events, and with no sink there is nothing to flush");

    api.registerCb(nullptr);
}

// ── The fake LANCommander server ──────────────────────────────────────────────

struct FakeRequest
{
    std::string method, path, headers, body;
    int         status = 0;
};

struct FakeReply
{
    int         status = 200;
    std::string body;
    std::string extraHeaders;
};

class FakeMatchServer
{
public:
    std::function<FakeReply(const FakeRequest&)> handler;

    bool Start()
    {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);

        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (listener_ == INVALID_SOCKET || bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(listener_, 16) != 0)
            return false;

        int length = sizeof(addr);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &length);
        port_ = ntohs(addr.sin_port);

        thread_ = std::thread([this] { Run(); });
        return true;
    }

    void Stop()
    {
        stop_ = true;

        if (thread_.joinable())
            thread_.join();

        if (listener_ != INVALID_SOCKET)
            closesocket(listener_);

        listener_ = INVALID_SOCKET;
    }

    u_short Port() const { return port_; }

    static std::string Lower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::vector<FakeRequest> Requests()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.clear();
    }

private:
    SOCKET                   listener_ = INVALID_SOCKET;
    u_short                  port_     = 0;
    std::thread              thread_;
    std::atomic<bool>        stop_{ false };
    std::mutex               mutex_;
    std::vector<FakeRequest> requests_;

    void Run()
    {
        while (!stop_)
        {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener_, &set);
            timeval tv{ 0, 100 * 1000 };

            if (select(0, &set, nullptr, nullptr, &tv) <= 0)
                continue;

            SOCKET client = accept(listener_, nullptr, nullptr);

            if (client != INVALID_SOCKET)
                Serve(client);
        }
    }

    void Serve(SOCKET client)
    {
        DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        std::string data;
        char        chunk[65536];
        size_t      headerEnd = std::string::npos;

        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos)
        {
            const int n = recv(client, chunk, sizeof(chunk), 0);

            if (n <= 0)
            {
                closesocket(client);
                return;
            }

            data.append(chunk, n);
        }

        FakeRequest request;
        request.headers = data.substr(0, headerEnd);

        const size_t space1 = request.headers.find(' ');
        const size_t space2 = request.headers.find(' ', space1 + 1);
        request.method = request.headers.substr(0, space1);
        request.path   = request.headers.substr(space1 + 1, space2 - space1 - 1);

        size_t contentLength = 0;
        const std::string lower = Lower(request.headers);
        const size_t cl = lower.find("content-length:");

        if (cl != std::string::npos)
            contentLength = strtoul(lower.c_str() + cl + 15, nullptr, 10);

        request.body = data.substr(headerEnd + 4);

        while (request.body.size() < contentLength)
        {
            const int n = recv(client, chunk, sizeof(chunk), 0);

            if (n <= 0)
                break;

            request.body.append(chunk, n);
        }

        const FakeReply reply = handler ? handler(request) : FakeReply{ 404, "", "" };
        request.status = reply.status;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
        }

        std::string response = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\n" +
            "Content-Type: application/json\r\n" +
            "Content-Length: " + std::to_string(reply.body.size()) + "\r\n" +
            "Connection: close\r\n" + reply.extraHeaders + "\r\n" + reply.body;

        send(client, response.data(), static_cast<int>(response.size()), 0);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
};

static const char* kFakeMatchId    = "11111111-1111-1111-1111-111111111111";
static const char* kFakeReporterId = "22222222-2222-2222-2222-222222222222";
static const char* kFakeAttachment = "33333333-3333-3333-3333-333333333333";
static const char* kFakeGameId     = "44444444-4444-4444-4444-444444444444";

// Every "sequence":N in a body.
static std::vector<long long> SequencesIn(const std::string& body)
{
    std::vector<long long> out;
    size_t at = 0;

    while ((at = body.find("\"sequence\":", at)) != std::string::npos)
    {
        at += 11;
        out.push_back(strtoll(body.c_str() + at, nullptr, 10));
    }

    return out;
}

// What /api/Matches would answer. `phase` decides the failures; the highest
// sequence accepted is remembered across phases, which is what a reporter
// re-creating the match is told.
static FakeReply AnswerMatchRequest(const FakeRequest& r, const std::wstring& phase,
                                    int& eventCalls, long long& lastAccepted)
{
    const bool post = r.method == "POST";

    if (post && r.path == "/api/Matches")
        return { 201, std::string("{\"matchId\":\"") + kFakeMatchId + "\",\"reporterId\":\"" + kFakeReporterId +
            "\",\"created\":true,\"lastSequence\":" + std::to_string(lastAccepted) + ",\"status\":\"InProgress\"}", "" };

    if (phase == L"crash")
        return { 503, "", "" };

    if (post && Contains(r.path, "/Events"))
    {
        if (phase == L"basic" && eventCalls++ == 0)
            return { 429, "", "Retry-After: 1\r\n" };

        std::string rejected;

        for (long long sequence : SequencesIn(r.body))
            lastAccepted = std::max(lastAccepted, sequence);

        // basic: the first event of the batch is refused, as the server
        // refuses an event it finds invalid
        if (phase == L"basic")
        {
            const size_t id = r.body.find("\"id\":\"");
            rejected = "{\"id\":\"" + r.body.substr(id + 6, 36) + "\",\"code\":\"invalid\",\"detail\":\"test\"}";
        }

        return { 202, "{\"queued\":1,\"rejected\":[" + rejected + "]}", "" };
    }

    if (post && Contains(r.path, "/Participants"))
        return { 204, "", "" };

    if (post && Contains(r.path, "/End"))
        return { 204, "", "" };

    if (post && Contains(r.path, "/Complete"))
        return { 200, std::string("{\"id\":\"") + kFakeAttachment + "\",\"status\":\"Uploaded\"}", "" };

    if (post && Contains(r.path, "/Attachments"))
        return { 201, std::string("{\"id\":\"") + kFakeAttachment + "\"}", "" };

    if (r.method == "PUT" && Contains(r.path, "/Content?offset="))
        return { 204, "", "" };

    return { 404, "", "" };
}

// ── The child ─────────────────────────────────────────────────────────────────

static const char kDemoContent[] = "DEMO-CONTENT-0123456789";

static int RunMatchHttpChild(const wchar_t* dirArg, const wchar_t* phaseArg)
{
    const std::wstring dir   = WithSeparator(dirArg);
    const std::wstring phase = phaseArg;

    wprintf(L"\n--- Match Statistics HTTP Tests (child, %ls) ---\n", phase.c_str());

    HMODULE dll = LoadLibraryW((dir + L"LANCommander.Interposer.dll").c_str());
    MatchApi api;

    if (!dll || !api.Load(dll))
    {
        wprintf(L"  FAIL  MS-20: child could not load the DLL (GLE=%lu)\n", GetLastError());
        return 1;
    }

    InterposerMatchInfo info{};
    info.structSize = sizeof(info);
    info.map        = "de_test";

    if (phase == L"basic")
    {
        info.externalId = "ms-basic";
        const DWORD match = api.start(&info);

        ASSERT(api.Status(match).sink == INTERPOSER_MATCH_SINK_HTTP,
            L"MS-20: with ServerUrl, Token and GameId the match is sent over HTTP");

        api.joined(match, "name:Alice", "Alice", "red", 0);
        api.joined(match, "name:Bob", "Bob", "blue", 0);
        api.event(match, "kill", 1000, -1, "name:Alice", "name:Bob", "{\"weapon\":\"rocket\"}");
        api.event(match, "kill", 2000, -1, "name:Alice", "name:Bob", nullptr);
        api.event(match, "flag_captured", 3000, -1, "name:Alice", nullptr, nullptr);
        api.addStat(match, "name:Alice", nullptr, "kills", 2);
        api.addStat(match, "name:Alice", nullptr, "kills", 1);

        InterposerMatchResult result{};
        result.structSize = sizeof(result);
        result.playerKey  = "name:Alice";
        result.rank       = 1;
        result.timeMs     = -1;
        api.setResult(match, &result);

        api.attach(match, (dir + L"demo.dem").c_str(), INTERPOSER_MATCH_ATTACHMENT_DEMO, 0, 3000);
        api.end(match, "completed", FALSE);

        ASSERT(api.flush(match, 15000), L"MS-21: InterposerMatchFlush returns once the server has everything");

        InterposerMatchStatus st{};

        for (int i = 0; i < 50; ++i)
        {
            st = api.Status(match);

            if (st.delivered)
                break;

            Sleep(100);
        }

        ASSERT(st.delivered == 1 && st.pendingEvents == 0 && st.pendingAttachments == 0 &&
               st.rejectedEvents == 1 && strcmp(st.matchId, kFakeMatchId) == 0,
            L"MS-22: the status reports the match delivered, LANCommander's id, and the one rejected event");
    }
    else if (phase == L"crash")
    {
        info.externalId = "ms-crash";
        const DWORD match = api.start(&info);

        for (int i = 1; i <= 4; ++i)
            api.event(match, "tick", i * 1000, -1, nullptr, nullptr, nullptr);

        // Wait for the worker to spool them, register, and fail to send them.
        const std::wstring spoolRoot = dir + L".interposer\\Matches\\";
        bool spooled = false;

        for (int i = 0; i < 100 && !spooled; ++i)
        {
            Sleep(100);

            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((spoolRoot + L"ms-crash-*").c_str(), &data);

            if (find == INVALID_HANDLE_VALUE)
                continue;

            FindClose(find);

            const std::wstring sub    = spoolRoot + data.cFileName + L"\\";
            const std::string  events = ReadOpenLog(sub + L"events.jsonl");
            const std::string  meta   = ReadOpenLog(sub + L"match.json");

            spooled = SequencesIn(events).size() == 4 && Contains(meta, "\"sequencesFinal\":true") &&
                      api.Status(match).lastError[0] != 0;
        }

        ASSERT(spooled, L"MS-23: events the server will not take are kept in .interposer\\Matches");

        // No FreeLibrary, no detach: a crash.
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(g_fail));
    }
    else if (phase == L"resume")
    {
        info.externalId = "ms-crash";
        const DWORD match = api.start(&info);

        api.event(match, "tick", 5000, -1, nullptr, nullptr, nullptr);
        api.end(match, "completed", FALSE);

        ASSERT(api.flush(match, 15000), L"MS-24: the match left by the crashed run is carried on and finished");

        for (int i = 0; i < 50 && !api.Status(match).delivered; ++i)
            Sleep(100);
    }
    else if (phase == L"configure" || phase == L"configure-resume")
    {
        // Config.yml has no ServerUrl or Token here: the settings come from the API.
        const std::string url = ReadOpenLog(dir + L"server-url.txt");

        InterposerMatchConfig config{};
        config.structSize   = sizeof(config);
        config.enabled      = -1;
        config.serverUrl    = url.c_str();
        config.token        = "configured-token";
        config.gameId       = "55555555-5555-5555-5555-555555555555";
        config.reporterName = "configured-server";

        if (phase == L"configure")
        {
            info.externalId = "ms-before-configure";
            const DWORD before = api.start(&info);

            ASSERT(api.Status(before).sink == INTERPOSER_MATCH_SINK_NONE,
                L"MS-27: without settings in Config.yml or from the API a match is not sent");

            InterposerMatchConfig bad = config;
            bad.gameId = "not-a-guid";

            InterposerMatchConfig badKind = config;
            badKind.reporterKind = "Spectator";

            InterposerMatchConfig badUrl = config;
            badUrl.serverUrl = "ftp://example";

            InterposerMatchConfig tooSmall = config;
            tooSmall.structSize = sizeof(DWORD);

            ASSERT(!api.configure(&bad) && !api.configure(&badKind) && !api.configure(&badUrl) &&
                   !api.configure(&tooSmall) && !api.configure(nullptr),
                L"MS-28: InterposerMatchConfigure refuses a bad GUID, kind, URL or struct size");

            info.externalId = "ms-after-bad-configure";
            ASSERT(api.Status(api.start(&info)).sink == INTERPOSER_MATCH_SINK_NONE,
                L"MS-29: a refused configuration changes nothing");
        }

        ASSERT(api.configure(&config), L"MS-30: InterposerMatchConfigure accepts complete settings");

        info.externalId = phase == L"configure" ? "ms-configured" : "ms-crash";
        const DWORD match = api.start(&info);

        ASSERT(api.Status(match).sink == INTERPOSER_MATCH_SINK_HTTP,
            L"MS-31: a match started after InterposerMatchConfigure is sent over HTTP");

        api.event(match, "tick", 6000, -1, nullptr, nullptr, nullptr);
        api.end(match, "completed", FALSE);

        ASSERT(api.flush(match, 15000), L"MS-32: the configured match is delivered");

        for (int i = 0; i < 50 && !api.Status(match).delivered; ++i)
            Sleep(100);
    }

    FreeLibrary(dll);
    return g_fail;
}

// ── The parent ────────────────────────────────────────────────────────────────

static DWORD RunChildProcess(const std::wstring& command, const std::wstring& exePath)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command;

    if (!CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
        return 0xFFFFFFFF;

    WaitForSingleObject(pi.hProcess, 120 * 1000);

    DWORD exitCode = 0xFFFFFFFF;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode;
}

static bool AnySpoolLeft(const std::wstring& root)
{
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((root + L"*").c_str(), &data);

    if (find == INVALID_HANDLE_VALUE)
        return false;

    bool any = false;

    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(data.cFileName, L".") != 0 &&
            wcscmp(data.cFileName, L"..") != 0)
            any = true;
    } while (FindNextFileW(find, &data));

    FindClose(find);
    return any;
}

static void RemoveTree(const std::wstring& dir)
{
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((dir + L"*").c_str(), &data);

    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
                continue;

            const std::wstring path = dir + data.cFileName;

            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveTree(path + L"\\");
            else
                DeleteFileW(path.c_str());
        } while (FindNextFileW(find, &data));

        FindClose(find);
    }

    std::wstring trimmed = dir;

    if (!trimmed.empty() && trimmed.back() == L'\\')
        trimmed.pop_back();

    RemoveDirectoryW(trimmed.c_str());
}

static void RunMatchHttpTests(const std::wstring& exeDir, const std::wstring& testTmpDir)
{
    wprintf(L"\n--- Match Statistics HTTP Tests ---\n");

    FakeMatchServer server;
    std::wstring    phase      = L"basic";
    int             eventCalls = 0;
    long long       lastAccepted = 0;
    std::mutex      phaseMutex;

    server.handler = [&](const FakeRequest& r)
    {
        std::lock_guard<std::mutex> lock(phaseMutex);
        return AnswerMatchRequest(r, phase, eventCalls, lastAccepted);
    };

    if (!server.Start())
    {
        ASSERT(false, L"MS-10: starting the fake LANCommander server");
        return;
    }

    const std::wstring childDir   = testTmpDir + L"Matches\\";
    const std::wstring interposer = childDir + L".interposer\\";
    const std::wstring spoolRoot  = interposer + L"Matches\\";
    const std::wstring exePath    = exeDir + L"LANCommander.Interposer.Tests.exe";

    CreateDirs(interposer);

    if (!CopyFileW((exeDir + L"LANCommander.Interposer.dll").c_str(),
                   (childDir + L"LANCommander.Interposer.dll").c_str(), FALSE))
    {
        ASSERT(false, L"MS-10: staging the child's copy of the DLL");
        server.Stop();
        return;
    }

    WriteTextFile(interposer + L"Config.yml",
        "Logging:\n"
        "  Matches: true\n"
        "  Level: Debug\n"
        "\n"
        "MatchStatistics:\n"
        "  ServerUrl: 'http://127.0.0.1:" + std::to_string(server.Port()) + "/'\n"
        "  Token: test-token\n"
        "  GameId: " + std::string(kFakeGameId) + "\n"
        "  ReporterName: test-server\n"
        "  FlushInterval: 200\n");

    WriteTextFile(childDir + L"demo.dem", kDemoContent);

    const std::wstring childArg = childDir.substr(0, childDir.size() - 1);
    auto command = [&](const wchar_t* p) { return L"\"" + exePath + L"\" --match-http \"" + childArg + L"\" " + p; };

    // ── basic ────────────────────────────────────────────────────────────────
    ASSERT(RunChildProcess(command(L"basic"), exePath) == 0, L"MS-10: the basic child process reported no failures");

    {
        const std::vector<FakeRequest> requests = server.Requests();

        const FakeRequest* create = nullptr;
        const FakeRequest* participants = nullptr;
        const FakeRequest* end = nullptr;
        std::vector<const FakeRequest*> events;
        std::vector<std::string> attachmentSteps;
        std::string chunk;

        for (const FakeRequest& r : requests)
        {
            if (r.path == "/api/Matches")                 create = &r;
            else if (Contains(r.path, "/Participants"))   participants = &r;
            else if (Contains(r.path, "/Events"))         events.push_back(&r);
            else if (Contains(r.path, "/End"))            end = &r;
            else if (Contains(r.path, "/Attachments"))
            {
                attachmentSteps.push_back(r.method + " " + r.path.substr(r.path.find("/Attachments")));

                if (r.method == "PUT")
                    chunk = r.body;
            }
        }

        ASSERT(create && Contains(FakeMatchServer::Lower(create->headers), "authorization: bearer test-token") &&
               Contains(create->body, "\"gameId\":\"44444444-4444-4444-4444-444444444444\"") &&
               Contains(create->body, "\"externalId\":\"ms-basic\"") && Contains(create->body, "\"map\":\"de_test\"") &&
               Contains(create->body, "\"reporterKind\":\"Server\"") &&
               Contains(create->body, "\"reporterName\":\"test-server\""),
            L"MS-11: the match is registered with the token, the game id and the reporter");

        ASSERT(participants && Contains(participants->body, "\"playerKey\":\"name:Alice\"") &&
               Contains(participants->body, "\"playerKey\":\"name:Bob\""),
            L"MS-12: participants are sent once the match is registered");

        const std::vector<long long> expected{ 1, 2, 3 };

        ASSERT(events.size() == 2 && events[0]->status == 429 && events[1]->status == 202 &&
               events[0]->body == events[1]->body &&
               SequencesIn(events[1]->body) == expected &&
               Contains(events[1]->body, std::string("\"reporterId\":\"") .append(kFakeReporterId).append("\"").c_str()),
            L"MS-13: a 429 is waited out and the same batch, same ids, sent again");

        ASSERT(end && Contains(end->body, "\"reason\":\"completed\"") &&
               Contains(end->body, "{\"playerKey\":\"name:Alice\",\"key\":\"kills\",\"value\":3}") &&
               Contains(end->body, "\"rank\":1,\"status\":\"Finished\"") &&
               Contains(end->body, "\"participants\":[{"),
            L"MS-14: the end carries results, statistics and participants");

        ASSERT(attachmentSteps.size() == 3 && attachmentSteps[0] == "POST /Attachments" &&
               Contains(attachmentSteps[1], "PUT /Attachments/33333333-3333-3333-3333-333333333333/Content?offset=0") &&
               Contains(attachmentSteps[2], "/Complete") && chunk == kDemoContent,
            L"MS-15: the attachment is declared, uploaded from offset 0 and completed");

        ASSERT(!AnySpoolLeft(spoolRoot), L"MS-16: a delivered match leaves nothing in .interposer\\Matches");
    }

    // ── crash ────────────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(phaseMutex);
        phase = L"crash";
        lastAccepted = 0;
    }

    server.Clear();

    ASSERT(RunChildProcess(command(L"crash"), exePath) == 0, L"MS-17: the crashing child spooled its events before it died");
    ASSERT(AnySpoolLeft(spoolRoot), L"MS-18: the spool survives the crash");

    // ── resume: the server took the first two before the crash ───────────────
    {
        std::lock_guard<std::mutex> lock(phaseMutex);
        phase = L"resume";
        lastAccepted = 2;
    }

    server.Clear();

    ASSERT(RunChildProcess(command(L"resume"), exePath) == 0, L"MS-19: the resuming child process reported no failures");

    {
        std::multiset<long long> sent;
        bool ended = false;

        for (const FakeRequest& r : server.Requests())
        {
            if (Contains(r.path, "/Events") && r.status == 202)
                for (long long sequence : SequencesIn(r.body))
                    sent.insert(sequence);

            ended |= Contains(r.path, "/End") && r.status == 204;
        }

        const std::multiset<long long> expected{ 3, 4, 5 };

        ASSERT(sent == expected && ended,
            L"MS-25: only the events after the server's last are resent, the new one follows them, and the match ends");

        ASSERT(!AnySpoolLeft(spoolRoot), L"MS-26: the spool is removed once the resumed match is delivered");
    }

    // ── configure: the same server, settings from InterposerMatchConfigure ───
    const std::string httpConfig =
        "Logging:\n"
        "  Matches: true\n"
        "MatchStatistics:\n"
        "  ServerUrl: 'http://127.0.0.1:" + std::to_string(server.Port()) + "/'\n"
        "  Token: test-token\n"
        "  GameId: " + std::string(kFakeGameId) + "\n"
        "  FlushInterval: 200\n";
    const std::string apiConfig =
        "Logging:\n"
        "  Matches: true\n"
        "MatchStatistics:\n"
        "  FlushInterval: 200\n";

    WriteTextFile(interposer + L"Config.yml", apiConfig);
    WriteTextFile(childDir + L"server-url.txt", "http://127.0.0.1:" + std::to_string(server.Port()));

    {
        std::lock_guard<std::mutex> lock(phaseMutex);
        phase = L"configure";
        lastAccepted = 0;
    }

    server.Clear();

    ASSERT(RunChildProcess(command(L"configure"), exePath) == 0,
        L"MS-33: the configure child process reported no failures");

    {
        bool configured = false, onlyConfigured = true;

        for (const FakeRequest& r : server.Requests())
        {
            onlyConfigured &= Contains(FakeMatchServer::Lower(r.headers), "authorization: bearer configured-token");

            if (r.path == "/api/Matches")
            {
                configured |= Contains(r.body, "\"gameId\":\"55555555-5555-5555-5555-555555555555\"") &&
                              Contains(r.body, "\"reporterName\":\"configured-server\"") &&
                              Contains(r.body, "\"externalId\":\"ms-configured\"");
                onlyConfigured &= !Contains(r.body, "ms-before-configure") && !Contains(r.body, "ms-after-bad-configure");
            }
        }

        ASSERT(configured && onlyConfigured,
            L"MS-34: the configured token, game and reporter are sent, and nothing started before configuring");
    }

    // ── configure-resume: a crash spooled under Config.yml's settings, picked
    //    up by a run whose settings come from InterposerMatchConfigure ────────
    WriteTextFile(interposer + L"Config.yml", httpConfig);

    {
        std::lock_guard<std::mutex> lock(phaseMutex);
        phase = L"crash";
        lastAccepted = 0;
    }

    server.Clear();
    RunChildProcess(command(L"crash"), exePath);
    WriteTextFile(interposer + L"Config.yml", apiConfig);

    {
        std::lock_guard<std::mutex> lock(phaseMutex);
        phase = L"resume";
        lastAccepted = 0;
    }

    server.Clear();

    ASSERT(AnySpoolLeft(spoolRoot) && RunChildProcess(command(L"configure-resume"), exePath) == 0,
        L"MS-35: the configure-resume child process reported no failures");
    ASSERT(!AnySpoolLeft(spoolRoot),
        L"MS-36: a spooled match is picked up once InterposerMatchConfigure completes the settings");

    server.Stop();
    RemoveTree(childDir);
}

int wmain(int argc, wchar_t** argv)
{
    // Unbuffered: a crash mid-run otherwise discards everything still sitting in
    // the buffer, which is exactly the output needed to locate it.
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Re-entry from RunIsolatedTests. Registry.Isolated cannot share a process
    // with the rest of the suite, so it gets one of its own.
    if (argc >= 3 && wcscmp(argv[1], L"--isolated") == 0)
        return RunIsolatedChild(argv[2]);

    // Re-entry from RunLauncherConfigTests: one child per mode, each with a base
    // directory and config overlay of its own.
    if (argc >= 5 && wcscmp(argv[1], L"--launcher-config") == 0)
        return RunLauncherConfigChild(argv[2], argv[3], argv[4]);

    // Re-entry from RunRelayTests: one of the two relay peers.
    if (argc >= 5 && wcscmp(argv[1], L"--relay-peer") == 0)
        return RunRelayChild(argv[2], argv[3], argv[4]);

    // Re-entry from RunSecondInstanceTests: two copies of the DLL in one process.
    if (argc >= 4 && wcscmp(argv[1], L"--second-instance") == 0)
        return RunSecondInstanceChild(argv[2], argv[3]);

    // Re-entry from RunMatchHttpTests: one process per phase, against the fake
    // LANCommander server the parent runs.
    if (argc >= 4 && wcscmp(argv[1], L"--match-http") == 0)
        return RunMatchHttpChild(argv[2], argv[3]);

    // Re-entry from RunEmbeddedTests: a patch with the Interposer inside it,
    // loaded before or after LANCommander.Interposer.dll.
    if (argc >= 5 && wcscmp(argv[1], L"--embedded") == 0)
        return RunEmbeddedChild(argv[2], argv[3], argv[4]);

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

    // The audio stack has to be in the process before the Interposer loads, the
    // same as dinput below. It is loaded here rather than there because the
    // fixture depends on it: the unfiltered device counts and the endpoint the
    // Output list pins can only be read while nothing is hooked.
    HMODULE hWinmm  = LoadLibraryW(L"winmm.dll");
    HMODULE hDSound = LoadLibraryW(L"dsound.dll");
    HMODULE hOle32  = LoadLibraryW(L"ole32.dll");

    CaptureRealAudioCounts(hWinmm, hDSound);
    DiscoverPinnedEndpoint(hOle32);

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
    RunFastDLTests(exeDir);
    RunRegistryTests();
    RunOverlayTests();
    RunIdentityTests();
    RunOsVersionTests();
    RunDirectInputTests(hDInput);
    RunDirectInput8Tests(hDInput8);
    RunMouseTransformTests(hDll, hDInput);
    RunAudioTests(hDll, hWinmm, hDSound, hOle32);
    RunPluginConfigTests(hDll);
    RunVoiceTests(hDll);
    RunSdkTests(hDll);
    RunPresenceTests(hDll);
    RunMatchTests(hDll, exeDir);

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

    if (hWinmm)  FreeLibrary(hWinmm);
    if (hDSound) FreeLibrary(hDSound);
    if (hOle32)  FreeLibrary(hOle32);

    // ── Post-unload tests ─────────────────────────────────────────────────────
    // Discover the timestamped log created by LoadConfig during DLL_PROCESS_ATTACH.
    std::wstring logPath = FindFirstLogFile(logsDir);
    RunLogTests(logPath);
    RunSdkLogTests(logPath);
    RunVoiceLogTests(logPath);
    RunPersistenceTests(regPath);
    RunOverlayPersistenceTests(baseRegPath, regPath);
    RunPluginConfigPersistenceTests(yamlPath);

    // Last, because it spawns a second process that loads its own copy of the
    // DLL -- nothing after it should depend on this process's hooks.
    RunIsolatedTests(exeDir, testTmpDir);

    // Likewise: a base directory and config overlay are read once, at load.
    RunLauncherConfigTests(exeDir, testTmpDir);

    // Likewise: two copies of the DLL, which this process cannot host twice.
    RunSecondInstanceTests(exeDir, testTmpDir);

    // Likewise: a patch that embeds the Interposer, beside the DLL.
    RunEmbeddedTests(exeDir, testTmpDir);

    // Likewise separate processes: two relay peers, each with its own DLL.
    RunRelayTests(exeDir, testTmpDir);

    // Likewise: the HTTP sink reads its server from config, once, at load.
    RunMatchHttpTests(exeDir, testTmpDir);

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
