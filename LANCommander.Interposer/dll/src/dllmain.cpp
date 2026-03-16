#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "hooks.h"

#ifdef INTERPOSER_PROXY
void InitProxy();
void UninitProxy();
#endif

static void WriteLog(const wchar_t* message)
{
    wchar_t temporaryPath[MAX_PATH];
    
    GetTempPathW(MAX_PATH, temporaryPath);
    
    wcscat_s(temporaryPath, L"Interposer.log");
    
    HANDLE fileHandle = CreateFileW(temporaryPath, FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    
    if (fileHandle == INVALID_HANDLE_VALUE) 
        return;
    
    DWORD written;
    
    WriteFile(fileHandle, message, static_cast<DWORD>(wcslen(message) * sizeof(wchar_t)), &written, nullptr);
    WriteFile(fileHandle, L"\r\n", 4, &written, nullptr);
    CloseHandle(fileHandle);
}

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD fdwReason, LPVOID /*lpvReserved*/)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInstance);
#ifdef INTERPOSER_PROXY
        InitProxy();
#endif
        WriteLog(L"DLL_PROCESS_ATTACH: calling InstallHooks");
        __try
        {
            InstallHooks();
            WriteLog(L"InstallHooks returned OK");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            wchar_t buf[64];
            wsprintfW(buf, L"InstallHooks EXCEPTION code=0x%08X", GetExceptionCode());
            WriteLog(buf);
        }
        break;

    case DLL_PROCESS_DETACH:
        RemoveHooks();
#ifdef INTERPOSER_PROXY
        UninitProxy();
#endif
        break;

    default:
        break;
    }
    return TRUE;
}
