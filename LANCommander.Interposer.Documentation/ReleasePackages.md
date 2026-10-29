---
sidebar_label: Release Packages
sidebar_position: 2
---

# Release Packages

| Artifact | For |
|---|---|
| `LANCommander.Interposer.<version>.zip` | Manual deployment |
| `LANCommander.Interposer.<version>.lcx` | Import into LANCommander |

## Manual Deployment

The manual deployment consists of builds for both x86 and 64-bit architectures:

```
README.md
x64\
x86\
  LANCommander.Interposer.dll
  version.dll
  dinput8.dll
  dinput.dll
  LANCommander.Interposer.asi
  LANCommander.Interposer.Injector.exe
  Plugins\
    LANCommander.Interposer.Plugin.CDKey.dll
  .interposer\
    Config.yml
SDK\
  include\
    LANCommander.Interposer.h
  lib\
    x86\LANCommander.Interposer.lib
    x64\LANCommander.Interposer.lib
```

:::note Unified Binary
`version.dll`, `dinput8.dll`, `dinput.dll` and `LANCommander.Interposer.asi` are
byte-for-byte the same file as `LANCommander.Interposer.dll`. The injection method
will vary based on which filename is used. Supported DLL signatures are represented
by these filenames.
:::

Copy **one** of those files, plus the `.interposer\` directory, next to the game
executable.

| File | Use it when |
|---|---|
| `version.dll` | **Start here.** Most executables load `version.dll` implicitly at startup, so nothing else is needed. |
| `dinput8.dll` | The game already ships its own `version.dll`, or a mod loader has claimed that name. Works for games using DirectInput 8. |
| `dinput.dll` | DirectInput 3/7 era games. They commonly import `dinput.dll` and nothing else that can be proxied, so this is often the only method that reaches them. Pair it with the [DirectInput](/Interposer/DirectInput) settings. |
| `LANCommander.Interposer.asi` | The game already has an ASI loader installed. |
| `LANCommander.Interposer.dll` | You control loading yourself — via the bundled injector, a launcher, or your own bootstrap code. |

`SDK\` is for game patches that link the Interposer rather than relying on a
load method. See [Linking from a Game Patch](GamePatches).

`Plugins\` is shipped but not active. To enable a plugin, copy it into
`.interposer\Plugins\` beside the DLL and add its section under `Plugins:` in
`Config.yml`.

## Architecture

Every variant ships both x64 and x86 builds. Use the architecture that matches the game executable — a 64-bit game needs the x64 DLL and a 32-bit game needs the x86 DLL. Mixing architectures will cause injection to silently fail.

If you are unsure, right-click the game executable in Windows Explorer → **Properties** → **Details** and check the listed machine type, or open Task Manager while the game is running and look for `(32 bit)` next to the process name.

## LCX Package

`LANCommander.Interposer.<version>.lcx` is a single redistributable for import into LANCommander. Its archive carries every load method for both architectures, so you assign one redistributable to a game and choose the loading strategy per game.

Because the redistributable defines an [option schema](https://lancommander.app/Server/Redistributables#option-schema), LANCommander treats it as a **compatibility shim**: every setting in `Config.yml` is exposed as a form field on the game's **Redistributables** page.

### Loader options

| Option | Values | Meaning |
|---|---|---|
| `Loader.Method` | `Proxy`, `DInput8`, `DInput`, `ASI`, `Injector` | Which build to copy next to the game executable. `Injector` also copies the injector executable. |
| `Loader.Architecture` | `Auto`, `x86`, `x64` | `Auto` reads the PE header of the game's primary executable |
| `Loader.TargetDirectory` | path | Override the install location. Blank uses the folder containing the primary executable. |

The `Injector` method installs `LANCommander.Interposer.dll` and `LANCommander.Interposer.Injector.exe` next to the game executable. Unlike the other methods, nothing loads the DLL implicitly - the game has to be launched through the injector, so point the game's primary action at the injector executable (`LANCommander.Interposer.Injector.exe --launch <game executable>`).

### Scripts

| Script | Behaviour |
|---|---|
| Detect Install | Always reports not installed — the shim is per game, not machine-wide |
| Install | Copies the selected build next to the game executable (plus the injector executable for the `Injector` method) and renders `.interposer\Config.yml` from the configured options |
| Name Change | Keeps `Player.Username` in sync with the LANCommander player alias |
| Uninstall | Removes the DLL, the injector executable and the `.interposer\` directory. Files are checked against their embedded product name first, so a game's own `version.dll` is never deleted. |
