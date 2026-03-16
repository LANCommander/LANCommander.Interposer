---
sidebar_label: Releases
sidebar_position: 2
---

# Releases

Each release publishes three package variants — a ZIP for manual deployment and a matching LCX for import into LANCommander. Choose the variant that matches how the DLL will be loaded into the game process.

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

All three variants ship both x64 and x86 builds. Use the architecture that matches the game executable — a 64-bit game needs the x64 DLL and a 32-bit game needs the x86 DLL. Mixing architectures will cause injection to silently fail.

If you are unsure, right-click the game executable in Windows Explorer → **Properties** → **Details** and check the listed machine type, or open Task Manager while the game is running and look for `(32 bit)` next to the process name.

## LCX Packages

Each variant is also distributed as an `.lcx` file for direct import into LANCommander as a redistributable. The LCX packages include install and name-change scripts that automatically configure the `.interposer\` directory structure and keep the player username in sync with the LANCommander player alias.

| File | Variant |
|---|---|
| `LANCommander.Interposer.<version>.lcx` | Standard |
| `LANCommander.Interposer.Proxy.<version>.lcx` | Proxy |
| `LANCommander.Interposer.ASI.<version>.lcx` | ASI |
