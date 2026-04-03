#include "hooks.h"
#include "borderless.h"
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

    if (g_borderlessEnabled)
        InstallBorderlessHooks();

    MH_EnableHook(MH_ALL_HOOKS);

    LoadPlugins();
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
