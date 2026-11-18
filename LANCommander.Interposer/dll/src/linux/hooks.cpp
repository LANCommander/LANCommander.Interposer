#include "hooks.h"
#include "config.h"
#include "fastdl.h"
#include "interposer_features.h"
#include "files.h"
#include "hooking.h"
#include "identity.h"
#include "matches.h"
#include "network.h"
#include "pipe_events.h"
#include "plugins.h"
#include "relay.h"

void InstallHooks()
{
    hooking::Initialize();

    // Before EnableAll, for the same reason as on Windows: the detours pass
    // through until then, so reading Config.yml cannot re-enter them.
    LoadConfig();
    LogCompiledFeatures();

    // Each subsystem below is compiled out of an embedded build that leaves its
    // feature off (core/interposer_features.h); FastDL's header no-ops it instead.
    InitFastDL();
#if INTERPOSER_FEATURE_FILES
    InstallFileHooks();
#endif
    // Decides RelayHooksWanted, which the network hooks read, and starts the
    // relay thread. relay.h no-ops it in a build without the relay.
    InitRelay();
#if INTERPOSER_FEATURE_NETWORK
    InstallNetworkHooks();
#endif
#if INTERPOSER_FEATURE_IDENTITY
    InstallIdentityHooks();
#endif

    hooking::EnableAll();

    ConnectEventPipe();

#if INTERPOSER_FEATURE_MATCHES
    InitMatches();
#endif

#if INTERPOSER_FEATURE_PLUGINS
    LoadPlugins();
#endif
}

void RemoveHooks()
{
    // Normally already done: relay.cpp stops its thread from an atexit
    // handler, which runs before this module's globals are destroyed.
    ShutdownRelay(false);
    DisconnectEventPipe();
#if INTERPOSER_FEATURE_PLUGINS
    UnloadPlugins();
#endif
#if INTERPOSER_FEATURE_MATCHES
    ShutdownMatches(false);
#endif
    hooking::DisableAll();
    ShutdownFastDL();
    hooking::Uninitialize();
    CloseLog();
}
