#include "hooks.h"
#include "audio.h"
#include "config.h"
#include "control.h"
#include "dinput.h"
#include "fastdl.h"
#include "files.h"
#include "identity.h"
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
    InstallRegistryHooks();
    InstallFileHooks();
    InitFastDL();
    InitRelay();            // decides RelayHooksWanted, which InstallNetworkHooks reads
    InstallNetworkHooks();
    InstallIdentityHooks();
    InstallOsVersionHooks();
    InstallDirectInputHooks();
    InstallAudioHooks();

    hooking::EnableAll();

    ConnectEventPipe();
    StartControlPipe();

    InitRichPresence();

    LoadPlugins();
}

void OnLibraryLoaded(HMODULE hModule)
{
    if (!hModule) return;

    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(hModule, path, MAX_PATH);

    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* name  = slash ? slash + 1 : path;

    LateInstallNetworkHooks(name);
    LateInstallDirectInputHooks(name);
    LateInstallAudioHooks(name);
}

void RemoveHooks(bool processTerminating)
{
    StopControlPipe(processTerminating);
    ShutdownRelay(processTerminating);
    DisconnectEventPipe();
    UnloadPlugins();
    ShutdownRichPresence();
    RemoveRegistryHooks();
    RemoveNetworkHooks();
    RemoveDirectInputHooks();
    RemoveAudioHooks();
    ShutdownFastDL();
    hooking::DisableAll();
    hooking::Uninitialize();
    CloseLog();
}
