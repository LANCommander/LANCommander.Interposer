---
sidebar_position: 3
---

# Getting Started

The Interposer works by injecting a DLL into a game process. Once loaded, it installs hooks on common Windows APIs and reads its configuration from files placed next to the DLL. No changes to the game itself are required.

## What You Need

The DLL itself is placed next to the game executable. Runtime configuration lives in a `.interposer\` subdirectory alongside it:

| File / Directory | Purpose |
|---|---|
| `LANCommander.Interposer.dll` | The hook DLL |
| `.interposer\Config.yml` | Settings, file redirect rules, and FastDL configuration |
| `.interposer\Registry.reg` | Virtual registry initial state (only needed for registry emulation; `Registry.Files` can move it or stack several) |
| `.interposer\Logs\` | Session log files — created automatically, one file per session |
| `.interposer\Downloads\` | FastDL overlay cache — created automatically |

A sample `Config.yml` is included in the release package with all options documented inline. You only need to enable or configure the features you want — every option has a sensible default.

### Supported Windows Versions

The Interposer, the injector and the bundled plugins run on **Windows XP SP3 and later** (x86), and **Windows XP x64 / Server 2003 SP2 and later** (x64). Nothing needs to be installed first: the C++ runtime is linked into each binary, so there is no Visual C++ Redistributable dependency on any version of Windows.

On older systems a feature can only work if Windows itself provides what it hooks:

- **Audio devices** — Core Audio does not exist before Vista. On XP, filtering covers winmm and DirectSound and matches device names only; endpoint IDs, instance paths and hardware IDs are not available.
- **FastDL** — XP's HTTP stack cannot negotiate modern TLS, so use a plain `http://` server.
- **Rich presence** — Discord does not run on XP.
- **Log noise** — a few `[HOOK INIT]` lines report an API as `not present on this version of Windows`. Those are APIs introduced after XP, which software written for it never calls.

## Choosing a Deployment Location

The DLL is found relative to whichever directory the injector or host process loads it from. The simplest approach is to place all files next to the game's executable:

```
C:\Games\MyGame\
  game.exe
  LANCommander.Interposer.dll
  .interposer\
    Config.yml
    Registry.reg      ← only needed for registry emulation
```

The DLL locates `Config.yml` and `Registry.reg` inside the `.interposer\` directory relative to its own path, not the working directory, so the exact placement of the game executable does not matter as long as the DLL and its `.interposer\` subdirectory share a directory.

## Injecting the DLL

### Using the Injector

The `LANCommander.Interposer.Injector.exe` handles injection in two modes.

**Inject into a running process** by name or PID:

```
Injector.exe <process-name-or-PID> [dll-path]
```

```
Injector.exe game.exe
Injector.exe 1234
Injector.exe game.exe "C:\path\to\LANCommander.Interposer.dll"
```

**Launch a game and inject before its first instruction runs** (recommended — avoids race conditions on startup):

```
Injector.exe --launch <exe-path> [game-args ...] -- [dll-path]
```

```
Injector.exe --launch "C:\Games\MyGame\game.exe"
Injector.exe --launch "C:\Games\MyGame\game.exe" -fullscreen
Injector.exe --launch "C:\Games\MyGame\game.exe" -fullscreen -- "C:\path\to\LANCommander.Interposer.dll"
```

The `--` separator is used to distinguish game arguments from the DLL path. If no DLL path is given, the injector searches its own directory for `LANCommander.Interposer.dll` or `interposer.dll`.

:::tip
Use `--launch` mode when possible. Injecting into a running process can miss hooks for API calls made during early startup, such as registry reads and file opens that happen before `main()`.
:::

:::caution
The injector and the game must match in bitness — a 64-bit injector cannot inject into a 32-bit process and vice versa. The injector will print a warning if a mismatch is detected.
:::

### Injecting from Another Application

Any process can inject the DLL by calling `LoadLibrary` with the full path to `LANCommander.Interposer.dll` from within the target process, or by using `CreateRemoteThread(LoadLibraryW, ...)` from an external process. The LANCommander client does this automatically when launching games.

## Minimal Configuration

The smallest working `.interposer\Config.yml` is:

```yaml
Logging:
  Files: true
  Registry: true
```

This enables access logging and leaves all other features at their defaults (file redirects disabled, FastDL disabled). Logs are written automatically to `.interposer\Logs\<timestamp>.log` — no path configuration is needed.

## Diagnostic Log

If the DLL fails to initialize — for example, because `.interposer\Config.yml` is missing or contains a syntax error — a diagnostic message is written to `%TEMP%\interposer_diag.log` regardless of any configuration. Check this file first if a game behaves unexpectedly after injection.
