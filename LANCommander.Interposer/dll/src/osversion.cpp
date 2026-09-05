#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include <string>

#include "osversion.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Trampoline types & pointers
//
// RTL_OSVERSIONINFOW and RTL_OSVERSIONINFOEXW live in the DDK headers, not in
// <windows.h>, but they are field-for-field identical to OSVERSIONINFOW and
// OSVERSIONINFOEXW — so RtlGetVersion reuses the same fill helper rather than
// redeclaring the pair. The static_asserts below are what keeps that honest.
// ---------------------------------------------------------------------------
using FnGetVersion    = DWORD(WINAPI*)();
using FnGetVersionExA = BOOL (WINAPI*)(LPOSVERSIONINFOA);
using FnGetVersionExW = BOOL (WINAPI*)(LPOSVERSIONINFOW);
using FnRtlGetVersion = LONG (NTAPI *)(OSVERSIONINFOEXW*);

static FnGetVersion    g_origGetVersion    = nullptr;
static FnGetVersionExA g_origGetVersionExA = nullptr;
static FnGetVersionExW g_origGetVersionExW = nullptr;
static FnRtlGetVersion g_origRtlGetVersion = nullptr;

static_assert(sizeof(OSVERSIONINFOW)   == 276, "RTL_OSVERSIONINFOW layout assumption");
static_assert(sizeof(OSVERSIONINFOEXW) == 284, "RTL_OSVERSIONINFOEXW layout assumption");

namespace {

// ---------------------------------------------------------------------------
// Fill helpers
//
// Both variants of the struct open with the same five DWORDs followed by
// szCSDVersion, so the base fill is shared and the EX fields are appended only
// when the caller sized its struct for them.
// ---------------------------------------------------------------------------

// Copy the service pack string, truncating to fit szCSDVersion's 128 chars.
void CopyCsd(wchar_t* dest, size_t destChars, const std::wstring& source)
{
    size_t count = source.size() < destChars - 1 ? source.size() : destChars - 1;

    wmemcpy(dest, source.c_str(), count);
    dest[count] = L'\0';
}

void CopyCsd(char* dest, size_t destChars, const std::wstring& source)
{
    // WideCharToMultiByte null-terminates when it has room; a source too long
    // for the buffer returns 0, in which case an empty string is the honest
    // answer — a half-converted service pack name is worse than none.
    int written = WideCharToMultiByte(CP_ACP, 0, source.c_str(), -1,
        dest, static_cast<int>(destChars), nullptr, nullptr);

    if (written <= 0)
        dest[0] = '\0';
}

template <typename InfoT, typename InfoExT>
void FillVersionInfo(InfoT* info)
{
    info->dwMajorVersion = g_osMajorVersion;
    info->dwMinorVersion = g_osMinorVersion;
    info->dwBuildNumber  = g_osBuildNumber;
    info->dwPlatformId   = VER_PLATFORM_WIN32_NT;

    CopyCsd(info->szCSDVersion, ARRAYSIZE(info->szCSDVersion), g_osServicePack);

    if (info->dwOSVersionInfoSize < sizeof(InfoExT))
        return;

    InfoExT* ex = reinterpret_cast<InfoExT*>(info);

    ex->wServicePackMajor = g_osServicePackMajor;
    ex->wServicePackMinor = 0;
    ex->wSuiteMask        = g_osSuiteMask;
    ex->wProductType      = g_osProductType;
    ex->wReserved         = 0;
}

// Render the spoofed version the way the log shows it, e.g.
// "5.2.3790 Service Pack 2".
std::wstring DescribeVersion()
{
    std::wstring text = std::to_wstring(g_osMajorVersion) + L"." +
                        std::to_wstring(g_osMinorVersion) + L"." +
                        std::to_wstring(g_osBuildNumber);

    if (!g_osServicePack.empty())
        text += L" " + g_osServicePack;

    return text;
}

// One line per call would flood the log — games call GetVersion from inside
// loops — so each entry point reports the first time it is used and then stays
// quiet for the rest of the session.
void LogOnce(bool& reported, const wchar_t* fn)
{
    if (reported)
        return;

    reported = true;

    LogOsVersion(L"OSVERSION", fn, DescribeVersion().c_str());
}

} // namespace

// ---------------------------------------------------------------------------
// Hook implementations
// ---------------------------------------------------------------------------

// The packed form: major in the low byte, minor in the next, build in the high
// word. The top bit stays clear, which is what marks the platform as NT.
static DWORD WINAPI HookGetVersion()
{
    static bool reported = false;
    LogOnce(reported, L"GetVersion");

    DWORD packed = (g_osMajorVersion & 0xFF) | ((g_osMinorVersion & 0xFF) << 8);

    if (g_osBuildNumber <= 0x7FFF)
        packed |= (g_osBuildNumber << 16);

    return packed;
}

// An unrecognized dwOSVersionInfoSize is passed through to the real API rather
// than guessed at: the call was going to fail anyway, and the OS decides how.
static BOOL WINAPI HookGetVersionExA(LPOSVERSIONINFOA info)
{
    if (!info)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOA) &&
        info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXA))
        return g_origGetVersionExA(info);

    static bool reported = false;
    LogOnce(reported, L"GetVersionExA");

    FillVersionInfo<OSVERSIONINFOA, OSVERSIONINFOEXA>(info);

    return TRUE;
}

static BOOL WINAPI HookGetVersionExW(LPOSVERSIONINFOW info)
{
    if (!info)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOW) &&
        info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXW))
        return g_origGetVersionExW(info);

    static bool reported = false;
    LogOnce(reported, L"GetVersionExW");

    FillVersionInfo<OSVERSIONINFOW, OSVERSIONINFOEXW>(info);

    return TRUE;
}

// Hooked as well as GetVersionEx, which an AppCompat layer does NOT do — a game
// or launcher that reads the version this way sees straight through
// compatibility mode, but not through this.
static LONG NTAPI HookRtlGetVersion(OSVERSIONINFOEXW* info)
{
    if (!info)
        return static_cast<LONG>(0xC000000DL); // STATUS_INVALID_PARAMETER

    if (info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOW) &&
        info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXW))
        return g_origRtlGetVersion(info);

    static bool reported = false;
    LogOnce(reported, L"RtlGetVersion");

    FillVersionInfo<OSVERSIONINFOW, OSVERSIONINFOEXW>(
        reinterpret_cast<OSVERSIONINFOW*>(info));

    return 0; // STATUS_SUCCESS
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InstallOsVersionHooks()
{
    if (!g_osVersionEnabled)
        return;

    std::wstring summary = DescribeVersion() +
        (g_osProductType == VER_NT_WORKSTATION ? L" (workstation)" : L" (server)");

    LogOsVersion(L"OSVERSION", g_osVersionName.c_str(), summary.c_str());

    LogHookInit(L"kernel32", "GetVersion",
        MH_CreateHookApi(L"kernel32", "GetVersion",
            reinterpret_cast<LPVOID>(HookGetVersion),
            reinterpret_cast<LPVOID*>(&g_origGetVersion)));

    LogHookInit(L"kernel32", "GetVersionExA",
        MH_CreateHookApi(L"kernel32", "GetVersionExA",
            reinterpret_cast<LPVOID>(HookGetVersionExA),
            reinterpret_cast<LPVOID*>(&g_origGetVersionExA)));

    LogHookInit(L"kernel32", "GetVersionExW",
        MH_CreateHookApi(L"kernel32", "GetVersionExW",
            reinterpret_cast<LPVOID>(HookGetVersionExW),
            reinterpret_cast<LPVOID*>(&g_origGetVersionExW)));

    LogHookInit(L"ntdll", "RtlGetVersion",
        MH_CreateHookApi(L"ntdll", "RtlGetVersion",
            reinterpret_cast<LPVOID>(HookRtlGetVersion),
            reinterpret_cast<LPVOID*>(&g_origRtlGetVersion)));
}
