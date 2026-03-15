# Logging

The Interposer can write a detailed access log showing every file and registry operation it intercepts. This log is useful for diagnosing redirect problems, discovering what paths a game uses, and verifying that virtual registry entries are being served correctly.

## Enabling the Log

Configure logging in the `Logging` section of `.interposer\Config.yml`:

```yaml
Logging:
  Files: true
  Registry: true
  Downloads: true
```

| Key | Type | Default | Description |
|---|---|---|---|
| `Files` | bool | `false` | Log file open and attribute operations. |
| `Registry` | bool | `false` | Log registry open, read, write, and delete operations. |
| `Downloads` | bool | `true` | Log file downloads from FastDL |

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

### FastDL Operations

| Verb | Meaning |
|---|---|
| `[FASTDL]` | A file was checked against or downloaded from the FastDL server. The path shows the URL and the local destination separated by `->`. |

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

## Example Log Output

```
# === Session started 2025-03-14 12:00:00 ===
2025-03-14 12:00:01  [FILE READ]    C:\Games\MyGame\config.cfg
2025-03-14 12:00:01  [FILE REDIRECT]  C:\Games\MyGame\Saves\profile.dat  ->  C:\Users\Pat\AppData\Roaming\MyGame\Saves\profile.dat
2025-03-14 12:00:01  [FILE READ]    C:\Users\Pat\AppData\Roaming\MyGame\Saves\profile.dat
2025-03-14 12:00:02  [REG OPEN]     HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0
2025-03-14 12:00:02  [REG READ]     HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0\PlayerName
2025-03-14 12:00:02  [FASTDL]       http://fastdl.lan/baseq3/maps/q3dm1.bsp  ->  C:\Games\Quake3\.interposer\Downloads\baseq3\maps\q3dm1.bsp
```

## Using Logs to Diagnose Problems

**Finding what paths a game uses**: Enable `Files: true`, run the game briefly, then search the log for paths that look like save directories, config files, or hard-coded installation paths.

**Checking if a redirect fired**: Look for `[FILE REDIRECT]` entries. If you expect a redirect but don't see one, the path did not match your regex pattern — copy the exact path from a `[FILE READ]` or `[FILE ATTR]` entry and test your pattern against it.

**Verifying virtual registry**: Enable `Registry: true`. Registry operations on keys that exist in `.interposer\Registry.reg` are served from memory and will still appear in the log — a `[REG READ]` line confirms the hook is active.

**Registry key paths are uppercased** in the log to normalize comparisons. This is expected behavior.
