---
sidebar_label: Overview
sidebar_position: 1
---

# Plugins

The Interposer supports plugins — DLL or ASI files placed in `.interposer\Plugins\` next to the main DLL. Plugins are loaded automatically after all built-in hooks are active, so they can rely on the full Interposer API from the moment they initialise.

## How Plugins Are Loaded

On startup, after installing and enabling all built-in hooks, the Interposer scans `.interposer\Plugins\` (and all subdirectories) for files matching `*.dll` and `*.asi` and loads each one with `LoadLibrary`.

After loading, the Interposer checks whether the plugin exports a function called `InterposerPluginInit`. If it does, the Interposer calls it immediately, passing its own `HMODULE`:

```cpp
extern "C" __declspec(dllexport) void WINAPI InterposerPluginInit(HMODULE hInterposer);
```

This `hInterposer` handle is all a plugin needs to resolve every Interposer export via `GetProcAddress` — no need to guess the DLL name (which varies between deployment variants).

Plugins are unloaded in reverse order when the process exits.

### Logging

Enable `Logging.Plugins` in `Config.yml` to see plugin lifecycle events:

```
2025-03-14 12:00:01  [PLUGIN LOAD]     C:\Games\MyGame\.interposer\Plugins\CDKey.dll
2025-03-14 12:00:01  [PLUGIN CONFIG]   Registered defaults for CDKey
```

If a plugin fails to load, the error is logged with the Win32 error code and the remaining plugins continue loading:

```
2025-03-14 12:00:01  [PLUGIN ERROR]    C:\Games\MyGame\.interposer\Plugins\broken.dll  (error 126)
```

## Available Plugins

| Plugin | Description |
|---|---|
| [CD Key](CDKey) | Generates a deterministic CD key from the player username and injects it into a registry value |

## Writing Your Own Plugin

See [Creating a Plugin](CreatingPlugins) for the full API reference and a worked example. The `LANCommander.Interposer.Plugin.Example` project in the solution is a minimal, buildable starting point.
