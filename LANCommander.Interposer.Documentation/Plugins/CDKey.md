---
sidebar_label: CD Key
sidebar_position: 3
---

# CD Key Plugin

The CD Key plugin generates a deterministic CD key from the player's username and injects it into a registry value so the game reads it as if the game were installed with that key. The same username always produces the same key for a given mask, so every player on a LAN has a unique key that is consistent across sessions without any manual entry.

## How It Works

On load the plugin:

1. Reads the key mask, registry key path suffix, and value name from `Config.yml`.
2. Resolves the player username via the Interposer identity system (the `Player.Username` config value or `--username` injector flag, falling back to the real Windows account name).
3. Generates a key by hashing the username with FNV-1a and stepping an LCG to fill each `*` position in the mask with a character from `[A-Z0-9]`.
4. Injects the generated key into the virtual registry store using suffix matching, so any registered key whose path ends with the configured `KeyPath` receives the value.

The injection is transient — it is not written back to `.interposer\Registry.reg`.

## Setup

### 1. Add the key to Registry.reg

The target registry key must appear in `.interposer\Registry.reg` for registry emulation to intercept reads. If the game stores the CD key as the default (unnamed) value, an empty key header is sufficient:

```
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Battlefield 1942\ergc]
```

If the key already has other values you want to preserve, export it from `regedit.exe` and include the full entry.

### 2. Configure the plugin

Add a `Plugins.CDKey` section to `.interposer\Config.yml`:

```yaml
Plugins:
  CDKey:
    Mask:      "****-**********-*******-****"
    KeyPath:   "Battlefield 1942\\ergc"
    ValueName: "@"
```

| Option | Required | Description |
|---|---|---|
| `Mask` | Yes | Key template — `*` is replaced with a generated character, all other characters are copied verbatim |
| `KeyPath` | Yes | Suffix of the registry key path to target. Matched case-insensitively on a backslash boundary against all keys in the virtual store |
| `ValueName` | No | Name of the registry value to write. Use `@` or omit entirely to target the default (unnamed) value. Defaults to `@` |

### 3. Place the plugin

Copy `LANCommander.Interposer.Plugin.CDKey.dll` into `.interposer\Plugins\` next to the main DLL:

```
C:\Games\Battlefield 1942\
  BF1942.exe
  LANCommander.Interposer.dll
  .interposer\
    Config.yml
    Registry.reg
    Plugins\
      LANCommander.Interposer.Plugin.CDKey.dll
```

## Mask Syntax

The mask defines the shape of the generated key. Any `*` character is replaced with a letter or digit (`[A-Z0-9]`). All other characters — including hyphens, spaces, and brackets — are preserved exactly as written.

| Mask | Example output |
|---|---|
| `****-****-****-****` | `K7MN-2BPQ-X4RT-9LWA` |
| `****-**********-*******-****` | `K7MN-2BPQ3X4RT9L-WA5FM2Z-8QBR` |
| `{****-****}` | `{K7MN-2BPQ}` |

## Log Output

With the plugin loaded, the session log shows the injected value:

```
2025-03-14 12:00:01  [PLUGIN LOAD]   ...\.interposer\Plugins\LANCommander.Interposer.Plugin.CDKey.dll
2025-03-14 12:00:01  [CDKEY]         Battlefield 1942\ergc\@  ->  K7MN-2BPQ3X4RT9L-WA5FM2Z-8QBR
```

If the configured `KeyPath` suffix matches no keys in the virtual store, a warning is logged instead:

```
2025-03-14 12:00:01  [CDKEY]         No virtual store keys matched suffix "Battlefield 1942\ergc" — add the key to Registry.reg
```

## Notes

- Key generation is deterministic: the same username and mask always produce the same key.
- The generated key is uppercase alphanumeric only. If a game requires a specific character set or checksum validation, a custom plugin with a tailored generation algorithm will be needed instead.
- If `KeyPath` matches more than one key in the virtual store (e.g. two subkeys both ending with the same suffix), the value is injected into all of them and the log line notes how many keys were updated.
