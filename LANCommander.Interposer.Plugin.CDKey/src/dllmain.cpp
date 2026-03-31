//
// LANCommander.Interposer.Plugin.CDKey
//
// Generates a deterministic CD key from the current player username and injects
// it into a configurable registry value so the game reads it as if it were
// installed with that key.
//
// Required Config.yml entries (under Plugins.CDKey):
//
//   Plugins:
//     CDKey:
//       Mask:      "****-**********-*******-****"  # '*' = generated char, anything else = literal
//       KeyPath:   "Battlefield 1942\\ergc"         # suffix match — no hive prefix required
//       ValueName: "@"                              # "@" or omit for the default (unnamed) value
//
// KeyPath is matched as a suffix against every key in the virtual registry store,
// so "Battlefield 1942\ergc" matches any full path that ends with that component.
// The virtual store is populated from .interposer\Registry.reg; the target key
// must appear there (even as an empty key header) for the injection to take effect.
//
// ValueName "@" refers to the default (unnamed) registry value, i.e. RegEdit's
// "(Default)" entry.  Any other string names a specific value.
//
// The generated key is deterministic: the same username always produces the same
// key for a given mask.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>

// ─── Interposer plugin API ────────────────────────────────────────────────────
// All functions are resolved at runtime via GetProcAddress so the plugin has no
// link-time dependency on the interposer.

using FnInterposerLog                      = void  (WINAPI*)(const wchar_t* verb, const wchar_t* message);
using FnInterposerGetConfigString          = BOOL  (WINAPI*)(const wchar_t* dotPath, wchar_t* buf, DWORD bufSize);
using FnInterposerGetUsername              = BOOL  (WINAPI*)(wchar_t* buf, DWORD bufSize);
using FnInterposerSetRegistryValueBySuffix = DWORD (WINAPI*)(const wchar_t* keySuffix, const wchar_t* valueName, const wchar_t* value);

static FnInterposerLog                      pfnLog          = nullptr;
static FnInterposerGetConfigString          pfnGetConfig    = nullptr;
static FnInterposerGetUsername              pfnGetUser      = nullptr;
static FnInterposerSetRegistryValueBySuffix pfnSetRegSuffix = nullptr;

static constexpr const wchar_t* kVerb = L"CDKEY";

// ─── Key generation ───────────────────────────────────────────────────────────
//
// Algorithm:
//   1. Compute a 32-bit FNV-1a hash of the uppercased username as the seed.
//   2. Step an LCG (Numerical Recipes constants) for each '*' in the mask.
//   3. Map each LCG output to a character from [A-Z0-9] (36 symbols).
//   Non-'*' characters in the mask (e.g. '-') are preserved verbatim.
//
static std::wstring GenerateKey(const std::wstring& username, const std::wstring& mask)
{
    // FNV-1a hash over uppercased username
    uint32_t hash = 2166136261u;
    for (wchar_t c : username)
    {
        hash ^= static_cast<uint32_t>(towupper(c));
        hash *= 16777619u;
    }

    static const wchar_t kAlphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    constexpr size_t kAlphabetLen    = (sizeof(kAlphabet) / sizeof(wchar_t)) - 1; // 36

    std::wstring key;
    key.reserve(mask.size());

    uint32_t state = hash;
    for (wchar_t c : mask)
    {
        if (c == L'*')
        {
            state = state * 1664525u + 1013904223u; // Numerical Recipes LCG
            key  += kAlphabet[(state >> 16) % kAlphabetLen];
        }
        else
        {
            key += c;
        }
    }

    return key;
}

// ─── Resolve interposer exports ───────────────────────────────────────────────
static bool ResolveAPI()
{
    // The interposer DLL may be loaded under its standard name or as version.dll / .asi.
    static const wchar_t* kCandidates[] = {
        L"LANCommander.Interposer.dll",
        L"version.dll",
    };

    HMODULE hInterposer = nullptr;
    for (const wchar_t* name : kCandidates)
    {
        hInterposer = GetModuleHandleW(name);
        if (hInterposer) break;
    }

    if (!hInterposer)
        return false;

    pfnLog          = reinterpret_cast<FnInterposerLog>(                     GetProcAddress(hInterposer, "InterposerLog"));
    pfnGetConfig    = reinterpret_cast<FnInterposerGetConfigString>(          GetProcAddress(hInterposer, "InterposerGetConfigString"));
    pfnGetUser      = reinterpret_cast<FnInterposerGetUsername>(              GetProcAddress(hInterposer, "InterposerGetUsername"));
    pfnSetRegSuffix = reinterpret_cast<FnInterposerSetRegistryValueBySuffix>(GetProcAddress(hInterposer, "InterposerSetRegistryValueBySuffix"));

    return pfnLog && pfnGetConfig && pfnGetUser && pfnSetRegSuffix;
}

// ─── DLL initialisation ───────────────────────────────────────────────────────
static void Initialize()
{
    if (!ResolveAPI())
        return; // interposer not present; nothing to do

    // ── Read configuration ───────────────────────────────────────────────────
    wchar_t mask[256]     = {};
    wchar_t keyPath[1024] = {};

    if (!pfnGetConfig(L"Plugins.CDKey.Mask",    mask,    ARRAYSIZE(mask)) ||
        !pfnGetConfig(L"Plugins.CDKey.KeyPath", keyPath, ARRAYSIZE(keyPath)))
    {
        pfnLog(kVerb, L"Configuration incomplete — Mask and KeyPath are required");
        return;
    }

    // ValueName defaults to "@" (the default/unnamed registry value) if not set.
    wchar_t valueName[256] = L"@";
    pfnGetConfig(L"Plugins.CDKey.ValueName", valueName, ARRAYSIZE(valueName));

    // ── Resolve username ─────────────────────────────────────────────────────
    wchar_t username[256] = {};
    if (!pfnGetUser(username, ARRAYSIZE(username)) || username[0] == L'\0')
    {
        pfnLog(kVerb, L"Username unavailable — cannot generate key");
        return;
    }

    // ── Generate key and inject into virtual registry ────────────────────────
    std::wstring key = GenerateKey(username, mask);

    DWORD matched = pfnSetRegSuffix(keyPath, valueName, key.c_str());

    if (matched == 0)
    {
        std::wstring warn;
        warn.reserve(256);
        warn += L"No virtual store keys matched suffix \"";
        warn += keyPath;
        warn += L"\" — add the key to Registry.reg";
        pfnLog(kVerb, warn.c_str());
        return;
    }

    // Log: <keyPath>\<valueName>  ->  <key>  (<n> key(s) updated)
    std::wstring msg;
    msg.reserve(512);
    msg += keyPath;
    msg += L'\\';
    msg += valueName;
    msg += L"  ->  ";
    msg += key;
    if (matched > 1)
    {
        wchar_t countBuf[32];
        wsprintfW(countBuf, L"  (%lu keys)", matched);
        msg += countBuf;
    }
    pfnLog(kVerb, msg.c_str());
}

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD fdwReason, LPVOID /*lpReserved*/)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
        Initialize();
    return TRUE;
}
