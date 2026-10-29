#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// ---------------------------------------------------------------------------
// One Interposer per process.
//
// Two copies can end up in the same process: a game patch that links
// LANCommander.Interposer.dll, in a game LANCommander has also deployed the
// Interposer to as version.dll; or an .asi beside a version.dll. Each copy has
// its own MinHook instance, so both would detour the same KernelBase exports
// and every file open would be logged, redirected and FastDL-probed twice,
// with two relay threads and two control pipes on top.
//
// So the first copy to reach DllMain claims Local\InterposerInstance_<pid> and
// becomes the primary; any later copy finds it there, installs nothing, and
// forwards every Interposer* export to the primary. The later copy pins the
// primary, so a forwarded call can never land in an unmapped image.
// ---------------------------------------------------------------------------

// Elect this copy the primary, or find the copy that already is. Returns true
// when this copy should install hooks. Call first thing in DLL_PROCESS_ATTACH.
bool ClaimPrimaryInstance(HMODULE self);

// Give up the claim, so a copy loaded after this one unloads can take over.
// Call from DLL_PROCESS_DETACH, primary only.
void ReleasePrimaryInstance();

// The primary this copy defers to, or null when this copy is the primary.
HMODULE PrimaryInstance();

// First statement of every Interposer* export:
//
//     BOOL InterposerGetUsername(wchar_t* buffer, DWORD bufferSize)
//     {
//         FORWARD_TO_PRIMARY(InterposerGetUsername, buffer, bufferSize);
//         ...
//
// In a secondary copy it returns whatever the primary's export returns. A
// primary too old to export the function falls through to this copy's own
// implementation, which sees configuration that was never loaded and so
// behaves as though every feature were off.
#define FORWARD_TO_PRIMARY(fn, ...)                                             \
    do                                                                          \
    {                                                                           \
        if (HMODULE primary_ = PrimaryInstance())                               \
        {                                                                       \
            static const auto forward_ = reinterpret_cast<decltype(&fn)>(       \
                reinterpret_cast<void*>(GetProcAddress(primary_, #fn)));        \
            if (forward_)                                                       \
                return forward_(__VA_ARGS__);                                   \
        }                                                                       \
    } while (0)
