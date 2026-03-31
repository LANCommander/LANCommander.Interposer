#include "plugins.h"
#include "config.h"

#include <windows.h>
#include <vector>

static std::vector<HMODULE> g_plugins;

// ---------------------------------------------------------------------------
// LoadPlugins
// ---------------------------------------------------------------------------
void LoadPlugins()
{
    // Locate <dlldir>\.interposer\Plugins
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadPlugins),
        &hSelf);

    wchar_t dllPathBuffer[MAX_PATH] = {};
    GetModuleFileNameW(hSelf, dllPathBuffer, MAX_PATH);

    std::wstring dllDir(dllPathBuffer);
    auto slash = dllDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dllDir.resize(slash + 1);

    std::wstring pluginsDir = dllDir + L".interposer\\Plugins\\";

    // Enumerate .dll files
    for (const wchar_t* ext : { L"*.dll", L"*.asi" })
    {
        std::wstring pattern = pluginsDir + ext;

        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);

        if (hFind == INVALID_HANDLE_VALUE)
            continue;

        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;

            std::wstring fullPath = pluginsDir + fd.cFileName;
            HMODULE hMod = LoadLibraryW(fullPath.c_str());

            if (hMod)
            {
                g_plugins.push_back(hMod);
                LogFileAccess(L"PLUGIN LOAD", fullPath.c_str());
            }
            else
            {
                // Log load failure (error code appended to path)
                wchar_t errBuf[MAX_PATH + 32];
                wsprintfW(errBuf, L"%s  (error %u)", fullPath.c_str(), GetLastError());
                LogFileAccess(L"PLUGIN ERROR", errBuf);
            }
        }
        while (FindNextFileW(hFind, &fd));

        FindClose(hFind);
    }
}

// ---------------------------------------------------------------------------
// UnloadPlugins
// ---------------------------------------------------------------------------
void UnloadPlugins()
{
    // Unload in reverse load order
    for (auto it = g_plugins.rbegin(); it != g_plugins.rend(); ++it)
        FreeLibrary(*it);

    g_plugins.clear();
}
