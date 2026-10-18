#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Install the audio device enumeration hooks: winmm (waveOut/waveIn), dsound
// (DirectSoundEnumerate / DirectSoundCaptureEnumerate) and ole32
// (CoCreateInstance, the way a game reaches the Core Audio MMDevice
// enumerator). Call from InstallHooks(), after LoadConfig().
// No-op unless the AudioDevices filter is active or Logging.Audio is set.
void InstallAudioHooks();

// Called when a DLL is loaded post-startup. Installs and enables any audio
// hooks for moduleName (the DLL's basename, e.g. L"dsound.dll") that could not
// be installed at startup because the module was not yet loaded. A game that
// links dsound.dll or the audio stack lazily is the normal case.
void LateInstallAudioHooks(const wchar_t* moduleName);

// Revert the MMDevice vtable patches and clear internal state. Call from
// RemoveHooks() before MH_DisableHook — MinHook knows nothing about the vtable
// slots, and an MMDevAPI vtable still pointing into this DLL after it unloads
// would fault.
void RemoveAudioHooks();

// ---------------------------------------------------------------------------
// Unified audio device enumeration — the counterpart to
// InterposerEnumNetworkAdapters, exported so plugins (via GetProcAddress) and
// managed hosts (via P/Invoke) share one canonical device list rather than
// re-implementing Core Audio enumeration. Fixed-layout struct = stable ABI.
// ---------------------------------------------------------------------------
typedef struct InterposerAudioDevice {
    wchar_t name[256];        // friendly name, e.g. "Speakers (Realtek High Definition Audio)"
    wchar_t description[256]; // device description, e.g. "Speakers"
    wchar_t id[256];          // Core Audio endpoint ID
    wchar_t instanceId[256];  // device instance path, e.g. SWD\MMDEVAPI\{0.0.0...}
    wchar_t hardwareId[256];  // most specific hardware ID, e.g. USB\VID_0965&PID_9001, empty if none
    int     direction;        // 1 = output (render), 2 = input (capture), 0 = unknown
    int     isDefault;        // 1 = the system default endpoint for its direction
    int     allowed;          // 1 = passes the AudioDevices filter (or no filter), 0 = hidden
} InterposerAudioDevice;

// Fills up to `capacity` entries into `buffer` and writes the number written to
// *outCount. Returns the TOTAL number of devices available (call with capacity
// 0 to query the count, then resize and call again).
//
// Reports the real device set regardless of the filter, and every string in the
// struct is one an AudioDevices entry will match on — so a picker UI can list
// every device, show which ones the game currently sees, and hand the user the
// exact text to paste into their config. The DLL allocates nothing across the
// boundary — the caller owns `buffer`.
extern "C" __declspec(dllexport) DWORD InterposerEnumAudioDevices(
    InterposerAudioDevice* buffer, DWORD capacity, DWORD* outCount);
