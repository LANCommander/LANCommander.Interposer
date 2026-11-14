#include "hooks.h"
#include "audio.h"
#include "config.h"
#include "control.h"
#include "dinput.h"
#include "fastdl.h"
#include "interposer_features.h"
#include "files.h"
#include "identity.h"
#include "matches.h"
#include "network.h"
#include "osversion.h"
#include "pipe_events.h"
#include "plugins.h"
#include "registry.h"
#include "relay.h"
#include "richpresence.h"

#include "hooking.h"

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InstallHooks()
{
#ifdef _DEBUG
    // Spin until a debugger attaches, then break.
    // Attach to this process in VS (Debug → Attach to Process) while it waits.
    while (!IsDebuggerPresent())
        Sleep(100);
    __debugbreak();
#endif

    hooking::Initialize();

    LoadConfig();
    LogCompiledFeatures();

    // Each subsystem below is compiled out of an embedded build that leaves its
    // feature off (core/interposer_features.h). FastDL and the relay need no guard: their
    // headers turn every entry point into a no-op instead.
#if INTERPOSER_FEATURE_REGISTRY
    InstallRegistryHooks();
#endif
#if INTERPOSER_FEATURE_FILES
    InstallFileHooks();
#endif
    InitFastDL();
    InitRelay();            // decides RelayHooksWanted, which InstallNetworkHooks reads
#if INTERPOSER_FEATURE_NETWORK
    InstallNetworkHooks();
#endif
#if INTERPOSER_FEATURE_IDENTITY
    InstallIdentityHooks();
#endif
#if INTERPOSER_FEATURE_OSVERSION
    InstallOsVersionHooks();
#endif
#if INTERPOSER_FEATURE_DIRECTINPUT
    InstallDirectInputHooks();
#endif
#if INTERPOSER_FEATURE_AUDIO
    InstallAudioHooks();
#endif

    hooking::EnableAll();

    ConnectEventPipe();
    StartControlPipe();

#if INTERPOSER_FEATURE_RICH_PRESENCE
    InitRichPresence();
#endif

#if INTERPOSER_FEATURE_MATCHES
    // After the event pipe: a match's sink depends on whether it connected.
    InitMatches();
#endif

#if INTERPOSER_FEATURE_PLUGINS
    LoadPlugins();
#endif
}

void OnLibraryLoaded(HMODULE hModule)
{
    if (!hModule) return;

    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(hModule, path, MAX_PATH);

    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* name  = slash ? slash + 1 : path;

#if INTERPOSER_FEATURE_NETWORK
    LateInstallNetworkHooks(name);
#endif
#if INTERPOSER_FEATURE_DIRECTINPUT
    LateInstallDirectInputHooks(name);
#endif
#if INTERPOSER_FEATURE_AUDIO
    LateInstallAudioHooks(name);
#endif
    (void)name;
}

void RemoveHooks(bool processTerminating)
{
    StopControlPipe(processTerminating);
    ShutdownRelay(processTerminating);
    DisconnectEventPipe();
#if INTERPOSER_FEATURE_PLUGINS
    UnloadPlugins();
#endif
#if INTERPOSER_FEATURE_MATCHES
    ShutdownMatches(processTerminating);   // before ShutdownFastDL: both hold the HTTP client
#endif
#if INTERPOSER_FEATURE_RICH_PRESENCE
    ShutdownRichPresence();
#endif
#if INTERPOSER_FEATURE_REGISTRY
    RemoveRegistryHooks();
#endif
#if INTERPOSER_FEATURE_NETWORK
    RemoveNetworkHooks();
#endif
#if INTERPOSER_FEATURE_DIRECTINPUT
    RemoveDirectInputHooks();
#endif
#if INTERPOSER_FEATURE_AUDIO
    RemoveAudioHooks();
#endif
    ShutdownFastDL();
    hooking::DisableAll();
    hooking::Uninitialize();
    CloseLog();
}
