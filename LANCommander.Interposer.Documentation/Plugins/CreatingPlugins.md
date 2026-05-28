---
sidebar_label: Creating a Plugin
sidebar_position: 2
---

# Creating a Plugin

A plugin is a standard Windows DLL (`.dll`) or ASI file (`.asi`) placed in `.interposer\Plugins\`. It has no link-time dependency on the Interposer — all API functions are resolved at runtime via `GetProcAddress`.

## Project Setup

Create a new DLL project targeting the same architecture as the game (x86 for 32-bit games, x64 for 64-bit games). No additional libraries or headers are required beyond the Windows SDK.

## Plugin Entry Point

The Interposer calls an optional exported function on each plugin immediately after `LoadLibrary`:

```cpp
extern "C" __declspec(dllexport) void WINAPI InterposerPluginInit(HMODULE hInterposer);
```

The `hInterposer` parameter is the Interposer's own module handle. Use it for all `GetProcAddress` calls — this works regardless of whether the Interposer was deployed as `LANCommander.Interposer.dll`, `version.dll`, `dinput8.dll`, or an `.asi` file.

Keep `DllMain` minimal — heavy initialization belongs in `InterposerPluginInit` where the full Interposer API is available:

```cpp
BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD /*fdwReason*/, LPVOID /*lpReserved*/)
{
    return TRUE;
}
```

## Resolving the API

Declare function pointer types for the Interposer exports you need and resolve them with `GetProcAddress` using the `hInterposer` handle:

```cpp
using FnInterposerLog                  = void (WINAPI*)(const wchar_t* verb, const wchar_t* message);
using FnInterposerGetConfigString      = BOOL (WINAPI*)(const wchar_t* dotPath, wchar_t* buf, DWORD bufSize);
using FnInterposerRegisterPluginConfig = BOOL (WINAPI*)(const wchar_t* pluginName, const wchar_t* yamlDefaults);

static FnInterposerLog                  pfnLog       = nullptr;
static FnInterposerGetConfigString      pfnGetConfig = nullptr;
static FnInterposerRegisterPluginConfig pfnRegConfig = nullptr;

static bool ResolveAPI(HMODULE hInterposer)
{
    pfnLog       = (FnInterposerLog)                 GetProcAddress(hInterposer, "InterposerLog");
    pfnGetConfig = (FnInterposerGetConfigString)     GetProcAddress(hInterposer, "InterposerGetConfigString");
    pfnRegConfig = (FnInterposerRegisterPluginConfig)GetProcAddress(hInterposer, "InterposerRegisterPluginConfig");

    return pfnLog && pfnGetConfig;
}
```

## API Reference

All exported functions use the `WINAPI` (`__stdcall`) calling convention and undecorated `extern "C"` names.

### `InterposerRegisterPluginConfig`

```cpp
BOOL InterposerRegisterPluginConfig(const wchar_t* pluginName, const wchar_t* yamlDefaults);
```

Register default configuration for the plugin. `pluginName` is the key under `Plugins:` in `Config.yml` (e.g. `L"MyPlugin"`). `yamlDefaults` is a YAML map body defining default keys and values.

If a `Plugins.<pluginName>` section already exists in `Config.yml`, the call is a no-op — user configuration is never overwritten. Otherwise the defaults are merged into the in-memory config (immediately queryable via `InterposerGetConfigString`) and appended to `Config.yml` on disk.

```cpp
if (pfnRegConfig)
{
    pfnRegConfig(L"MyPlugin",
        L"Greeting: 'Hello!'\n"
        L"Count: 42\n"
        L"Enabled: true");
}
```

After this call, `pfnGetConfig(L"Plugins.MyPlugin.Greeting", ...)` returns `"Hello!"` even if the user has never touched `Config.yml`. On the first run, `Config.yml` is updated to include:

```yaml
Plugins:
  MyPlugin:
    Greeting: Hello!
    Count: 42
    Enabled: true
```

Returns `TRUE` on success (or if the section already exists). Returns `FALSE` on error (bad YAML, file write failure).

:::tip
Call `InterposerRegisterPluginConfig` before reading any config values — it ensures defaults are always available.
:::

---

### `InterposerLog`

```cpp
void InterposerLog(const wchar_t* verb, const wchar_t* message);
```

Writes a line to the session log regardless of the `Logging` flags in `Config.yml`. The log line format matches the rest of the session log:

```
YYYY-MM-DD HH:MM:SS  [VERB]             <message>
```

`verb` is normalised automatically: any existing `[`/`]` brackets and surrounding whitespace are stripped, the content is truncated to 16 characters, and it is re-wrapped as `[verb]` right-padded to 18 characters. Pass a plain string such as `L"MYPLUGIN"` — no manual padding required.

---

### `InterposerGetConfigString`

```cpp
BOOL InterposerGetConfigString(const wchar_t* dotPath, wchar_t* buffer, DWORD bufferSize);
```

Reads a scalar value from `Config.yml` by dot-separated YAML path. Returns `TRUE` on success, `FALSE` if the key does not exist, is not a scalar, or the buffer is too small.

`bufferSize` is in `wchar_t` units and must include room for the null terminator.

```cpp
wchar_t setting[256];
if (pfnGetConfig(L"Plugins.MyPlugin.Setting", setting, ARRAYSIZE(setting)))
{
    // use setting
}
```

Plugin configuration should live under a `Plugins.<PluginName>` namespace in `Config.yml` to avoid collisions:

```yaml
Plugins:
  MyPlugin:
    Setting: hello
    Count: 42
```

---

### `InterposerGetUsername`

```cpp
BOOL InterposerGetUsername(wchar_t* buffer, DWORD bufferSize);
```

Returns the effective player username: the value configured in `Config.yml` under `Player.Username` or passed via the `--username` injector flag. Falls back to the real Windows account name (`GetUserNameW`) if no override is configured.

`bufferSize` is in `wchar_t` units including the null terminator. Returns `TRUE` on success.

---

### `InterposerSetRegistryValue`

```cpp
void InterposerSetRegistryValue(const wchar_t* keyPath, const wchar_t* valueName, const wchar_t* value);
```

Injects a `REG_SZ` string value into the in-memory virtual registry store. Subsequent `RegQueryValueEx` calls for `keyPath\valueName` return `value` without touching the real registry. The injection is transient — it is not persisted to `.interposer\Registry.reg`.

`keyPath` must be a full path beginning with a hive name:

```
HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0
```

Set `valueName` to `L"@"`, `L""`, or `nullptr` to target the default (unnamed) registry value — the entry shown as `(Default)` in Registry Editor.

:::note
The target key must already exist in `.interposer\Registry.reg` for reads to be intercepted. Add an empty key header if no values need to be pre-populated:

```
[HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0]
```
:::

---

### `InterposerSetRegistryValueBySuffix`

```cpp
DWORD InterposerSetRegistryValueBySuffix(const wchar_t* keySuffix, const wchar_t* valueName, const wchar_t* value);
```

Like `InterposerSetRegistryValue`, but matches by suffix rather than exact path. Any key in the virtual store whose path ends with `\keySuffix` (matched case-insensitively on a backslash component boundary) receives the injected value.

Returns the number of keys updated. A return value of `0` means the suffix matched nothing in the virtual store — check that the target key is present in `.interposer\Registry.reg`.

This is useful when the full registry path varies between game versions or installations:

```cpp
// Matches HKEY_LOCAL_MACHINE\...\Electronic Arts\EA Games\Battlefield 1942\ergc
// regardless of any intermediate path components.
pfnSetBySuffix(L"Battlefield 1942\\ergc", L"@", generatedKey);
```

## Minimal Example

```cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

using FnInterposerLog                  = void (WINAPI*)(const wchar_t*, const wchar_t*);
using FnInterposerGetConfigString      = BOOL (WINAPI*)(const wchar_t*, wchar_t*, DWORD);
using FnInterposerRegisterPluginConfig = BOOL (WINAPI*)(const wchar_t*, const wchar_t*);

static FnInterposerLog                  pfnLog       = nullptr;
static FnInterposerGetConfigString      pfnGetConfig = nullptr;
static FnInterposerRegisterPluginConfig pfnRegConfig = nullptr;

extern "C" __declspec(dllexport) void WINAPI InterposerPluginInit(HMODULE hInterposer)
{
    pfnLog       = (FnInterposerLog)                 GetProcAddress(hInterposer, "InterposerLog");
    pfnGetConfig = (FnInterposerGetConfigString)     GetProcAddress(hInterposer, "InterposerGetConfigString");
    pfnRegConfig = (FnInterposerRegisterPluginConfig)GetProcAddress(hInterposer, "InterposerRegisterPluginConfig");

    if (!pfnLog || !pfnGetConfig) return;

    // Register defaults — written to Config.yml on first run only.
    if (pfnRegConfig)
        pfnRegConfig(L"MyPlugin", L"Greeting: 'Plugin loaded successfully'");

    // Read config (defaults are immediately available after registration).
    wchar_t greeting[256] = {};
    pfnGetConfig(L"Plugins.MyPlugin.Greeting", greeting, ARRAYSIZE(greeting));

    pfnLog(L"MYPLUGIN", greeting);
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
```

```yaml
# After first run, Config.yml automatically contains:
Plugins:
  MyPlugin:
    Greeting: Plugin loaded successfully
```
