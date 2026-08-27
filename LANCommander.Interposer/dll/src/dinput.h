#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// ---------------------------------------------------------------------------
// DirectInput support.
//
// Two features, configured under the DirectInput section of Config.yml.
//
// 1. FixLegacyDeviceEnumeration — serve DirectInput 3/7 from DirectInput 8.
//
//    The legacy dinput.dll shipped with Windows overruns an internal
//    116-byte-per-object record array while enumerating the HID device set on
//    some machines:
//
//        dinput.dll+0x12941   movl $0x2710,0x40(%edx)   <- writes past its block
//
//    Confirmed with full page heap and reproduced in a standalone program with
//    no game code: DirectInput 7 hangs without page heap and crashes with it,
//    while DirectInput 8 walks the identical devices cleanly. Games that ask
//    for version 0x0300 or 0x0700 land on the broken path. The bug is in
//    Windows, not in the game, and there is nothing the game can do about it.
//
// 2. DeviceFilter — an allow-list applied to device enumeration, so a game can
//    be shown only the devices it should bind to. It covers bridged DirectInput
//    3/7 games and native DirectInput 8 games alike.
//
//    When the filter admits nothing but the system mouse and keyboard, the
//    enumeration is answered directly instead of being run, because a real
//    enumeration can cost ~21 seconds per call on a large HID stack and those
//    two devices need no enumeration to be created.
//
// The bridge is reached two ways: the dinput.dll proxy build calls
// DirectInputBridgeCreate directly from its exports, and every other load
// method installs MinHook detours over the real dinput.dll exports. The filter
// additionally hooks dinput8.dll's DirectInput8Create.
// ---------------------------------------------------------------------------

// Create a bridged IDirectInput object on top of dinput8.dll. `ppDI` receives
// something the caller may drive as IDirectInput3A or IDirectInput7A. Returns
// E_FAIL if dinput8.dll cannot be loaded.
HRESULT DirectInputBridgeCreate(HINSTANCE hinst, DWORD version, void** ppDI, void* punkOuter);

// True when `riid` is one of the ANSI IDirectInput IIDs the bridge can serve
// (IDirectInputA / 2A / 7A). DirectInputCreateEx callers asking for a Unicode
// interface have to be passed through to the real dinput.dll instead.
bool DirectInputBridgeWantsIid(const void* riid);

// Create a DirectInput 8 object through the real dinput8.dll and attach the
// device filter to it. Used by the dinput8.dll proxy build, which serves the
// export itself rather than having it hooked.
HRESULT DirectInput8CreateFiltered(HINSTANCE hinst, DWORD version, const void* riid,
    void** ppvOut, void* punkOuter);

// Install MinHook detours over dinput.dll's DirectInputCreateA/Ex and
// dinput8.dll's DirectInput8Create. Call from InstallHooks(), after LoadConfig()
// and before MH_EnableHook. Each is a no-op when its feature is switched off,
// and the exports a proxy build serves itself are never hooked.
void InstallDirectInputHooks();

// Called when a DLL is loaded post-startup. Installs and enables the detours
// above for modules that were not loaded at startup.
void LateInstallDirectInputHooks(const wchar_t* moduleName);

// Revert the vtable patches and drop cached state. Call from RemoveHooks()
// before MH_DisableHook.
void RemoveDirectInputHooks();
