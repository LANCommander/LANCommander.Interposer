// The DirectInput entry points, for an embedded build with
// INTERPOSER_FEATURE_DIRECTINPUT off (see core/interposer_features.h). There is no bridge
// and no device filter to apply, so each goes straight to the system DLL --
// what a patch deployed as dinput.dll / dinput8.dll would otherwise have to do
// itself. Loaded by explicit System32 path for the reason proxy.cpp gives: the
// patch calling this may itself be named dinput.dll, and a bare name would find
// it and recurse.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "instance.h"

namespace
{
    // Never freed, as in proxy.cpp: a caller may still be inside the system DLL
    // at detach, and the process keeps it loaded anyway.
    FARPROC SystemProc(const wchar_t* module, const char* name)
    {
        wchar_t path[MAX_PATH]{};

        const UINT length = GetSystemDirectoryW(path, MAX_PATH);

        if (length == 0 || length > MAX_PATH - 16)
            return nullptr;

        wcscat_s(path, L"\\");
        wcscat_s(path, module);

        const HMODULE handle = LoadLibraryW(path);

        return handle ? GetProcAddress(handle, name) : nullptr;
    }

    template <typename Fn>
    Fn Resolve(const wchar_t* module, const char* name)
    {
        return reinterpret_cast<Fn>(reinterpret_cast<void*>(SystemProc(module, name)));
    }
}

extern "C" {

INTERPOSER_API LONG InterposerDirectInputCreateA(HINSTANCE hinst, DWORD version, void** ppvOut, void* punkOuter)
{
    FORWARD_TO_PRIMARY(InterposerDirectInputCreateA, hinst, version, ppvOut, punkOuter);

    using Fn = HRESULT (WINAPI*)(HINSTANCE, DWORD, void**, void*);
    static const Fn create = Resolve<Fn>(L"dinput.dll", "DirectInputCreateA");

    return create ? create(hinst, version, ppvOut, punkOuter) : E_FAIL;
}

INTERPOSER_API LONG InterposerDirectInputCreateEx(HINSTANCE hinst, DWORD version, const void* riid,
    void** ppvOut, void* punkOuter)
{
    FORWARD_TO_PRIMARY(InterposerDirectInputCreateEx, hinst, version, riid, ppvOut, punkOuter);

    using Fn = HRESULT (WINAPI*)(HINSTANCE, DWORD, const void*, void**, void*);
    static const Fn create = Resolve<Fn>(L"dinput.dll", "DirectInputCreateEx");

    return create ? create(hinst, version, riid, ppvOut, punkOuter) : E_FAIL;
}

INTERPOSER_API LONG InterposerDirectInput8Create(HINSTANCE hinst, DWORD version, const void* riid,
    void** ppvOut, void* punkOuter)
{
    FORWARD_TO_PRIMARY(InterposerDirectInput8Create, hinst, version, riid, ppvOut, punkOuter);

    using Fn = HRESULT (WINAPI*)(HINSTANCE, DWORD, const void*, void**, void*);
    static const Fn create = Resolve<Fn>(L"dinput8.dll", "DirectInput8Create");

    return create ? create(hinst, version, riid, ppvOut, punkOuter) : E_FAIL;
}

} // extern "C"
