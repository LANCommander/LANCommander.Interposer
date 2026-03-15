---
sidebar_label: Overview
sidebar_position: 4
---

# File Redirection

File redirection intercepts calls to `CreateFileW/A`, `GetFileAttributesW/A`, `FindFirstFileW/A`, and the `LoadLibrary` family, replacing path arguments on the fly before the call reaches Windows. The game opens and reads the redirected file without any knowledge that its path was changed.

## Why Use It

Common use cases:

- **Portable save files** — redirect a hard-coded save path (e.g. `C:\Program Files\MyGame\saves\`) to a per-user location (`%APPDATA%\MyGame\saves\`)
- **Config file portability** — redirect absolute paths baked into old games to locations inside the game directory
- **Multi-user coexistence** — different users can be redirected to different profile directories without modifying the game
- **Registry-adjacent paths** — some games write config to hard-coded paths under `C:\Windows` or `C:\Program Files`; redirect these to writable locations

## Configuring Redirects

Redirects are defined as a list in `.interposer/Config.yml` under the `Redirects` key. Each entry has a `Pattern` (regex) and a `Replacement`:

```yaml
Redirects:
  - Pattern: 'C:\\Games\\MyGame\\Saves\\(.+)'
    Replacement: '%APPDATA%\MyGame\Saves\$1'
```

:::tip Use single-quoted strings for patterns
YAML single-quoted strings pass backslashes through literally — no extra escaping needed when writing Windows paths as regex patterns. Double-quoted strings interpret YAML escape sequences and should be avoided here.
:::

Rules are evaluated in order. **The first matching rule wins** — subsequent rules are not checked once a match is found.

## Pattern Syntax

Patterns are [ECMAScript regular expressions](https://en.cppreference.com/w/cpp/regex/ecmascript), matched case-insensitively against the full file path.

In a single-quoted YAML string, each `\\` represents one literal backslash character, which the regex engine then treats as a literal backslash (matching `\` in a Windows path). To match a directory separator, write `\\\\` in a double-quoted string or `\\` in a single-quoted string.

| You want to match | Single-quoted YAML | Regex sees |
|---|---|---|
| A literal backslash | `'\\'` | `\\` (matches `\`) |
| `C:\Games\MyGame\` | `'C:\\Games\\MyGame\\'` | `C:\\Games\\MyGame\\` |
| Any characters | `'(.+)'` | `(.+)` |
| A literal dot | `'\.'` | `\.` |

### Capture Groups

Use parentheses to capture parts of the matched path for use in the replacement:

```yaml
- Pattern: 'C:\\Games\\MyGame\\Saves\\(.+)'
  Replacement: '%APPDATA%\MyGame\Saves\$1'
```

For the path `C:\Games\MyGame\Saves\profile.dat`:
- `$1` captures `profile.dat`
- The replacement expands to `%APPDATA%\MyGame\Saves\profile.dat` (with `%APPDATA%` further expanded)

Up to nine capture groups (`$1` through `$9`) are supported.

## Replacement Syntax

| Token | Meaning |
|---|---|
| `$1` – `$9` | Replaced with the corresponding capture group from the pattern match. |
| `%VARNAME%` | Expanded to the value of the named Windows environment variable after capture group substitution. |

Environment variable expansion happens after capture group substitution, so a capture group can itself contain a variable reference if needed.

## Examples

### Redirect a save directory

```yaml
Redirects:
  - Pattern: 'C:\\Games\\Quake\\id1\\save\\(.+)'
    Replacement: '%APPDATA%\Quake\save\$1'
```

### Redirect an absolute config path to the game directory

```yaml
Redirects:
  - Pattern: 'C:\\Program Files.*\\MyGame\\config\.cfg'
    Replacement: '%GAMEDIR%\config.cfg'
```

### Redirect multiple directories with one rule

This example redirects any file under `C:\OldGame\data\` or `C:\OldGame\mods\` to corresponding locations under `%APPDATA%\OldGame\`:

```yaml
FileRedirects:
  - Pattern: 'C:\\OldGame\\(data|mods)\\(.+)'
    Replacement: '%APPDATA%\OldGame\$1\$2'
```

### Multiple rules — first match wins

```yaml
Redirects:
  - Pattern: 'C:\\Games\\MyGame\\Saves\\current\\(.+)'
    Replacement: '%APPDATA%\MyGame\Saves\slot1\$1'
  - Pattern: 'C:\\Games\\MyGame\\Saves\\(.+)'
    Replacement: '%APPDATA%\MyGame\Saves\$1'
```

The first rule redirects any path under `current\` specifically; the second catches everything else under `Saves\`.

## Verifying Redirects

Enable file logging and look for `[FILE REDIRECT]` entries:

```
2025-03-14 12:00:01  [FILE REDIRECT]  C:\Games\MyGame\Saves\profile.dat  ->  C:\Users\Pat\AppData\Roaming\MyGame\Saves\profile.dat
```

If a redirect is not firing, check the exact path in `[FILE READ]` or `[FILE ATTR]` entries and compare it against your pattern.

## Hooked Functions

File redirection applies to the following Windows API functions:

| Function | Notes |
|---|---|
| `CreateFileW` / `CreateFileA` | Applies redirect before opening; ANSI variant converts to wide first. |
| `GetFileAttributesW` / `GetFileAttributesA` | Applies redirect before querying attributes. |
| `FindFirstFileW` / `FindFirstFileA` | Applies redirect before beginning enumeration. |
| `LoadLibraryW` / `LoadLibraryA` | Applies redirect before loading a DLL. |
| `LoadLibraryExW` / `LoadLibraryExA` | Applies redirect before loading a DLL with flags. |
