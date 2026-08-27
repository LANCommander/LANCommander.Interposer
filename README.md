# LANCommander Interposer

A Windows DLL that hooks into game processes to provide virtual registry, file redirection, DNS redirection, identity overrides, FastDL content delivery, Discord rich presence, and a plugin system. Designed for LAN gaming scenarios where games need runtime patching without modifying files on disk.

## Features

- **Virtual Registry** - Maintain an in-memory registry backed by a `.reg` file. Games read/write virtual keys transparently; real registry is never modified.
- **File Redirection** - Redirect file paths at the `CreateFile`/`GetFileAttributes` level using regex rules. Relocate saves, configs, or assets without touching the game.
- **DNS Redirection** - Redirect hostname lookups (Winsock 1 & 2) to point games at private master servers or community replacements.
- **Identity Overrides** - Override `GetUserNameW/A` and `GetComputerNameW/A` so each player gets a unique identity without renaming their Windows account.
- **FastDL** - Automatically download missing game files from an HTTP server on first access. Supports auto-discovery by probing game server addresses.
- **Rich Presence** - Push game activity to Discord via local IPC. Configurable from `Config.yml` or at runtime from plugins.
- **DirectInput** - Serve DirectInput 3/7 from `dinput8.dll`, working around the record-array overrun in the legacy Windows `dinput.dll` that makes affected games hang or crash on startup. Filter enumerated devices by class or name so a game binds to the right one.
- **Plugin System** - Drop custom DLLs into `.interposer\Plugins\` to extend functionality. Plugins resolve Interposer exports at runtime (no link-time dependency). Ships with a CD key generator and a mouse smoothing/scaling plugin.

## Quick Start

1. Download a release ZIP from the [Releases](https://github.com/LANCommander/LANCommander.Interposer/releases) page.
2. Place the DLL (or `version.dll` proxy) and the `.interposer\` directory next to the game executable.
3. Edit `.interposer\Config.yml` to configure features.
4. Launch the game - the Interposer loads automatically (proxy) or via the injector CLI.

## Release Variants

Each release attaches two artifacts: `LANCommander.Interposer.<version>.zip` for manual
deployment, and `LANCommander.Interposer.<version>.lcx` for import into LANCommander.

The ZIP holds an `x64\` and an `x86\` directory. Within each, the Interposer is present
under every name it can be loaded as — all byte-identical, because one binary serves every
load method and works out which system DLL it is standing in for from the name it was
given:

| File | How it loads |
|---------|-------------|
| `version.dll` | Auto-loaded by Windows when placed next to the game EXE. Start here. |
| `dinput8.dll` | For games that import DirectInput 8, or that ship their own `version.dll` |
| `dinput.dll` | For DirectInput 3/7 era games, which often import nothing else that can be proxied |
| `LANCommander.Interposer.asi` | Loaded by ASI loaders (Ultimate ASI Loader, ScriptHookV, etc.) |
| `LANCommander.Interposer.dll` | Injected via `LANCommander.Interposer.Injector.exe` or programmatically |

Copy one of them plus the `.interposer\` directory next to the game executable. The
archive's `README.md` walks through it.

## Injector CLI

```
# Inject into a running process
Injector.exe game.exe
Injector.exe 1234                          # by PID

# Launch and inject before the game runs
Injector.exe --launch "C:\Games\game.exe"
Injector.exe --launch "C:\Games\game.exe" -fullscreen -- interposer.dll

# Runtime overrides (passed via named memory-mapped files)
Injector.exe --username Player1 --computername GAMEPC --launch "C:\Games\game.exe"
Injector.exe --fastdl-url http://fastdl.lan/ --launch "C:\Games\game.exe"
```

## .NET Bindings

The `LANCommander.Interposer` NuGet package provides managed bindings for the Interposer.

```
dotnet add package LANCommander.Interposer
```

### Launch and inject with ProcessStartInfo

```csharp
using System.Diagnostics;
using LANCommander.Interposer;

using var interposer = new InterposerService();
interposer.Username = "Player1";
interposer.ComputerName = "GAMEPC";
interposer.FastDlUrl = "http://fastdl.lan/";

// Launch a process with the Interposer DLL injected before it runs
var startInfo = new ProcessStartInfo(@"C:\Games\game.exe", "-fullscreen");
Process game = interposer.Start(startInfo);

// Or inject into an already-running process
interposer.Inject(existingProcess);
interposer.Inject(1234);  // by PID
```

### In-process plugin API and hook events

```csharp
using LANCommander.Interposer;

using var interposer = new InterposerService();

// Enable hook event callbacks (in-process only)
interposer.EnableEvents();
interposer.RegistryAccessed += (s, e) =>
    Console.WriteLine($"{e.Verb}: {e.KeyPath}");
interposer.FileAccessed += (s, e) =>
    Console.WriteLine($"{e.Verb}: {e.Path}");

// Logging, config, virtual registry, rich presence
interposer.Log("[MYPLUGIN]     ", "Hello from managed code!");
string value = interposer.GetConfigString("Plugins.MyPlugin.Setting");
interposer.SetRegistryValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\MyGame", "CDKey", "ABCD-1234");
interposer.SetPresenceName("My Game");
interposer.UpdatePresence();
```

## Configuration

All configuration lives in `.interposer\Config.yml` next to the DLL. See the [sample Config.yml](LANCommander.Interposer/.interposer/Config.yml) for the full reference with inline documentation.

### Key sections

| Section | Purpose |
|---------|---------|
| `Player` | Username and computer name overrides |
| `Logging` | Toggle per-subsystem logging (files, registry, network, etc.) |
| `FileRedirects` | Regex-based file path redirection rules |
| `DnsRedirects` | Regex-based DNS hostname redirection rules |
| `DirectInput` | Legacy enumeration fix and device filtering by class or name |
| `FastDL` | HTTP content delivery with auto-discovery |
| `RichPresence` | Discord activity display with configurable defaults |
| `Plugins` | Arbitrary key-value config for plugins |

## Building from Source

### Prerequisites

- Visual Studio 2022 with C++ desktop workload
- [vcpkg](https://vcpkg.io/) (for MinHook and yaml-cpp)
- .NET SDK 8.0+ (for the .NET bindings)

### Native DLL + Injector

```bash
# Install dependencies
vcpkg install --triplet x64-windows-static-md --overlay-triplets=triplets --x-install-root=vcpkg_installed
vcpkg install --triplet x86-windows-static-md --overlay-triplets=triplets --x-install-root=vcpkg_installed_x86

# Build (from Git Bash - use double-slash for MSBuild flags)
MSBuild LANCommander.Interposer.slnx //p:Configuration=Release //p:Platform=x64
```

### .NET Bindings

```bash
dotnet build LANCommander.Interposer.NET/LANCommander.Interposer.NET.csproj -c Release
```

### Tests

```bash
MSBuild LANCommander.Interposer.slnx //p:Configuration=Debug //p:Platform=x64
x64/Debug/LANCommander.Interposer.Tests.exe
```

## Runtime File Structure

```
GameDirectory/
  game.exe
  version.dll (or LANCommander.Interposer.dll)
  .interposer/
    Config.yml          # Settings and redirect rules
    Registry.reg        # Virtual registry state (auto-updated on writes)
    Logs/
      <timestamp>.log   # Per-session log (auto-created)
    Downloads/          # FastDL cache (auto-created)
    Plugins/            # Custom plugin DLLs
      CDKey.dll
```

## Plugin Development

Plugins are native DLLs that export `InterposerPluginInit(HMODULE hInterposer)`. They resolve Interposer functions at runtime via `GetProcAddress`:

```cpp
extern "C" __declspec(dllexport) void InterposerPluginInit(HMODULE hInterposer)
{
    auto Log = (void(*)(const wchar_t*, const wchar_t*))
        GetProcAddress(hInterposer, "InterposerLog");

    Log(L"[MYPLUGIN]     ", L"Plugin loaded!");
}
```

See [LANCommander.Interposer.Plugin.Example](LANCommander.Interposer.Plugin.Example/) for a minimal template and [LANCommander.Interposer.Plugin.CDKey](LANCommander.Interposer.Plugin.CDKey/) for a real-world example.

## License

[MIT](LICENSE) - Copyright (c) 2024-2026 LANCommander Contributors
