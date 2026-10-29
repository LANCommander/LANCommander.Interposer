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

// InterposerAudioDevice and InterposerEnumAudioDevices are declared in the
// public header.
#include "LANCommander.Interposer.h"
