# Player Identity

The Interposer can override the username and computer name returned by the Windows API so that games see custom values instead of the real Windows account and machine names. This is useful when multiple users share the same Windows installation, or when a game uses these values for player identification.

## Username

### How It Works

When `GetUserNameW` or `GetUserNameA` is called by the game, the hook returns the configured username instead of delegating to the operating system.

### Configuring the Username

Set `Username` under the `Player` section of `.interposer/Config.yml`:

```yaml
Player:
  Username: 'Player1'
```

Leave `Username` empty (or omit the key entirely) to pass calls through to the real Windows API unchanged.

### Overriding the Username at Launch

To set the username without editing `.interposer/Config.yml`, pass `--username` to the injector:

```
Injector.exe --username Player1 --launch "C:\Games\game.exe"
```

The name is passed to the DLL via a named memory-mapped file (`Local\InterposerUsername_<pid>`) and overrides any `Username` value in the config.

## Computer Name

### How It Works

When `GetComputerNameW` or `GetComputerNameA` is called by the game, the hook returns the configured computer name.

### Configuring the Computer Name

Set `ComputerName` under the `Player` section of `.interposer/Config.yml`:

```yaml
Player:
  ComputerName: 'GAMEPC'
```

Leave `ComputerName` empty (or omit the key) to pass calls through to the real Windows API unchanged.

### Overriding the Computer Name at Launch

```
Injector.exe --computername GAMEPC --launch "C:\Games\game.exe"
```

The name is passed via `Local\InterposerComputerName_<pid>` and overrides the config value.

## Hooked Functions

| Function | DLL | Notes |
|---|---|---|
| `GetUserNameW` | `advapi32` | Returns the configured username as a wide string. On buffer-too-small: `ERROR_INSUFFICIENT_BUFFER`. |
| `GetUserNameA` | `advapi32` | Returns the configured username in the system ANSI code page. On buffer-too-small: `ERROR_INSUFFICIENT_BUFFER`. |
| `GetComputerNameW` | `kernel32` | Returns the configured computer name as a wide string. On buffer-too-small: `ERROR_BUFFER_OVERFLOW`. |
| `GetComputerNameA` | `kernel32` | Returns the configured computer name in the system ANSI code page. On buffer-too-small: `ERROR_BUFFER_OVERFLOW`. |

All four functions honour their respective Windows API buffer contracts. Passing `NULL` as the buffer performs a size query.

Each pair is only hooked when its corresponding config value (or injector argument) is non-empty — if both are unset, no hooks are installed and all calls pass through to the real API.
