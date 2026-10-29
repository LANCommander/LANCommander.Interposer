#include "hooking.h"
#include "config.h"
#include "minhook_support.h"

namespace hooking
{

namespace
{
    Status FromMinHook(MH_STATUS status)
    {
        switch (status)
        {
        case MH_OK:                      return Status::Ok;
        case MH_ERROR_ALREADY_CREATED:   return Status::AlreadyHooked;
        case MH_ERROR_MODULE_NOT_FOUND:  return Status::ModuleNotFound;
        case MH_ERROR_FUNCTION_NOT_FOUND:return Status::FunctionNotFound;
        default:                         return Status::Failed;
        }
    }
}

bool Initialize()
{
    return MH_Initialize() == MH_OK;
}

void Uninitialize()
{
    MH_Uninitialize();
}

void EnableAll()
{
    MH_EnableHook(MH_ALL_HOOKS);
}

void DisableAll()
{
    MH_DisableHook(MH_ALL_HOOKS);
}

Status HookExport(const wchar_t* module, const char* function, void* detour, void** original)
{
    MH_STATUS status = MH_CreateHookApi(module, function, detour, original);
    LogHookInit(module, function, status);
    return FromMinHook(status);
}

void LogHookStatus(const wchar_t* module, const char* function, Status status, const char* detail)
{
    if (status == Status::AlreadyHooked)
        return; // Hook was already installed (e.g. late-install called twice); not an error.

    std::wstring message = std::wstring(module) + L"!" + platform::Utf8ToWide(function);

    if (status == Status::ModuleNotFound || status == Status::FunctionNotFound)
        // Expected on older Windows rather than a fault: XP has no RegGetValue,
        // RegDeleteTree, CreateFile2, Core Audio, ... and nothing written for it
        // calls them. Worded so a log from such a system does not read as broken.
        message += L"  not present on this version of Windows";
    else if (status != Status::Ok)
        message += L"  " + platform::Utf8ToWide(detail ? detail : "failed");

    InterposerLog(L"HOOK INIT", message.c_str());
}

} // namespace hooking

void LogHookInit(const wchar_t* module, const char* fn, MH_STATUS status)
{
    hooking::LogHookStatus(module, fn, hooking::FromMinHook(status), MH_StatusToString(status));
}
