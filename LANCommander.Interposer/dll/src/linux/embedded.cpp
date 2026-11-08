// The embedding API on Linux (LANCommander.Interposer.Embedded.h): the
// Interposer compiled into a host's own shared object -- a game server plugin,
// a patch -- rather than preloaded beside it.
//
// LANCommander.Interposer.Static is the shared object's sources without
// main.cpp, so nothing runs at load: the host calls InterposerEmbeddedAttach
// once it is ready, and InterposerEmbeddedDetach before it unloads. By then
// every static initialiser in the host's image has run, so the link-order
// constraint main.cpp documents does not apply here.
//
// What attach brings up is the library's API, not its hooks: the config and
// session log, the launcher's event channel, match statistics and plugins. The
// libc interposers (files.cpp, network.cpp, identity.cpp) are linked in but
// never enabled, so they pass every call straight to libc. A host loaded with
// dlopen(RTLD_LOCAL) -- as game servers load their plugins -- could not
// interpose on the process with them anyway; hooking a game is the preloaded
// .so's job.
//
// The base directory is the host's: platform::SelfModule() finds the image this
// code is linked into by address, so .interposer/ is looked for beside it.

#include "config.h"
#include "instance.h"
#include "matches.h"
#include "pipe_events.h"
#include "plugins.h"

#include "LANCommander.Interposer.Embedded.h"

#include <cstdio>
#include <exception>

namespace
{
    bool g_attached = false;
}

extern "C" BOOL InterposerEmbeddedAttach(void)
{
    if (g_attached)
        return TRUE;

    // Another copy already owns this process. Install nothing; the exports
    // forward to it (see instance.h).
    if (!ClaimPrimaryInstance(platform::SelfModule()))
        return FALSE;

    g_attached = true;

    try
    {
        LoadConfig();
        ConnectEventPipe();
        InitMatches();
        LoadPlugins();
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "LANCommander.Interposer: embedded attach failed: %s\n", e.what());
    }
    catch (...)
    {
        fprintf(stderr, "LANCommander.Interposer: embedded attach failed\n");
    }

    return TRUE;
}

extern "C" void InterposerEmbeddedDetach(BOOL processTerminating)
{
    if (!g_attached)
        return;

    DisconnectEventPipe();
    UnloadPlugins();
    ShutdownMatches(processTerminating != FALSE);
    CloseLog();
    ReleasePrimaryInstance();

    g_attached = false;
}
