# LANCommander.Interposer

.NET bindings for [LANCommander Interposer](https://github.com/LANCommander/LANCommander.Interposer) - a Windows DLL that hooks into game processes to provide virtual registry, file redirection, DNS redirection, identity overrides, FastDL, and Discord rich presence for LAN gaming.

Everything goes through `InterposerService`:

- **Injection** - Launch processes with `ProcessStartInfo` or inject into running `Process` instances, with optional runtime overrides for username, computer name, and FastDL URL.
- **Plugin API** - Logging, config, virtual registry, and rich presence (in-process only).
- **Hook Events** - Subscribe to file, registry, DNS, network, and identity events (in-process only).

## Launch and Inject

```csharp
using System.Diagnostics;
using LANCommander.Interposer;

using var interposer = new InterposerService();
interposer.Username = "Player1";
interposer.ComputerName = "GAMEPC";
interposer.FastDlUrl = "http://fastdl.lan/";

// Launch suspended, inject the DLL, resume - returns a standard Process
var startInfo = new ProcessStartInfo(@"C:\Games\game.exe", "-fullscreen");
Process game = interposer.Start(startInfo);

// Or inject into an already-running process
interposer.Inject(existingProcess);
interposer.Inject(1234);  // by PID
```

The DLL is located automatically next to the application (`interposer.dll` or `LANCommander.Interposer.dll`). Pass an explicit path to override:

```csharp
interposer.Start(startInfo, dllPath: @"C:\Tools\LANCommander.Interposer.dll");
```

## Plugin API (in-process)

These methods are only callable from within a process that has the Interposer DLL loaded.

```csharp
using var interposer = new InterposerService();

interposer.Log("[MANAGED]      ", "Hello from .NET!");

string setting = interposer.GetConfigString("Plugins.MyPlugin.ApiKey");
interposer.RegisterPluginConfig("MyPlugin", "ApiKey: ''\nEnabled: true");

string username = interposer.GetUsername();

interposer.SetRegistryValue(
    @"HKEY_LOCAL_MACHINE\SOFTWARE\MyGame", "CDKey", "ABCD-1234-EFGH");

interposer.SetPresenceName("My Game");
interposer.SetPresenceDetails("Playing on de_dust2");
interposer.UpdatePresence();
```

## Hook Events (in-process)

Subscribe to hook events from within an injected process. Events fire on the game's thread - subscribers must be thread-safe.

```csharp
using LANCommander.Interposer;
using LANCommander.Interposer.Events;

using var interposer = new InterposerService();
interposer.EnableEvents();

interposer.FileAccessed += (s, e) =>
    Console.WriteLine($"{e.Verb}: {e.Path}");
interposer.RegistryAccessed += (s, e) =>
    Console.WriteLine($"{e.Verb}: {e.KeyPath} -> {e.ValueName}");
interposer.DnsResolved += (s, e) =>
    Console.WriteLine($"DNS: {e.Hostname} -> {e.RedirectedHostname}");
interposer.NetworkConnection += (s, e) =>
    Console.WriteLine($"Connect: {e.Address}:{e.Port}");
interposer.IdentityQueried += (s, e) =>
    Console.WriteLine($"{e.IdentityType}: {e.Value}");
```

## Requirements

- **Windows** - The Interposer DLL and injection APIs are Windows-only.
- **.NET 8.0 / 9.0 / 10.0** - Targets modern .NET runtimes.
- The native `LANCommander.Interposer.dll` (or proxy variant) must be deployed alongside the target process. Download it from [Releases](https://github.com/LANCommander/LANCommander.Interposer/releases).

## License

[MIT](https://github.com/LANCommander/LANCommander.Interposer/blob/main/LICENSE)
