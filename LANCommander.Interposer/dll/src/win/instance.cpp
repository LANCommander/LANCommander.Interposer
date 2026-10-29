#include "instance.h"

#include <cwchar>

namespace
{
    // Held open by the primary for as long as it is loaded: the mapping object
    // exists exactly while some copy owns the process, so a later copy that
    // finds it can trust the module handle inside.
    HANDLE  g_claim   = nullptr;
    HMODULE g_primary = nullptr;

    void InstanceName(wchar_t* name, size_t size)
    {
        swprintf_s(name, size, L"Local\\InterposerInstance_%lu", GetCurrentProcessId());
    }
}

bool ClaimPrimaryInstance(platform::Module selfModule)
{
    const HMODULE self = static_cast<HMODULE>(selfModule);

    wchar_t name[64];
    InstanceName(name, ARRAYSIZE(name));

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(HMODULE), name);

    // Without the mapping there is no way to coordinate. Behave as every copy
    // did before there was an election, rather than leave the game unhooked.
    if (!mapping)
        return true;

    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;

    auto* slot = static_cast<HMODULE*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HMODULE)));

    if (!slot)
    {
        CloseHandle(mapping);
        return true;
    }

    if (existed)
    {
        HMODULE primary = *slot;
        HMODULE pinned  = nullptr;

        // Pinning is what keeps FORWARD_TO_PRIMARY safe: the primary can no
        // longer be unloaded out from under a call this copy forwards to it.
        // It is also the liveness check -- a slot naming an image that is no
        // longer mapped fails here and this copy takes over instead.
        if (primary && primary != self &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(primary), &pinned) &&
            pinned == primary)
        {
            g_primary = primary;
            UnmapViewOfFile(slot);
            CloseHandle(mapping);
            return false;
        }
    }

    *slot = self;
    UnmapViewOfFile(slot);
    g_claim = mapping;
    return true;
}

void ReleasePrimaryInstance()
{
    if (g_claim)
    {
        CloseHandle(g_claim);
        g_claim = nullptr;
    }
}

platform::Module PrimaryInstance()
{
    return g_primary;
}
