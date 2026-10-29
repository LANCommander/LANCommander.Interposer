#pragma once

// Installing function hooks, independent of how a platform does it. On Windows
// a hook is a MinHook detour created on an export and enabled with the rest in
// one step. On a platform that hooks by symbol interposition the detour is
// already in place by being exported, and HookExport only resolves the next
// definition of the symbol for `original` to call.
//
// Windows subsystems written before this interface still call MinHook directly
// through minhook_support.h; they move over as they gain a second platform.

namespace hooking
{
    enum class Status
    {
        Ok,
        AlreadyHooked,     // a second install of the same hook: not an error
        ModuleNotFound,    // expected on older systems, see LogHookStatus
        FunctionNotFound,
        Failed,
    };

    // Prepare / tear down the hooking engine. Initialize before any HookExport.
    bool Initialize();
    void Uninitialize();

    // Activate or deactivate every hook created so far.
    void EnableAll();
    void DisableAll();

    // Hook `function` exported by `module`. `original` receives what the detour
    // calls to reach the real implementation.
    Status HookExport(const wchar_t* module, const char* function, void* detour, void** original);

    // Write the [HOOK INIT] line for a hook. Always written, whatever the
    // Logging flags; AlreadyHooked writes nothing.
    void LogHookStatus(const wchar_t* module, const char* function, Status status, const char* detail = nullptr);
}
