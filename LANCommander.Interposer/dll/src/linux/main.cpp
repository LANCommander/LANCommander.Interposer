// The Linux entry point: what DllMain is on Windows.
//
// LD_PRELOAD maps this object into the game before main() runs, and its
// constructor runs then. It also runs when a patch links the object or a
// process dlopens it, and the destructor on dlclose or at exit.
//
// This file must stay LAST in the target's source list. Constructors run in
// .init_array order, which is link order, and both functions here use globals
// that other translation units construct dynamically (config.cpp's vectors and
// strings). Linked earlier, InstallHooks would parse into objects whose own
// initialisers then run and wipe the result.
//
// The statically linked libstdc++ is a library, so its initialisers land after
// even this one, and that cannot be changed from here. For GCC 12 that was
// checked by disassembly and is harmless: locale_inst/wlocale_inst only set
// guard bytes (locale::id's constructor is empty), system_error only registers
// exit-time destructors for constant-initialised objects, and eh_alloc builds
// the emergency exception pool, which is not touched while malloc succeeds.
// Re-check with objdump after a toolchain upgrade.

#include "hooks.h"
#include "instance.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include <unistd.h>

namespace
{
    bool g_installed = false;

    // INTERPOSER_TARGET: a comma-separated list of executable names. LD_PRELOAD
    // is inherited by every child process -- a wrapper script, the shell it
    // runs, a crash reporter -- and only the game should be hooked, configured
    // and logged. Unset means every process.
    bool IsTargetProcess()
    {
        const char* targets = getenv("INTERPOSER_TARGET");

        if (!targets || !*targets)
            return true;

        char exe[4096];
        const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

        if (length <= 0)
            return false;

        exe[length] = '\0';

        const char* slash = strrchr(exe, '/');
        const std::string name = slash ? slash + 1 : exe;

        std::string list = targets;
        size_t start = 0;

        while (start <= list.size())
        {
            size_t comma = list.find(',', start);
            if (comma == std::string::npos)
                comma = list.size();

            if (list.compare(start, comma - start, name) == 0)
                return true;

            start = comma + 1;
        }

        return false;
    }
}

__attribute__((constructor))
static void InterposerLoad()
{
    // Dormant outside the target: no claim either, so a copy in a process
    // that is not the game never stands in the way of one that is.
    if (!IsTargetProcess())
        return;

    // Another copy already owns this process. Install nothing; the exports
    // forward to it (see instance.h).
    if (!ClaimPrimaryInstance(platform::SelfModule()))
        return;

    g_installed = true;

    try
    {
        InstallHooks();
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "LANCommander.Interposer: InstallHooks failed: %s\n", e.what());
    }
    catch (...)
    {
        fprintf(stderr, "LANCommander.Interposer: InstallHooks failed\n");
    }
}

__attribute__((destructor))
static void InterposerUnload()
{
    if (!g_installed)
        return;

    RemoveHooks();
    ReleasePrimaryInstance();
    g_installed = false;
}
