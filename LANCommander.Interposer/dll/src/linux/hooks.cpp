#include "hooks.h"
#include "config.h"
#include "hooking.h"
#include "pipe_events.h"
#include "plugins.h"

void InstallHooks()
{
    hooking::Initialize();

    // Before EnableAll, for the same reason as on Windows: the detours pass
    // through until then, so reading Config.yml cannot re-enter them.
    LoadConfig();

    hooking::EnableAll();

    ConnectEventPipe();

    LoadPlugins();
}

void RemoveHooks()
{
    DisconnectEventPipe();
    UnloadPlugins();
    hooking::DisableAll();
    hooking::Uninitialize();
    CloseLog();
}
