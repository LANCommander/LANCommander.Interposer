---
sidebar_label: File Redirection
sidebar_position: 4
---

# File Redirection

File redirection intercepts calls to `CreateFileW/A`, `GetFileAttributesW/A`, `FindFirstFileW/A`, and the `LoadLibrary` family, replacing path arguments on the fly before the call reaches Windows. The game opens and reads the redirected file without any knowledge that its path was changed.

## Why Use It

Common use cases:

- **Portable save files** — redirect a hard-coded save path (e.g. `C:\Program Files\MyGame\saves\`) to a per-user location (`%USERPROFILE%\Saved Games\My Game\`)
- **Config file portability** — redirect absolute paths baked into old games to locations inside the game directory
- **Multi-user coexistence** — different users can be redirected to different profile directories without modifying the game
- **Registry-adjacent paths** — some games write config to hard-coded paths under `C:\Windows` or `C:\Program Files`; redirect these to writable locations

## Configuring Redirects

Redirects are defined as a list in `.interposer/Config.yml` under the `FileRedirects` key. Each entry has a `Pattern` (regex) and a `Replacement`:

```yaml
FileRedirects:
  - Pattern: 'C:\\Games\\MyGame\\Saves\\(.+)'
    Replacement: '%USERPROFILE%\Saved Games\My Game\$1'
```

:::note Backwards compatibility
The legacy key name `Redirects` is still accepted and behaves identically. It is read only when `FileRedirects` is not present, so prefer `FileRedirects` for new configs.
:::

:::tip Use single-quoted strings for patterns
YAML single-quoted strings pass backslashes through literally. No extra escaping is needed when writing Windows paths as regex patterns. Double-quoted strings interpret YAML escape sequences and should be avoided here.
:::

Rules are evaluated in order. **The first matching rule wins**. Subsequent rules are not checked once a match is found.

## Pattern Syntax

Patterns are [ECMAScript regular expressions](https://en.cppreference.com/w/cpp/regex/ecmascript), matched case-insensitively against the full file path.

In a single-quoted YAML string, each `\\` represents one literal backslash character, which the regex engine then treats as a literal backslash (matching `\` in a Windows path). To match a directory separator, write `\\\\` in a double-quoted string or `\\` in a single-quoted string.

| You want to match | Single-quoted YAML | Regex sees |
|---|---|---|
| A literal backslash | `'\\'` | `\\` (matches `\`) |
| `C:\Games\MyGame\` | `'C:\\Games\\MyGame\\'` | `C:\\Games\\MyGame\\` |
| Any characters | `'(.+)'` | `(.+)` |
| A literal dot | `'\.'` | `\.` |

Patterns may also contain [path tokens](#path-tokens), which are substituted before
the regex is compiled.

:::warning Tokens do not remove the need to double your own separators
The value a token expands to is regex-escaped for you, so never double the backslashes
*inside* a token. Everything else in the pattern is still your regex, so a separator you
write yourself still needs `\\`:

```yaml
- Pattern: '%APPDATA%\\My Game\\(.+)'   # correct
- Pattern: '%APPDATA%\My Game\(.+)'         # wrong
```

In the second form `\M` is a regex escape, not a directory separator. It compiles
without error and then matches nothing. Check the log at `Level: Debug` if a rule
that looks right never fires.
:::

### Capture Groups

Use parentheses to capture parts of the matched path for use in the replacement:

```yaml
- Pattern: 'C:\\Games\\MyGame\\Saves\\(.+)'
  Replacement: '%USERPROFILE%\Saved Games\My Game\$1'
```

For the path `C:\Games\MyGame\Saves\profile.dat`:
- `$1` captures `profile.dat`
- The replacement expands to `%USERPROFILE%\Saved Games\My Game\profile.dat` (with `%APPDATA%` further expanded)

Up to nine capture groups (`$1` through `$9`) are supported.

## Replacement Syntax

| Token | Meaning |
|---|---|
| `$1` – `$9` | Replaced with the corresponding capture group from the pattern match. |
| `%NAME%` | Expanded as a [path token](#path-tokens) after capture group substitution. |

Token expansion happens after capture group substitution, so a capture group can itself contain a token reference if needed.

## Path Tokens

`%NAME%` is resolved the same way in both `Pattern` and `Replacement`. A name is looked up in this order, and the first hit wins:

| Source | Examples |
|---|---|
| Any Windows environment variable | `%APPDATA%`, `%LOCALAPPDATA%`, `%USERPROFILE%`, `%PROGRAMDATA%`, `%TEMP%` |
| A known folder Windows tracks but the environment does not | `%SAVEDGAMES%`, `%DOCUMENTS%`, `%MYDOCUMENTS%`, `%MYGAMES%`, `%DESKTOP%`, `%PICTURES%`, `%MUSIC%`, `%VIDEOS%` |
| The running executable's directory | `%GAMEDIR%` |
| The directory holding the Interposer DLL | `%INTERPOSERDIR%` |

Because the environment is consulted first, a token that already exists as an environment
variable keeps its existing meaning The known-folder table only covers names the
environment leaves undefined.

Known folders are read from `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders`,
so a relocated Documents or Saved Games folder resolves to wherever it actually lives.

:::note Unresolved tokens are left alone
A token that matches nothing is kept verbatim, delimiters included, rather than expanding
to an empty string. The rule therefore fails to match instead of silently redirecting to a
truncated path, and the name is reported once at startup:

```
2025-03-14 12:00:00  [FILEREDIRECT]  unresolved %MYGAMEDIR% in FileRedirects pattern: %MYGAMEDIR%\\saves\\(.+)
```
:::

## Examples

### Redirect a save directory

```yaml
FileRedirects:
  - Pattern: 'C:\\Games\\Quake\\id1\\save\\(.+)'
    Replacement: '%APPDATA%\Quake\save\$1'
```

### Redirect an absolute config path to the game directory

`%GAMEDIR%` is the directory of the running executable, so this rule keeps working
wherever the game is installed:

```yaml
FileRedirects:
  - Pattern: 'C:\\Program Files.*\\MyGame\\config\.cfg'
    Replacement: '%GAMEDIR%\config.cfg'
```

### Move an AppData save folder into Saved Games

Both sides use tokens, so nothing in the rule is tied to a particular user or drive:

```yaml
FileRedirects:
  - Pattern: '%APPDATA%\\My The Lord of the Rings, The Rise of the Witch-king Files(.*)'
    Replacement: '%SAVEDGAMES%\EA Games\The Lord of the Rings - Battle for Middle-earth II - Rise of the Witch-king$1'
```

Note the `(.*)` with no separator in front of it: that matches the folder itself as well
as everything under it, so the game's initial `GetFileAttributes` probe on the directory
is redirected too. Writing `\\(.+)` instead would redirect only the files inside it.

### Redirect multiple directories with one rule

This example redirects any file under `C:\OldGame\data\` or `C:\OldGame\mods\` to corresponding locations under `%APPDATA%\OldGame\`:

```yaml
FileRedirects:
  - Pattern: 'C:\\OldGame\\(data|mods)\\(.+)'
    Replacement: '%APPDATA%\OldGame\$1\$2'
```

### Multiple rules — first match wins

```yaml
FileRedirects:
  - Pattern: 'C:\\Games\\MyGame\\Saves\\current\\(.+)'
    Replacement: '%USERPROFILE%\Saved Games\My Game\slot1\$1'
  - Pattern: 'C:\\Games\\MyGame\\Saves\\(.+)'
    Replacement: '%USERPROFILE%\Saved Games\My Game\$1'
```

The first rule redirects any path under `current\` specifically; the second catches everything else under `Saves\`.

## Verifying Redirects

Enable file logging and look for `[FILE REDIRECT]` entries:

```
2025-03-14 12:00:01  [FILE REDIRECT]  C:\Games\MyGame\Saves\profile.dat  ->  C:\Users\Pat\AppData\Roaming\MyGame\Saves\profile.dat
```

### Diagnosing a rule that isn't firing

Set `Logging.Level` to `Debug`. Every file operation then reports whether a rule matched:

```
2025-03-14 12:00:01  [REDIRECT HIT]   C:\Games\MyGame\Saves\profile.dat  ->  rule #1  C:\\Games\\MyGame\\Saves\\(.+)
2025-03-14 12:00:01  [REDIRECT MISS]  C:\Games\MyGame\config.cfg  ->  2 rules, none matched
```

A `[REDIRECT MISS]` gives you the exact path the game asked for — copy it and test your pattern against it. A `[REDIRECT HIT]` names the 1-based rule number and its pattern, which is how you confirm that an earlier, broader rule isn't shadowing the one you intended (remember: first match wins).

When a pattern contains a [path token](#path-tokens), the hit line shows the compiled
pattern first and the rule as you wrote it after it, so you can see exactly what the token
expanded to:

```
2025-03-14 12:00:01  [REDIRECT HIT]   C:\Users\Pat\AppData\Roaming\My Game\save.dat  ->  rule #1  C:\\Users\\Pat\\AppData\\Roaming\\My Game\\(.+)  (as written: %APPDATA%\\My Game\\(.+))
```

If the miss says `no rules configured`, the `FileRedirects` block was empty or failed to parse — check for malformed regexes, which are skipped silently at load time.

For a stubborn pattern, `Level: Trace` adds a `[REDIRECT RULE]` line for every pattern that was evaluated and rejected against that path:

```
2025-03-14 12:00:01  [REDIRECT MISS]  C:\Games\MyGame\config.cfg  ->  2 rules, none matched
2025-03-14 12:00:01  [REDIRECT RULE]  C:\Games\MyGame\config.cfg  ->  C:\\Games\\MyGame\\Saves\\(.+)
2025-03-14 12:00:01  [REDIRECT RULE]  C:\Games\MyGame\config.cfg  ->  C:\\Games\\MyGame\\Demos\\(.+)
```

See [Logging](./Logging.md) for the full list of diagnostic verbs.

## Hooked Functions

File redirection applies to the following Windows API functions:

| Function | Notes |
|---|---|
| `CreateFileW` / `CreateFileA` | Applies redirect before opening; ANSI variant converts to wide first. |
| `GetFileAttributesW` / `GetFileAttributesA` | Applies redirect before querying attributes. |
| `FindFirstFileW` / `FindFirstFileA` | Applies redirect before beginning enumeration. |
| `LoadLibraryW` / `LoadLibraryA` | Applies redirect before loading a DLL. |
| `LoadLibraryExW` / `LoadLibraryExA` | Applies redirect before loading a DLL with flags. |
