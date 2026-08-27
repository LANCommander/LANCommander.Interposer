//
// LANCommander.Interposer.Plugin.Example
//
// Minimal example plugin for LANCommander Interposer. Use this as a starting
// point for writing your own plugins.
//
// Build this project as a DLL and drop it (or a subfolder containing it) into
// .interposer\Plugins\ next to the Interposer DLL. The Interposer loads all
// .dll and .asi files from that directory tree automatically.
//
// Plugin lifecycle:
//   1. Interposer calls LoadLibrary on your DLL  -> DllMain(DLL_PROCESS_ATTACH)
//   2. Interposer looks for an "InterposerPluginInit" export via GetProcAddress.
//      If found, it calls InterposerPluginInit(hInterposer) with its own HMODULE.
//   3. Use the provided HMODULE to resolve any Interposer exports you need.
//   4. On shutdown the Interposer calls FreeLibrary   -> DllMain(DLL_PROCESS_DETACH)
//
// Available Interposer exports (resolve with GetProcAddress):
//
//   Configuration / Logging (wchar_t*):
//     InterposerGetUsername(wchar_t* buf, DWORD bufSize)          -> BOOL
//     InterposerLog(const wchar_t* verb, const wchar_t* message)  -> void
//     InterposerGetConfigString(const wchar_t* dotPath,
//                               wchar_t* buf, DWORD bufSize)      -> BOOL
//     InterposerRegisterPluginConfig(const wchar_t* pluginName,
//                                    const wchar_t* yamlDefaults) -> BOOL
//
//   Virtual Registry (wchar_t*):
//     InterposerSetRegistryValue(const wchar_t* keyPath,
//                                const wchar_t* valueName,
//                                const wchar_t* value)            -> void
//     InterposerSetRegistryValueBySuffix(const wchar_t* keySuffix,
//                                        const wchar_t* valueName,
//                                        const wchar_t* value)    -> DWORD
//
//   Rich Presence (char* — UTF-8):
//     InterposerSetPresenceDetails(const char* details)           -> void
//     InterposerSetPresenceState(const char* state)               -> void
//     InterposerSetPresenceTimestamps(int64_t start, int64_t end) -> void
//     InterposerSetPresenceLargeImage(const char* key, text)      -> void
//     InterposerSetPresenceSmallImage(const char* key, text)      -> void
//     InterposerSetPresenceParty(const char* id, int size, max)   -> void
//     InterposerSetPresenceButton(int index, const char* text, url) -> void
//     InterposerSetPresenceType(int type)                         -> void
//     InterposerSetPresenceName(const char* name)                 -> void
//     InterposerUpdatePresence()                                  -> void
//     InterposerClearPresence()                                   -> void
//
// Config.yml example for this plugin:
//
//   Plugins:
//     Example:
//       Greeting: "Hello from the example plugin!"
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

// ---- Interposer API typedefs ------------------------------------------------
// Declare function pointer types for each export you intend to use.
// These have no link-time dependency on the Interposer.

// The Interposer's exports are __cdecl — they are declared without a calling
// convention and the export table carries their undecorated names. Declaring
// these pointers __stdcall instead leaks the arguments on every call on x86.
using FnInterposerLog                  = void (*)(const wchar_t* verb, const wchar_t* message);
using FnInterposerGetConfigString      = BOOL (*)(const wchar_t* dotPath, wchar_t* buf, DWORD bufSize);
using FnInterposerGetUsername          = BOOL (*)(wchar_t* buf, DWORD bufSize);
using FnInterposerRegisterPluginConfig = BOOL (*)(const wchar_t* pluginName, const wchar_t* yamlDefaults);

static FnInterposerLog                pfnLog       = nullptr;
static FnInterposerGetConfigString    pfnGetConfig = nullptr;
static FnInterposerGetUsername        pfnGetUser   = nullptr;

// ---- Plugin entry point -----------------------------------------------------
// The Interposer calls this immediately after LoadLibrary, passing its own
// HMODULE. Use it for all GetProcAddress calls — this works regardless of
// whether the Interposer was deployed as LANCommander.Interposer.dll,
// version.dll, dinput8.dll, or an .asi file.

extern "C" __declspec(dllexport) void WINAPI InterposerPluginInit(HMODULE hInterposer)
{
    // Resolve the exports we need.
    pfnLog       = reinterpret_cast<FnInterposerLog>(GetProcAddress(hInterposer, "InterposerLog"));
    pfnGetConfig = reinterpret_cast<FnInterposerGetConfigString>(GetProcAddress(hInterposer, "InterposerGetConfigString"));
    pfnGetUser   = reinterpret_cast<FnInterposerGetUsername>(GetProcAddress(hInterposer, "InterposerGetUsername"));

    if (!pfnLog)
        return; // Can't even log — bail out.

    // Register default config. On first run this writes the Plugins.Example
    // section into Config.yml; on subsequent runs it's a no-op.
    auto pfnRegConfig = reinterpret_cast<FnInterposerRegisterPluginConfig>(
        GetProcAddress(hInterposer, "InterposerRegisterPluginConfig"));
    if (pfnRegConfig)
        pfnRegConfig(L"Example", L"Greeting: 'Hello from the example plugin!'");

    // Read a plugin-specific setting from Config.yml.
    wchar_t greeting[256] = {};
    if (pfnGetConfig && pfnGetConfig(L"Plugins.Example.Greeting", greeting, ARRAYSIZE(greeting)))
    {
        pfnLog(L"EXAMPLE", greeting);
    }
    else
    {
        pfnLog(L"EXAMPLE", L"Plugin loaded (no Plugins.Example.Greeting configured)");
    }

    // Demonstrate reading the current username.
    wchar_t username[256] = {};
    if (pfnGetUser && pfnGetUser(username, ARRAYSIZE(username)))
    {
        std::wstring msg = L"Current player: ";
        msg += username;
        pfnLog(L"EXAMPLE", msg.c_str());
    }
}

// ---- DllMain ----------------------------------------------------------------
// Keep DllMain minimal. Heavy work belongs in InterposerPluginInit where you
// have guaranteed access to the Interposer API.

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD /*fdwReason*/, LPVOID /*lpReserved*/)
{
    return TRUE;
}
