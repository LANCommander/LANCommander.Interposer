---
sidebar_position: 5
---

# Registry Emulation

Registry emulation intercepts Windows registry API calls and serves reads and writes from an in-memory store instead of the real Windows registry. The store is initialized from a `Registry.reg` file placed inside the `.interposer\` directory next to the DLL, and any writes made by the game are persisted back to that file automatically.

## Why Use It

Many older games write configuration, key bindings, and save state directly to the Windows registry — typically under `HKEY_LOCAL_MACHINE\SOFTWARE\<Publisher>\<Game>` or `HKEY_CURRENT_USER\SOFTWARE\<Game>`. This causes several problems:

- **Administrator privileges required** — writing to `HKEY_LOCAL_MACHINE` requires elevation
- **Not portable** — registry entries don't travel with a game installation
- **Shared between users** — a single set of registry keys is used by all users on the machine
- **Hard to back up or reset** — registry state is scattered and opaque

Registry emulation solves all of these by redirecting those calls to a plain text file inside the game directory.

## Setting Up Registry.reg

Create a file named `Registry.reg` inside the `.interposer\` directory next to the DLL. This file uses the standard Windows `.reg` format:

```
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\MyGame\1.0]
"PlayerName"="DefaultPlayer"
"MusicVolume"=dword:00000064
"FullScreen"=dword:00000001
```

Only keys that appear in this file are intercepted. Any registry access to a key that is **not** listed in `.interposer\Registry.reg` passes through to the real Windows registry unchanged.

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

## Subkeys

Subkeys work the same way — add them as separate sections:

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

If a key the game opens does not appear in the log, it is not in the virtual store and is passing through to the real registry. Add it to `.interposer\Registry.reg` to intercept it.

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
