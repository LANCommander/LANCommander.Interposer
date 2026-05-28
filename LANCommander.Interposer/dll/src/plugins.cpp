#include "plugins.h"
#include "config.h"

#include <windows.h>
#include <vector>

static std::vector<HMODULE> g_plugins;

// Signature for the optional plugin init callback.
using FnInterposerPluginInit = void (WINAPI*)(HMODULE hInterposer);

// ---------------------------------------------------------------------------
// LoadPlugins
// ---------------------------------------------------------------------------
void LoadPlugins()
{
    // Locate <dlldir>\.interposer\Plugins and our own HMODULE (passed to plugins).
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

    // Recursively enumerate .dll and .asi files in Plugins and all subdirectories
    std::vector<std::wstring> dirs;
    dirs.push_back(pluginsDir);

    while (!dirs.empty())
    {
        std::wstring dir = std::move(dirs.back());
        dirs.pop_back();

        // Discover subdirectories
        {
            std::wstring pattern = dir + L"*";
            WIN32_FIND_DATAW fd{};
            HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);

            if (hFind != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        continue;
                    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                        continue;

                    dirs.push_back(dir + fd.cFileName + L"\\");
                }
                while (FindNextFileW(hFind, &fd));

                FindClose(hFind);
            }
        }

        // Load plugin files from this directory
        for (const wchar_t* ext : { L"*.dll", L"*.asi" })
        {
            std::wstring pattern = dir + ext;

            WIN32_FIND_DATAW fd{};
            HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);

            if (hFind == INVALID_HANDLE_VALUE)
                continue;

            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;

                std::wstring fullPath = dir + fd.cFileName;
                HMODULE hMod = LoadLibraryW(fullPath.c_str());

                if (hMod)
                {
                    g_plugins.push_back(hMod);
                    LogPluginEvent(L"PLUGIN LOAD", fullPath.c_str());

                    // If the plugin exports InterposerPluginInit, call it with
                    // our own HMODULE so it can resolve exports without guessing
                    // the DLL name (which varies by deployment: .dll, version.dll,
                    // dinput8.dll, .asi).
                    auto pfnInit = reinterpret_cast<FnInterposerPluginInit>(
                        GetProcAddress(hMod, "InterposerPluginInit"));
                    if (pfnInit)
                        pfnInit(hSelf);
                }
                else
                {
                    wchar_t errBuf[MAX_PATH + 32];
                    wsprintfW(errBuf, L"%s  (error %u)", fullPath.c_str(), GetLastError());
                    LogPluginEvent(L"PLUGIN ERROR", errBuf);
                }
            }
            while (FindNextFileW(hFind, &fd));

            FindClose(hFind);
        }
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
