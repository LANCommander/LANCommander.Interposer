# LANCommander.Interposer

.NET bindings for [LANCommander Interposer](https://github.com/LANCommander/LANCommander.Interposer) - a Windows DLL that hooks into game processes to provide virtual registry, file redirection, DNS redirection, identity overrides, FastDL, DirectInput fixes, OS version spoofing, and Discord rich presence for LAN gaming.

Everything goes through `InterposerService`:

- **Injection** - Launch processes with `ProcessStartInfo` or inject into running `Process` instances, with optional runtime overrides for username, computer name, and FastDL URL.
- **Plugin API** - Logging, config, virtual registry, and rich presence (in-process only).
- **Hook Events** - Subscribe to file, registry, DNS, network, and identity events (in-process only).
- **Network Relay** - Put a game on a virtual LAN through a LANCommander relay server, and drive it from the launcher while it runs.

Everything else the DLL does is driven by `.interposer\Config.yml` next to the DLL and needs no managed calls - see [Configuration](#configuration) below.

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
interposer.SetConfigString("Plugins.MyPlugin.LastServer", "192.168.1.10");
interposer.RegisterPluginConfig("MyPlugin", "ApiKey: ''\nEnabled: true");

string username = interposer.GetUsername();

interposer.SetRegistryValue(
    @"HKEY_LOCAL_MACHINE\SOFTWARE\MyGame", "CDKey", "ABCD-1234-EFGH");

interposer.SetPresenceName("My Game");
interposer.SetPresenceDetails("Playing on de_dust2");
interposer.UpdatePresence();
```

## Configuration

The DLL reads `.interposer\Config.yml` from its own directory at load time. Redirection rules, DirectInput fixes and the reported OS version are configured there, not through this API - injecting the DLL is enough to apply them.

```yaml
# Report Windows XP SP3 to a game whose version switch has no case past 5.x
OsVersion:
  Version: WindowsXP

# Move a save folder. %TOKEN% resolves environment variables first, then known
# folders Windows tracks but the environment does not (%SAVEDGAMES%, %DOCUMENTS%,
# %MYGAMES%, ...), then %GAMEDIR% and %INTERPOSERDIR%. Tokens work on both sides.
FileRedirects:
  - Pattern: '%APPDATA%\\My Game\\(.+)'
    Replacement: '%SAVEDGAMES%\My Game\$1'
```

The substituted value of a token in a `Pattern` is regex-escaped automatically, so never double the backslashes inside a token - only separators you write yourself need `\\`.

Read any scalar back from inside an injected process:

```csharp
string osVersion = interposer.GetConfigString("OsVersion.Version");  // "WindowsXP"
string username = interposer.GetConfigString("Player.Username");
```

`GetConfigString` resolves the path against the whole document, so it is not limited to the `Plugins` section. Sequence-valued keys such as `FileRedirects` are not scalar and return `null`.

See the [OS Version](https://github.com/LANCommander/LANCommander.Interposer/blob/main/LANCommander.Interposer.Documentation/OsVersion.md) and [File Redirection](https://github.com/LANCommander/LANCommander.Interposer/blob/main/LANCommander.Interposer.Documentation/FileRedirection.md) docs for the full option reference.

## Network Relay

The DLL can carry a game's LAN traffic - broadcasts, datagrams, TCP connections and emulated IPX - through a relay server, giving each player a virtual LAN address instead of a VPN. A launcher passes the session's options at launch, overriding `NetworkRelay` in Config.yml key by key, and controls the relay of the running game over a named pipe:

```csharp
using var interposer = new InterposerService
{
    Relay = new RelayOptions
    {
        Enabled = true,
        Server  = "lancommander.example.com",
        Network = sessionId,
        Ticket  = relayTicket,
    },
};

interposer.RelayStateChanged += (s, e) => Console.WriteLine($"Relay {e.State} as {e.VirtualAddress}");
interposer.RelayPeerJoined   += (s, e) => Console.WriteLine($"{e.Name} joined at {e.VirtualAddress}");
interposer.RelayPeerLeft     += (s, e) => Console.WriteLine($"{e.Name} left");

Process game = interposer.Start(new ProcessStartInfo(@"C:\Games\game.exe"));

RelayStatus status = await interposer.GetRelayStatusAsync();
await interposer.ConnectRelayAsync(new RelayOptions { Network = "another-lobby" });
await interposer.DisconnectRelayAsync();
```

Setting `Relay` at all - even with `Enabled = false` - makes the DLL install its socket hooks, which it can only do at startup; set it whenever the launcher may connect the game later. The relay events also arrive over the event pipe, so they need no `EnableEvents`. Inside the game process, `GetRelayStatus()`, `ConnectRelay()` and `DisconnectRelay()` call the DLL directly.

The server side is the separate `LANCommander.Interposer.Relay` library. See the [Network Relay](https://github.com/LANCommander/LANCommander.Interposer/blob/main/LANCommander.Interposer.Documentation/NetworkRelay.md) docs.

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

`FileEventArgs.SecondaryPath` carries the fully resolved redirect target for `FILE REDIRECT` - capture groups and `%TOKEN%` references are already expanded. The redirect diagnostics emitted at `Logging.Level` `Debug` and `Trace` (`REDIRECT HIT`, `REDIRECT MISS`, `REDIRECT RULE`) go to the session log only and never reach this event stream.

## Requirements

- **Windows** - The Interposer DLL and injection APIs are Windows-only.
- **.NET 8.0 / 9.0 / 10.0** - Targets modern .NET runtimes.
- The native `LANCommander.Interposer.dll` (or proxy variant) must be deployed alongside the target process. Download it from [Releases](https://github.com/LANCommander/LANCommander.Interposer/releases).

## License

[MIT](https://github.com/LANCommander/LANCommander.Interposer/blob/main/LICENSE)
