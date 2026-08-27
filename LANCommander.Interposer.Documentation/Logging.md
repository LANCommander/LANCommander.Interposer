---
sidebar_position: 3
---

# Logging

The Interposer can write a detailed access log showing every file, registry, network, identity, plugin, and rich presence operation it intercepts. This log is useful for diagnosing redirect problems, discovering what paths a game uses, and verifying that virtual registry entries are being served correctly.

## Enabling the Log

Configure logging in the `Logging` section of `.interposer\Config.yml`:

```yaml
Logging:
  Files: true
  Registry: true
  Downloads: true
  Plugins: false
  Identity: false
  RichPresence: false
  DnsRedirects: true
  Network: false
  Level: Info
```

| Key | Type | Default | Description |
|---|---|---|---|
| `Files` | bool | `false` | Log file open and attribute operations. |
| `Registry` | bool | `false` | Log registry open, read, write, and delete operations. |
| `Downloads` | bool | `true` | Log file downloads from FastDL. |
| `Plugins` | bool | `false` | Log plugin load, unload, error, and config registration events. |
| `Identity` | bool | `false` | Log identity override operations (GetUserName, GetComputerName hooks). |
| `RichPresence` | bool | `false` | Log rich presence field changes, update pushes, and clears. |
| `DnsRedirects` | bool | `true` | Log DNS redirect matches. Enabled by default because DNS redirects are deliberate user-configured actions. |
| `Network` | bool | `false` | Log socket connections and DNS lookups. |
| `DirectInput` | bool | `false` | Log DirectInput object and device creation, enumeration, and interface aliasing. At `Debug` also lists every device an enumeration found and every device the filter hid. |
| `Level` | choice | `Info` | Verbosity within the subsystems enabled above. One of `Info`, `Debug`, `Trace`. |

## Log Level

The per-subsystem flags above decide *which* subsystems log. `Level` decides how much each one says. Unrecognized values fall back to `Info`.

| Level | Adds |
|---|---|
| `Info` | Nothing — access lines only. This is the historical output. |
| `Debug` | Redirect diagnostics: whether a redirect rule fired, and whether a registry key was served from the virtual store or passed through to the real registry. |
| `Trace` | Everything in `Debug`, plus one line per file redirect rule evaluated and rejected. |

Diagnostic lines are gated by *both* settings. `[REDIRECT MISS]` requires `Files: true` **and** `Level: Debug`; the registry diagnostics require `Registry: true` and `Level: Debug`.

:::caution Debug is verbose
At `Debug`, every file operation that does *not* match a redirect rule produces an extra `[REDIRECT MISS]` line. That is the point — it shows you the exact paths a game asks for, so you can write rules against them — but the log grows quickly. Set it back to `Info` once your rules work.
:::

Diagnostic lines are written to the session log only. Unlike the access verbs, they are not delivered to plugins or to the .NET event stream.

Logs are written automatically to `.interposer\Logs\<timestamp>.log` — one file per session, no path configuration required. Each session log begins with a header:

```
# === Session started 2025-03-14 12:00:00 ===
```

## Log Format

Each line follows this structure:

```
YYYY-MM-DD HH:MM:SS  [VERB]  <path>  [->  <redirected-path>]
```

The `->` portion only appears when a path was changed — for example, when a file redirect matches or a FastDL file is served from the overlay cache.

## Log Verbs

### File Operations

| Verb | Meaning |
|---|---|
| `[FILE READ]` | A file was opened for reading via `CreateFileW/A`. |
| `[FILE WRITE]` | A file was opened for writing via `CreateFileW/A`. |
| `[FILE R/W]` | A file was opened for both reading and writing. |
| `[FILE ATTR]` | `GetFileAttributesW/A` was called on a path. |
| `[FILE REDIRECT]` | A file open or attribute query was redirected by a rule. The line shows the original path and the destination path separated by `->`. |
| `[FILE FIND]` | `FindFirstFileW/A` was called on a path. |
| `[DLL LOAD]` | A DLL was loaded via `LoadLibraryW/A` or `LoadLibraryExW/A`. |
| `[FILE OVERLAY]` | A file open was served from the FastDL overlay cache instead of the game directory. |

### Redirect Diagnostics

Only written at `Level: Debug` or higher, and only for a subsystem that is already enabled.

| Verb | Level | Meaning |
|---|---|---|
| `[REDIRECT HIT]` | Debug | A `FileRedirects` rule matched. The line shows the source path and, after the `->`, the 1-based rule number and its pattern. |
| `[REDIRECT MISS]` | Debug | No rule matched this path. The line shows either `no rules configured` or how many rules were evaluated. |
| `[REDIRECT RULE]` | Trace | One line per redirect pattern that was evaluated and rejected, for working out why a regex did not match. |
| `[REG HIT]` | Debug | The key was found in the virtual registry, so the request is served from `Registry.reg`. |
| `[REG MISS]` | Debug | The key was passed through to the real registry, with the reason — `not in virtual space`, `handle not resolvable`, or `virtual key not in store`. |
| `[REG PARTIAL]` | Debug | The key exists in the virtual store but the requested value name does not. The game receives `ERROR_FILE_NOT_FOUND` and there is **no** fallback to the real registry. |

`[REG PARTIAL]` is worth calling out: it is the signature of a `Registry.reg` that has the right key but is missing a value the game reads. At `Info` level this looks like an ordinary successful `[REG READ]`.

### FastDL Operations

| Verb | Meaning |
|---|---|
| `[FASTDL]` | A file was checked against or downloaded from the FastDL server. The path shows the URL and the local destination separated by `->`. |

### Plugin Operations

| Verb | Meaning |
|---|---|
| `[PLUGIN LOAD]` | A plugin DLL or ASI was loaded successfully. |
| `[PLUGIN ERROR]` | A plugin failed to load. The line includes the Win32 error code. |
| `[PLUGIN CONFIG]` | A plugin registered default configuration via `InterposerRegisterPluginConfig`. |

### Identity Operations

| Verb | Meaning |
|---|---|
| `[IDENTITY]` | An identity override was applied or a hooked `GetUserName`/`GetComputerName` call returned the configured override value. |

### Rich Presence Operations

| Verb | Meaning |
|---|---|
| `[RP INIT]` | A rich presence backend connected successfully. |
| `[RP SET]` | A presence field was updated by a plugin (details, state, image, etc.). |
| `[RP UPDATE]` | Presence changes were pushed to all backends. |
| `[RP CLEAR]` | Presence was reset to config defaults. |

### Network Operations

| Verb | Meaning |
|---|---|
| `[CONNECT]` | A socket connected to a remote host. The line shows the host (or IP literal) and the port. |
| `[DNS REDIRECT]` | A `DnsRedirects` rule matched a hostname lookup and substituted a replacement. The line shows the original and substituted hostnames separated by `->`. Gated by `Logging.DnsRedirects` (not `Logging.Network`). |

### Registry Operations

| Verb | Meaning |
|---|---|
| `[REG OPEN]` | A registry key was opened via `RegOpenKeyExW/A`. |
| `[REG CREATE]` | A registry key was opened or created via `RegCreateKeyExW/A`. |
| `[REG READ]` | A registry value was queried via `RegQueryValueExW/A`. |
| `[REG WRITE]` | A registry value was written via `RegSetValueExW/A`. |
| `[REG DELETE]` | A registry key or value was deleted. |
| `[REG ENUM]` | Registry subkeys or values were enumerated. |
| `[REG QUERY]` | Key metadata was queried via `RegQueryInfoKeyW/A`. |

### Always-On Events

These events are always written regardless of logging flags:

| Verb | Meaning |
|---|---|
| `[HOOK INIT]` | A MinHook hook was installed. Shows the module, function name, and status. |

## Example Log Output

```
# === Session started 2025-03-14 12:00:00 ===
2025-03-14 12:00:01  [HOOK INIT]       advapi32!RegOpenKeyExW
2025-03-14 12:00:01  [IDENTITY]        Username override: PlayerOne
2025-03-14 12:00:01  [PLUGIN LOAD]     C:\Games\MyGame\.interposer\Plugins\CDKey.dll
2025-03-14 12:00:01  [PLUGIN CONFIG]   Registered defaults for CDKey
2025-03-14 12:00:01  [FILE READ]       C:\Games\MyGame\config.cfg
2025-03-14 12:00:01  [FILE REDIRECT]   C:\Games\MyGame\Saves\profile.dat  ->  C:\Users\Pat\AppData\Roaming\MyGame\Saves\profile.dat
2025-03-14 12:00:02  [REG OPEN]        HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0
2025-03-14 12:00:02  [REG READ]        HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0\PlayerName
2025-03-14 12:00:02  [DNS REDIRECT]    master.gamespy.com  ->  master.local
2025-03-14 12:00:02  [IDENTITY]        GetUserNameW -> PlayerOne
2025-03-14 12:00:03  [RP SET]          Details: Playing on de_dust2
2025-03-14 12:00:03  [RP UPDATE]       Details=Playing on de_dust2  State=Score: 7 - 3
2025-03-14 12:00:03  [FASTDL]          http://fastdl.lan/baseq3/maps/q3dm1.bsp  ->  C:\Games\Quake3\.interposer\Downloads\baseq3\maps\q3dm1.bsp
```

With `Level: Debug`, the same session additionally shows what the Interposer decided:

```
2025-03-14 12:00:01  [REDIRECT MISS]   C:\Games\MyGame\config.cfg  ->  2 rules, none matched
2025-03-14 12:00:01  [REDIRECT HIT]    C:\Games\MyGame\Saves\profile.dat  ->  rule #1  C:\\Games\\MyGame\\Saves\\(.+)
2025-03-14 12:00:02  [REG HIT]         HKEY_LOCAL_MACHINE\SOFTWARE\MYGAME\1.0  ->  served from virtual store
2025-03-14 12:00:02  [REG PARTIAL]     HKEY_LOCAL_MACHINE\SOFTWARE\MYGAME\1.0  ->  value not in store: RESOLUTION
2025-03-14 12:00:02  [REG MISS]        HKEY_CURRENT_USER\SOFTWARE\OTHERAPP  ->  not in virtual space
```

## Using Logs to Diagnose Problems

**Finding what paths a game uses**: Enable `Files: true`, run the game briefly, then search the log for paths that look like save directories, config files, or hard-coded installation paths.

**Checking if a redirect fired**: Set `Level: Debug` and look for `[REDIRECT HIT]` / `[REDIRECT MISS]`. A hit names the rule number and pattern that matched; a miss shows the exact path that nothing matched, which is what you want to write your next pattern against. At `Trace`, each `[REDIRECT RULE]` line shows a pattern that was tried and rejected for that path.

**Verifying virtual registry**: Enable `Registry: true`. At `Info`, a `[REG READ]` line only confirms the hook is active — it looks the same whether the value came from `.interposer\Registry.reg` or the real registry. Set `Level: Debug` to tell them apart: `[REG HIT]` means the virtual store served it, `[REG MISS]` means it passed through, and `[REG PARTIAL]` means the key was virtual but the value was missing, so the game got `ERROR_FILE_NOT_FOUND`.

**Debugging plugin issues**: Enable `Plugins: true` to see whether plugins loaded, what errors occurred, and whether config registration succeeded.

**Checking identity overrides**: Enable `Identity: true` to confirm that `GetUserName` and `GetComputerName` calls are returning the configured values. The log shows every invocation of the hooked functions.

**Registry key paths are uppercased** in the log to normalize comparisons. This is expected behavior.
