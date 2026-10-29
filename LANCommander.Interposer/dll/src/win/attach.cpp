#include "attach.h"

#include "hooks.h"
#include "instance.h"
#include "LANCommander.Interposer.Embedded.h"

namespace
{
    void WriteLog(const wchar_t* message)
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
}

bool AttachInterposer(HMODULE self)
{
    // Another copy already owns this process. Install nothing; the exports
    // forward to it (see instance.h).
    if (!ClaimPrimaryInstance(self))
    {
        WriteLog(L"DLL_PROCESS_ATTACH: another Interposer is already loaded; deferring to it");
        return false;
    }

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

    return true;
}

void DetachInterposer(bool processTerminating)
{
    if (PrimaryInstance())
        return;

    RemoveHooks(processTerminating);
    ReleasePrimaryInstance();
}

// ---------------------------------------------------------------------------
// The embedding API (LANCommander.Interposer.Embedded.h). Deliberately not
// exported: only the image that links the library calls these, from its own
// DllMain, and a copy reached by name must never be attached a second time.
// ---------------------------------------------------------------------------

extern "C" BOOL InterposerEmbeddedAttach(HMODULE module)
{
    return AttachInterposer(module) ? TRUE : FALSE;
}

extern "C" void InterposerEmbeddedDetach(BOOL processTerminating)
{
    DetachInterposer(processTerminating != FALSE);
}
