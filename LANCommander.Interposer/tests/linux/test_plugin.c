/* A plugin, as the harness installs it in .interposer/Plugins/. It resolves the
 * Interposer through the handle InterposerPluginInit receives -- never by name,
 * which varies by deployment -- logs that it ran, registers default config, and
 * leaves a marker file when it is unloaded. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "LANCommander.Interposer.h"

typedef void (*FnLog)(const wchar_t* verb, const wchar_t* message);
typedef BOOL (*FnRegisterPluginConfig)(const wchar_t* name, const wchar_t* yamlDefaults);

__attribute__((visibility("default")))
void InterposerPluginInit(void* interposer)
{
    FnLog log = (FnLog)dlsym(interposer, "InterposerLog");
    FnRegisterPluginConfig registerConfig = (FnRegisterPluginConfig)dlsym(interposer, "InterposerRegisterPluginConfig");

    if (log)
        log(L"TESTPLUGIN", L"initialised through the handle it was given");

    if (registerConfig)
        registerConfig(L"TestPlugin", L"Greeting: hello\n");
}

__attribute__((destructor))
static void TestPluginUnload(void)
{
    const char* marker = getenv("INTERPOSER_TEST_PLUGIN_MARKER");

    if (marker && *marker)
    {
        FILE* f = fopen(marker, "w");

        if (f)
            fclose(f);
    }
}
