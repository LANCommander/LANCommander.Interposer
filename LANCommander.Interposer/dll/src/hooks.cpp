#include "hooks.h"
#include "config.h"
#include "dinput.h"
#include "fastdl.h"
#include "files.h"
#include "identity.h"
#include "network.h"
#include "pipe_events.h"
#include "plugins.h"
#include "registry.h"
#include "richpresence.h"

#include <MinHook.h>

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

    MH_Initialize();

    LoadConfig();
    InstallRegistryHooks();
    InstallFileHooks();
    InitFastDL();
    InstallNetworkHooks();
    InstallIdentityHooks();
    InstallDirectInputHooks();

    MH_EnableHook(MH_ALL_HOOKS);

    ConnectEventPipe();

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
}

void RemoveHooks()
{
    DisconnectEventPipe();
    UnloadPlugins();
    ShutdownRichPresence();
    RemoveRegistryHooks();
    RemoveNetworkHooks();
    RemoveDirectInputHooks();
    ShutdownFastDL();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    CloseLog();
}
