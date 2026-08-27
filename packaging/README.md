# LANCommander Interposer {VersionTag}

The LANCommander Interposer is a set of Windows API hooks designed to make games easier to run on modern platforms. The current version of the interposer adds the following functionality to almost any game:

- File path redirection
- DNS redirection
- Registry emulation
- FastDL support
- Rich presence (Discord)
- DirectInput compatibility and device filtering

Full documentation: <https://lancommander.app/Interposer/Overview>

## What is in this archive

```
x64\                                     64-bit game executables
x86\                                     32-bit game executables
  LANCommander.Interposer.dll              the Interposer itself
  version.dll                              |
  dinput8.dll                              |  the same DLL, renamed --
  dinput.dll                               |  pick ONE (see below)
  LANCommander.Interposer.asi              |
  LANCommander.Interposer.Injector.exe     inject into a running/launching game
  Plugins\                                 optional plugins, not enabled by default
  .interposer\Config.yml                   settings
```

`version.dll`, `dinput8.dll`, `dinput.dll` and `LANCommander.Interposer.asi` are
byte-for-byte the same file as `LANCommander.Interposer.dll`. It exports
everything all three of those system DLLs export and works out which one it is
standing in for from the name it was given, passing anything it does not handle
through to the real system DLL. Copy whichever one suits the game.

## Installing

1. **Pick the architecture that matches the game executable**, not your version
   of Windows. Most games from the LAN era are 32-bit, so `x86` is the usual
   answer. If you are unsure, right-click the game's `.exe` →
   **Properties** → **Details**, or check Task Manager for `(32 bit)` while it
   runs. Mixing architectures silently fails to load.

2. **Pick a load method** and copy that one file next to the game executable:

   | File | Use it when |
   |---|---|
   | `version.dll` | **Start here.** Most executables load `version.dll` implicitly at startup, so nothing else is needed. |
   | `dinput8.dll` | The game already ships its own `version.dll`, or a mod loader has claimed that name. Works for games that use DirectInput 8. |
   | `dinput.dll` | DirectInput 3/7 era games. These commonly import `dinput.dll` and nothing else that can be proxied, so this is often the only method that reaches them. |
   | `LANCommander.Interposer.asi` | The game already has an ASI loader installed (Ultimate ASI Loader, ScriptHookV, and similar). |

   If the game does not implicitly load any of those, use the injector instead:

   ```
   LANCommander.Interposer.Injector.exe --launch "C:\Games\MyGame\game.exe" -- LANCommander.Interposer.dll
   ```

3. **Copy the `.interposer` folder** into the same directory. The Interposer
   resolves it relative to its own location, so it has to sit beside whichever
   DLL you chose.

4. **Edit `.interposer\Config.yml`.** Every feature is off or empty by default;
   the file documents each setting inline.

5. Launch the game normally. A log is written to
   `.interposer\Logs\<timestamp>.log` whenever any `Logging` option is on.

## Plugins

`Plugins\` holds optional add-ons. They are **not** active as shipped — to
enable one, copy it into `.interposer\Plugins\` next to the DLL and add the
matching section under `Plugins:` in `Config.yml`.

| Plugin | Purpose |
|---|---|
| `LANCommander.Interposer.Plugin.CDKey.dll` | Serves a per-player CD key from the virtual registry |
| `LANCommander.Interposer.Plugin.Mouse.dll` | Stops stair-stepped mouse look in older engines, and scales each axis |

## Using this with LANCommander

Do not install by hand. Import the `.lcx` package published alongside this
archive instead. LANCommander will pick the load method and architecture per
game and renders `Config.yml` from the options you set on the server.

## Uninstalling

Delete the DLL you copied and the `.interposer` folder. The Interposer never
writes to the real registry and makes no changes outside the game directory.
