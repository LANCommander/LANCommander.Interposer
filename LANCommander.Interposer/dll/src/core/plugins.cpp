#include "plugins.h"
#include "config.h"

#include <cwctype>
#include <vector>

static std::vector<platform::Module> g_plugins;

// Signature for the optional plugin init callback. WINAPI (__stdcall) on
// Windows, which matters on x86; nothing elsewhere.
using FnInterposerPluginInit = void (WINAPI*)(platform::Module hInterposer);

// True when `name` ends with `extension`, ignoring case.
static bool HasExtension(const std::wstring& name, const std::wstring& extension)
{
    if (name.size() < extension.size())
        return false;

    for (size_t i = 0; i < extension.size(); ++i)
    {
        if (std::towlower(name[name.size() - extension.size() + i]) != std::towlower(extension[i]))
            return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// LoadPlugins
// ---------------------------------------------------------------------------
void LoadPlugins()
{
    // Locate <base>/.interposer/Plugins -- the base directory is the DLL's own
    // unless the launcher named another -- and our own module (passed to plugins).
    platform::Module self = platform::SelfModule();

    std::wstring pluginsDir = GetInterposerDataDirectory() + L"Plugins" + platform::kPathSeparator;

    // Recursively enumerate plugin files in Plugins and all subdirectories
    std::vector<std::wstring> dirs;
    dirs.push_back(pluginsDir);

    while (!dirs.empty())
    {
        std::wstring dir = std::move(dirs.back());
        dirs.pop_back();

        const std::vector<platform::DirectoryEntry> entries = platform::ListDirectory(dir);

        // Discover subdirectories
        for (const platform::DirectoryEntry& entry : entries)
        {
            if (entry.isDirectory)
                dirs.push_back(dir + entry.name + platform::kPathSeparator);
        }

        // Load plugin files from this directory, one extension at a time
        for (const std::wstring& extension : platform::PluginExtensions())
        {
            for (const platform::DirectoryEntry& entry : entries)
            {
                if (entry.isDirectory || !HasExtension(entry.name, extension))
                    continue;

                std::wstring fullPath = dir + entry.name;
                std::wstring error;
                platform::Module module = platform::LoadModule(fullPath, error);

                if (module)
                {
                    g_plugins.push_back(module);
                    LogPluginEvent(L"PLUGIN LOAD", fullPath.c_str());

                    // If the plugin exports InterposerPluginInit, call it with
                    // our own module so it can resolve exports without guessing
                    // the DLL name (which varies by deployment: .dll, version.dll,
                    // dinput8.dll, .asi).
                    auto pfnInit = reinterpret_cast<FnInterposerPluginInit>(
                        platform::FindSymbol(module, "InterposerPluginInit"));
                    if (pfnInit)
                        pfnInit(self);
                }
                else
                {
                    LogPluginEvent(L"PLUGIN ERROR", (fullPath + L"  (" + error + L")").c_str());
                }
            }
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
        platform::UnloadModule(*it);

    g_plugins.clear();
}
