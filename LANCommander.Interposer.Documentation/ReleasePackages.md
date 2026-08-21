---
sidebar_label: Release Packages
sidebar_position: 2
---

# Release Packages

Each release publishes four ZIP variants for manual deployment, plus a single LCX package for import into LANCommander. Choose the ZIP variant that matches how the DLL will be loaded into the game process — the LCX bundles all of them and lets you pick per game.

## Standard

**Files:** `LANCommander.Interposer.dll` + `LANCommander.Interposer.Injector.exe`

Use this when you have explicit control over how the DLL is loaded — for example, via the bundled injector CLI, the LANCommander client, or your own bootstrap code.

```
x64\
  LANCommander.Interposer.dll
  LANCommander.Interposer.Injector.exe
x86\
  LANCommander.Interposer.dll
  LANCommander.Interposer.Injector.exe
.interposer\
  Config.yml
```

This is the most flexible variant and the recommended choice for use with LANCommander.

## Proxy

**Files:** `version.dll`

The DLL is renamed to `version.dll` — a Windows system library that most game executables load implicitly. Placing it in the same directory as the game executable causes Windows to load it automatically before the game starts, with no injector required.

```
x64\
  version.dll
x86\
  version.dll
.interposer\
  Config.yml
```

:::tip
This is the easiest deployment method for manual use. Copy the correct architecture's `version.dll` and the `.interposer\` directory next to the game executable and launch the game normally.
:::

:::caution
Some games ship their own `version.dll`. If the game directory already contains `version.dll`, use the Standard or ASI variant instead.
:::

## Proxy (dinput8)

**Files:** `dinput8.dll`

This is another version of the proxy DLL that hooks using `dinput8.dll` instead of `version.dll`. This is also the easiest method, but is provided as an alternative as most hook-based game patchers already use `dinput8.dll` for injection.

```
x64\
  dinput8.dll
x86\
  dinput8.dll
.interposer\
  Config.yml
```

## ASI

**Files:** `LANCommander.Interposer.asi`

The DLL is output as a `.asi` file for use with ASI loaders such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) or ScriptHookV. These loaders are already present in many moddable games (GTA series, etc.) and will pick up any `.asi` file placed in the game directory.

```
x64\
  LANCommander.Interposer.asi
x86\
  LANCommander.Interposer.asi
.interposer\
  Config.yml
```

Use this variant when the target game already has an ASI loader installed and you want to avoid replacing any existing `version.dll`.

## Architecture

Every variant ships both x64 and x86 builds. Use the architecture that matches the game executable — a 64-bit game needs the x64 DLL and a 32-bit game needs the x86 DLL. Mixing architectures will cause injection to silently fail.

If you are unsure, right-click the game executable in Windows Explorer → **Properties** → **Details** and check the listed machine type, or open Task Manager while the game is running and look for `(32 bit)` next to the process name.

## LCX Package

`LANCommander.Interposer.<version>.lcx` is a single redistributable for import into LANCommander. Its archive carries every load method for both architectures, so you assign one redistributable to a game and choose the loading strategy per game.

Because the redistributable defines an [option schema](https://lancommander.app/Server/Redistributables#option-schema), LANCommander treats it as a **compatibility shim**: every setting in `Config.yml` is exposed as a form field on the game's **Redistributables** page.

### Loader options

| Option | Values | Meaning |
|---|---|---|
| `Loader.Method` | `Proxy`, `DInput8`, `ASI` | Which build to copy next to the game executable |
| `Loader.Architecture` | `Auto`, `x86`, `x64` | `Auto` reads the PE header of the game's primary executable |
| `Loader.TargetDirectory` | path | Override the install location. Blank uses the folder containing the primary executable. |

The Standard (injector) variant is not offered through the LCX — driving the injector would require a [Run Wrapper](https://lancommander.app/Scripting/Script%20Types/Run%20Wrapper) script, which takes over game launching for every game the redistributable is assigned to. Use the Standard ZIP directly if you need the injector.

### Scripts

| Script | Behaviour |
|---|---|
| Detect Install | Always reports not installed — the shim is per game, not machine-wide |
| Install | Copies the selected build next to the game executable and renders `.interposer\Config.yml` from the configured options |
| Name Change | Keeps `Player.Username` in sync with the LANCommander player alias |
| Uninstall | Removes the DLL and the `.interposer\` directory. Files are checked against their embedded product name first, so a game's own `version.dll` is never deleted. |

:::caution
`Config.yml` is rendered at install time only. Changing options on the server requires reinstalling the game before they take effect, and local edits to the file are overwritten when that happens.
:::
