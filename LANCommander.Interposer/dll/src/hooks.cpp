#include "hooks.h"
#include "config.h"
#include "fastdl.h"
#include "files.h"
#include "identity.h"
#include "network.h"
#include "plugins.h"
#include "registry.h"

#include <MinHook.h>

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InstallHooks()
{
    MH_Initialize();

    LoadConfig();
    InstallRegistryHooks();
    InstallFileHooks();
    InitFastDL();
    InstallNetworkHooks();
    InstallIdentityHooks();

    MH_EnableHook(MH_ALL_HOOKS);

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
}

void RemoveHooks()
{
    UnloadPlugins();
    RemoveRegistryHooks();
    RemoveNetworkHooks();
    ShutdownFastDL();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    CloseLog();
}
