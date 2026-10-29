#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "attach.h"

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInstance);
        AttachInterposer(hInstance);
        break;

    case DLL_PROCESS_DETACH:
        DetachInterposer(lpvReserved != nullptr);
        break;

    default:
        break;
    }
    return TRUE;
}
