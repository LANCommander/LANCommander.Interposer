---
sidebar_position: 6
---

# Registry Emulation

Registry emulation intercepts Windows registry API calls and serves reads and writes from an in-memory store instead of the real Windows registry. By default the store is initialized from a `Registry.reg` file placed inside the `.interposer\` directory next to the DLL, and any writes made by the game are persisted back to that file automatically. [`Registry.Files`](#stacking-several-reg-files) can point somewhere else, or stack several files into one store.

## Why Use It

Many older games write configuration, key bindings, and save state directly to the Windows registry, typically under `HKEY_LOCAL_MACHINE\SOFTWARE\<Publisher>\<Game>` or `HKEY_CURRENT_USER\SOFTWARE\<Game>`. This causes several problems:

- **Administrator privileges required** — writing to `HKEY_LOCAL_MACHINE` requires elevation
- **Not portable** — registry entries don't travel with a game installation
- **Shared between users** — a single set of registry keys is used by all users on the machine
- **Hard to back up or reset** — registry state is scattered and opaque

Registry emulation solves all of these by redirecting those calls to a plain text file.

## Setting Up Registry.reg

Create a file named `Registry.reg` inside the `.interposer\` directory next to the DLL. This file uses the standard Windows `.reg` format:

```
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0]
"PlayerName"="DefaultPlayer"
"MusicVolume"=dword:00000064
"FullScreen"=dword:00000001
```

Only keys that appear in this file are intercepted. Any registry access to a key that is **not** listed in `.interposer\Registry.reg` passes through to the real Windows registry unchanged, unless [`Registry.Isolated`](#isolated-mode) is on, which makes the store authoritative for the whole registry.

### Supported Value Types

| .reg syntax | Registry type | Example |
|---|---|---|
| `"value"` | `REG_SZ` | `"Name"="Player1"` |
| `dword:` | `REG_DWORD` | `"Volume"=dword:00000064` |
| `hex(2):` | `REG_EXPAND_SZ` | `"Path"=hex(2):25,00,41,00,50,00,50,00,44,00,41,00,54,00,41,00,25,00,00,00` |
| `hex(7):` | `REG_MULTI_SZ` | `"List"=hex(7):66,00,6f,00,6f,00,00,00,62,00,61,00,72,00,00,00,00,00` |
| `hex(b):` | `REG_QWORD` | `"BigNumber"=hex(b):01,00,00,00,00,00,00,00` |
| `hex:` | `REG_BINARY` | `"Data"=hex:DE,AD,BE,EF` |

:::tip Finding existing registry values
Run the game once normally (without the Interposer), then use `regedit.exe` to locate the keys it created. Export them with **File → Export** and use that output as your starting `.interposer\Registry.reg`.
:::

## How It Works

When the DLL loads, it reads `.interposer\Registry.reg` into an in-memory store. All registry API calls are intercepted:

- **Reads** (`RegOpenKeyEx`, `RegQueryValueEx`, `RegEnumKeyEx`, etc.) — if the requested key exists in the virtual store, the call is satisfied entirely from memory. The real registry is not accessed.
- **Writes** (`RegSetValueEx`) — values are written to the in-memory store and immediately persisted back to `.interposer\Registry.reg` on disk.
- **Deletes** (`RegDeleteKey`, `RegDeleteValue`) — removals are applied to the store and persisted.
- **Real keys** — any key not listed in `.interposer\Registry.reg` passes through to the real registry.

The `.reg` file is written back in standard format, so it can be edited with any text editor between runs.

## Stacking Several .reg Files

`Registry.Files` in `.interposer\Config.yml` replaces the single default file with an
ordered stack. Each file is loaded on top of the one before it, so a later file wins
wherever two of them define the same value:

```yaml
Registry:
  Files:
    - '.interposer\Registry.reg'
    - '%SAVEDGAMES%\MyGame\Registry.reg'
```

The **last** file in the list is the writable layer. Every registry write the game makes
is saved there, and the files above it are never modified. That is what makes the pattern
above work: `.interposer\Registry.reg` ships with the game as a read-only template of
sane defaults, and each player's own settings accumulate in their Saved Games folder
without the two ever being confused for one another.

A few consequences worth knowing:

- A value the writable layer inherits from an earlier file is **not** copied into it just
  because the game read it. Only what the game actually writes is stored there, so
  updating the template between sessions still reaches players who already have their own
  layer.
- Deleting a value that came from an earlier file writes regedit's delete marker
  (`"Name"=-`) into the writable layer, because simply dropping it would not survive the
  next launch; the earlier file would hand it straight back.
- Paths may be absolute or relative to the directory holding the DLL, and support the same
  `%TOKEN%` expansion as [file redirects](/Interposer/FileRedirection): environment
  variables first, then known folders (`%SAVEDGAMES%`, `%DOCUMENTS%`, `%MYGAMES%`, ...),
  then `%GAMEDIR%` and `%INTERPOSERDIR%`. Any missing directory above the writable layer
  is created for you.
- A file that does not exist is simply an empty layer, so a fresh install with no per-user
  file yet behaves exactly like a single-file setup.

Omit `Registry.Files` entirely and the default path of `.interposer\Registry.reg` is assumed.

At `Level: Debug` each layer is named in the session log as it loads:

```
2025-03-14 12:00:00  [REG LAYER]  C:\Games\MyGame\.interposer\Registry.reg  ->  read-only
2025-03-14 12:00:00  [REG LAYER]  C:\Users\Pat\Saved Games\MyGame\Registry.reg  ->  writable
```

## Isolated Mode

By default the virtual store owns only the keys the `.reg` files actually name, and
everything else passes through to the real Windows registry. `Registry.Isolated` makes the
store authoritative for the entire registry instead:

```yaml
Registry:
  Isolated: true
```

With it on, a key the files have never heard of is treated as virtual anyway. Reads of it
return `ERROR_FILE_NOT_FOUND` rather than falling through, and writes land in the writable
layer rather than in `HKEY_LOCAL_MACHINE` or `HKEY_CURRENT_USER`. Nothing the game does
through the [hooked functions](#hooked-functions) reaches the real registry, and nothing
it does needs to be anticipated in advance. A key it invents at runtime is created in the
store and persisted like any other.

This is what to reach for when a game must be fully self-contained: a shared or
locked-down machine where the game may not write to the registry at all, a portable
install on removable media, or several copies of the same game that must not share state.

:::caution
Isolation hides genuine system configuration too. A game that reads real keys for
legitimate reasons, e.g. a codec path, an installed runtime's location, its own installer's
breadcrumbs, will find nothing there. Enable registry logging at `Debug` and look for
[`[REG PARTIAL]`](#telling-virtual-reads-from-real-ones) to see exactly what it went
looking for and did not get, then add those values to a `.reg` file.
:::

Isolation changes which keys are virtual, not what the store contains, so everything else
on this page still applies: values defined in the files read back normally, the overlay
ordering is unchanged, and writes still go to the last file in `Registry.Files`.

## Subkeys

Subkeys work the same way. Add them as separate sections:

```
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0]
"PlayerName"="DefaultPlayer"
"MusicVolume"=dword:00000064

[HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0\Controls]
"JumpKey"=dword:00000020
"FireKey"=dword:0000001D
```

A game opening `HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0` and then enumerating subkeys will see `Controls` in the list.

## Logging Registry Access

To verify that registry emulation is working, enable registry logging in `.interposer\Config.yml`:

```yaml
Logging:
  Registry: true
```

A working session looks like:

```
2025-03-14 12:00:01  [REG OPEN]   HKEY_LOCAL_MACHINE\SOFTWARE\MYGAME\1.0
2025-03-14 12:00:01  [REG READ]   HKEY_LOCAL_MACHINE\SOFTWARE\MYGAME\1.0\PLAYERNAME
2025-03-14 12:00:02  [REG WRITE]  HKEY_LOCAL_MACHINE\SOFTWARE\MYGAME\1.0\PLAYERNAME
```

:::note
Key paths are uppercased in the log for normalization. Registry key and value name matching is always case-insensitive.
:::

### Telling virtual reads from real ones

At the default `Info` level a `[REG READ]` line looks identical whether the value came from `.interposer\Registry.reg` or from the real Windows registry. Add `Level: Debug` to see the verdict:

```yaml
Logging:
  Registry: true
  Level: Debug
```

```
2025-03-14 12:00:01  [REG HIT]      HKEY_LOCAL_MACHINE\SOFTWARE\MYGAME\1.0  ->  served from virtual store
2025-03-14 12:00:02  [REG MISS]     HKEY_CURRENT_USER\SOFTWARE\OTHERAPP  ->  not in virtual space
2025-03-14 12:00:02  [REG PARTIAL]  HKEY_LOCAL_MACHINE\SOFTWARE\MYGAME\1.0  ->  value not in store: RESOLUTION
```

- **`[REG MISS]`** — the key is not in the virtual store, so the call passed through to the real registry. Add the key to `.interposer\Registry.reg` to intercept it. A miss reading `handle not resolvable` means the game obtained the key handle in a way the Interposer could not trace (for example through an unhooked API), so no redirect was even attempted.
- **`[REG PARTIAL]`** — the key *is* virtual but the specific value the game asked for is not in the file. Because a virtual key never falls back to the real registry, the game receives `ERROR_FILE_NOT_FOUND`. This is the most common cause of a game behaving as if its settings were wiped, and it is invisible at `Info` level. Add the missing value to `.interposer\Registry.reg`.

## Hooked Functions

The following registry API functions (17 total from `advapi32.dll`) are intercepted:

- `RegOpenKeyExW` / `RegOpenKeyExA`
- `RegCreateKeyExW` / `RegCreateKeyExA`
- `RegQueryValueExW` / `RegQueryValueExA`
- `RegSetValueExW` / `RegSetValueExA`
- `RegDeleteKeyW` / `RegDeleteKeyA`
- `RegDeleteValueW` / `RegDeleteValueA`
- `RegEnumKeyExW` / `RegEnumKeyExA`
- `RegEnumValueW` / `RegEnumValueA`
- `RegCloseKey`
- `RegFlushKey`
- `RegQueryInfoKeyW`
