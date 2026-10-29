#include "instance.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dlfcn.h>
#include <unistd.h>

// The election on Linux. There is no named kernel object to hold, so the claim
// is an environment variable, INTERPOSER_INSTANCE_<pid>, naming the address of
// the primary's InterposerInstanceMarker. A later copy checks with dladdr that
// the address really is that symbol in a loaded object before trusting it --
// which also rejects a claim inherited through exec, where the PID survives but
// the image does not -- and pins the primary with RTLD_NODELETE, the
// counterpart of GET_MODULE_HANDLE_EX_FLAG_PIN.

// Exported so dladdr can name it; the value is irrelevant.
extern "C" INTERPOSER_API const char InterposerInstanceMarker = 0;

namespace
{
    platform::Module g_primary = nullptr;
    bool             g_claimed = false;

    std::string ClaimVariable()
    {
        return "INTERPOSER_INSTANCE_" + std::to_string(getpid());
    }

    std::string MarkerText()
    {
        char text[32];
        snprintf(text, sizeof(text), "%p", static_cast<const void*>(&InterposerInstanceMarker));
        return text;
    }

    // The primary a claim names, pinned, or null when the claim is stale.
    platform::Module PinClaimant(const char* claim)
    {
        void* address = nullptr;

        if (sscanf(claim, "%p", &address) != 1 || !address)
            return nullptr;

        Dl_info info{};

        if (!dladdr(address, &info) || !info.dli_fname || !info.dli_sname ||
            strcmp(info.dli_sname, "InterposerInstanceMarker") != 0 || info.dli_saddr != address)
            return nullptr;

        // NOLOAD: only an object that is already mapped. NODELETE: never unmap
        // it, so nothing this copy forwards to can disappear. The reference
        // this takes is never dropped, which is the point.
        return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
    }
}

bool ClaimPrimaryInstance(platform::Module self)
{
    const std::string variable = ClaimVariable();

    if (const char* claim = getenv(variable.c_str()))
    {
        platform::Module primary = PinClaimant(claim);

        if (primary && primary != self)
        {
            g_primary = primary;
            return false;
        }
    }

    // Without the variable there is no way to coordinate. Install anyway,
    // rather than leave the game unhooked.
    g_claimed = setenv(variable.c_str(), MarkerText().c_str(), 1) == 0;
    return true;
}

void ReleasePrimaryInstance()
{
    if (!g_claimed)
        return;

    const std::string variable = ClaimVariable();

    // Only a claim that is still ours: a later copy may have taken over.
    if (const char* claim = getenv(variable.c_str()); claim && MarkerText() == claim)
        unsetenv(variable.c_str());

    g_claimed = false;
}

platform::Module PrimaryInstance()
{
    return g_primary;
}
