#include "hooking.h"
#include "config.h"

#include <atomic>
#include <dlfcn.h>

// On Linux a hook is a function this shared object exports under the name of
// the one it replaces. Preloaded, it comes first in symbol lookup, so the game's
// calls already reach it; nothing is patched. HookExport only looks up the next
// definition after ours -- normally libc's -- for the detour to call.
//
// Interposition is in force from the moment the object is mapped, long before
// anything is configured, so "enabled" is a flag every detour checks: until
// EnableAll it must pass straight through to `original`.

namespace hooking
{

namespace
{
    std::atomic<bool> g_enabled{ false };
}

bool Initialize()
{
    return true;
}

void Uninitialize()
{
}

void EnableAll()
{
    g_enabled.store(true, std::memory_order_release);
}

void DisableAll()
{
    g_enabled.store(false, std::memory_order_release);
}

Status HookExport(const wchar_t* module, const char* function, void* /*detour*/, void** original)
{
    // RTLD_NEXT searches the objects loaded after this one, whatever module
    // the caller named: symbol lookup on Linux is by name, not by library.
    void* next = dlsym(RTLD_NEXT, function);

    Status status = next ? Status::Ok : Status::FunctionNotFound;

    if (next)
        *original = next;

    LogHookStatus(module, function, status);
    return status;
}

void LogHookStatus(const wchar_t* module, const char* function, Status status, const char* detail)
{
    if (status == Status::AlreadyHooked)
        return;

    std::wstring message = std::wstring(module) + L"!" + platform::Utf8ToWide(function);

    if (status == Status::ModuleNotFound || status == Status::FunctionNotFound)
        message += L"  not present in this process";
    else if (status != Status::Ok)
        message += L"  " + platform::Utf8ToWide(detail ? detail : "failed");

    InterposerLog(L"HOOK INIT", message.c_str());
}

} // namespace hooking
