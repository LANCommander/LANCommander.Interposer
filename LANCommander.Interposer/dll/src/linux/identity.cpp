#include "identity.h"
#include "callbacks.h"
#include "config.h"
#include "hooking.h"
#include "platform.h"

#include <cstring>
#include <string>

#include <dlfcn.h>
#include <sys/utsname.h>
#include <unistd.h>

// gethostname carries __attribute__((nonnull)) on its buffer, so GCC flags the
// deliberate null guard. The guard stays: a game may pass null.
#pragma GCC diagnostic ignored "-Wnonnull-compare"

// Computer-name override on Linux. Where Windows hooks GetComputerName, here the
// shared object exports gethostname and uname: a game reading the machine name
// through either sees g_computername instead. The username override is served
// through InterposerGetUsername (the SDK) rather than a libc hook -- getlogin
// and the passwd database are not what a game reads for a display name.

namespace
{
    bool Enabled() { return hooking::Enabled() && !g_computername.empty(); }

    std::string ComputerNameUtf8()
    {
        return platform::WideToUtf8(g_computername);
    }
}

extern "C" INTERPOSER_API int gethostname(char* name, size_t len)
{
    static int (*real)(char*, size_t) = nullptr;
    if (!real)
        real = reinterpret_cast<int (*)(char*, size_t)>(dlsym(RTLD_NEXT, "gethostname"));

    if (!Enabled() || !name)
        return real ? real(name, len) : -1;

    const std::string value = ComputerNameUtf8();

    // Truncate like the real call: it fails with ENAMETOOLONG if the buffer is
    // too small, but most callers pass HOST_NAME_MAX; copy what fits with a NUL.
    if (value.size() + 1 > len)
    {
        if (len > 0)
        {
            std::memcpy(name, value.c_str(), len - 1);
            name[len - 1] = '\0';
        }
        return -1;
    }

    std::memcpy(name, value.c_str(), value.size() + 1);

    FireIdentityCallback(L"COMPUTERNAME", g_computername.c_str());
    LogIdentityAccess(L"IDENTITY", (L"gethostname -> " + g_computername).c_str());
    return 0;
}

extern "C" INTERPOSER_API int uname(struct utsname* buf)
{
    static int (*real)(struct utsname*) = nullptr;
    if (!real)
        real = reinterpret_cast<int (*)(struct utsname*)>(dlsym(RTLD_NEXT, "uname"));

    const int result = real ? real(buf) : -1;

    if (result != 0 || !buf || !Enabled())
        return result;

    const std::string value = ComputerNameUtf8();

    // nodename is a fixed-size array; copy what fits with a NUL.
    std::strncpy(buf->nodename, value.c_str(), sizeof(buf->nodename) - 1);
    buf->nodename[sizeof(buf->nodename) - 1] = '\0';

    FireIdentityCallback(L"COMPUTERNAME", g_computername.c_str());
    LogIdentityAccess(L"IDENTITY", (L"uname -> " + g_computername).c_str());
    return 0;
}

void InstallIdentityHooks()
{
    // Launcher overrides, on top of Player.Username / Player.ComputerName from
    // Config.yml (the environment counterparts of the Windows injector MMFs).
    if (std::string user = platform::ReadLauncherValue(L"InterposerUsername"); !user.empty())
        g_username = platform::Utf8ToWide(user);

    if (std::string computer = platform::ReadLauncherValue(L"InterposerComputerName"); !computer.empty())
        g_computername = platform::Utf8ToWide(computer);

    if (!g_username.empty())
        LogIdentityAccess(L"IDENTITY", (L"Username override: " + g_username).c_str());

    if (!g_computername.empty())
    {
        LogIdentityAccess(L"IDENTITY", (L"ComputerName override: " + g_computername).c_str());
        hooking::LogHookStatus(L"libc", "gethostname", hooking::Status::Ok);
        hooking::LogHookStatus(L"libc", "uname", hooking::Status::Ok);
    }
}
