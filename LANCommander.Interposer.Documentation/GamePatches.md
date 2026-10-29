---
sidebar_label: Linking from a Game Patch
sidebar_position: 13
---

# Linking from a Game Patch

A community patch can ship the Interposer as part of itself. Your patch DLL (or
patched executable) links `LANCommander.Interposer.lib`, and from then on
Windows loads the Interposer whenever it loads your patch. FastDL, file
redirection, registry emulation and the rest then work for every player who
installs the patch, whether or not they use LANCommander.

Say you maintain a Battlefield 1942 patch and want custom maps to download from
the server automatically. Link the Interposer, ship a `Config.yml` with FastDL
turned on, and you are done. Your patch does not have to call anything for that
to work. The API is there if you want to log, read settings, or set rich
presence.

## What is in the SDK

The release ZIP has an `SDK\` folder:

```
SDK\
  include\
    LANCommander.Interposer.h      the whole API, plain C
  lib\
    x86\LANCommander.Interposer.lib
    x64\LANCommander.Interposer.lib
```

The header and libraries work with any compiler that can consume an MSVC import
library: MSVC of any version, clang-cl, or MinGW. The API is plain C. Every
function is declared `__cdecl` explicitly, so a project built with a different
default calling convention (`/Gz`) still calls it correctly. Buffers are always
owned by the caller, so your patch and the Interposer do not need to share a C
runtime.

The import library contains only the `Interposer*` functions. The DLL also
exports the `version.dll` and DirectInput entry points it uses as a proxy, but
those are left out of the `.lib`, so linking it next to `version.lib` or
`dinput8.lib` never binds `GetFileVersionInfoW` or `DirectInput8Create` to the
wrong DLL.

## Project setup

1. Add `SDK\include` to the include path.
2. Link `SDK\lib\x86\LANCommander.Interposer.lib` or
   `SDK\lib\x64\LANCommander.Interposer.lib`, whichever matches the game. Most
   games from the LAN era are 32-bit.
3. Call the API wherever you need it:

```cpp
#include <LANCommander.Interposer.h>

void PatchInit()
{
    InterposerRegisterPluginConfig(L"BF1942Patch",
        L"ShowWelcome: true\n"
        L"WelcomeText: 'Patched by the community'");

    wchar_t text[256];

    if (InterposerGetConfigString(L"Plugins.BF1942Patch.WelcomeText", text, ARRAYSIZE(text)))
        InterposerLog(L"BF1942PATCH", text);
}
```

Your patch's own settings go under `Plugins.<Name>` in `Config.yml`, just like a
plugin's. See [Creating a Plugin](Plugins/CreatingPlugins) for the full API
reference. The functions are the same, whether you link them or resolve them
with `GetProcAddress`.

## What you ship

Place these beside the game executable, along with your patch:

```
BF1942.exe
YourPatch.dll
LANCommander.Interposer.dll          from the release ZIP's x86\ (or x64\)
.interposer\
  Config.yml
```

The Interposer looks for `.interposer\` beside its own DLL, not beside your
patch. Keep both in the game directory unless you have a reason not to.

A `Config.yml` for the FastDL example:

```yaml
FastDL:
  Enabled: true
  ProbeConnections: true    # find the server's FastDL endpoint from the server you join
  Paths:
    - Local: '%GAMEDIR%\Mods\bf1942\archives\bf1942\levels'
      Remote: 'bf1942/levels'
```

`%GAMEDIR%` is what makes this config portable. It expands to the folder
holding the game executable on each player's machine, so you never need to know
where they installed the game. The mapping also catches files the game opens by
relative path. See [FastDL](FastDL#paths-that-do-not-depend-on-the-install-location).

Start from the sample `Config.yml` in the release ZIP. Every option is
documented in it, and every feature is off until you turn it on.

## When the hooks are in place

With a normal (static) link, the loader loads the Interposer before your patch's
`DllMain` runs, and the Interposer installs every hook from its own `DllMain`.
By the time any of your code runs, the hooks are live. If the game loads your
patch at startup, they are live before the game opens its first file.

If your patch is a DLL the game loads later with `LoadLibrary`, the Interposer
arrives at that point. Files the game opened earlier were not seen, and that
includes registry keys it has already opened. For FastDL this rarely matters
because maps load long after startup. For registry emulation it can matter.

### Making the Interposer optional

A static link makes the Interposer a hard dependency. If
`LANCommander.Interposer.dll` is missing, Windows refuses to load your patch,
and if your patch is imported by the game executable, the game does not start.

If the patch should keep working without the Interposer, delay-load it instead:
link with `/DELAYLOAD:LANCommander.Interposer.dll` and `delayimp.lib`, then load
it yourself once the game is running and call the API only if that worked:

```cpp
static bool g_hasInterposer = false;

void PatchInit()   // not DllMain
{
    g_hasInterposer = LoadLibraryW(L"LANCommander.Interposer.dll") != nullptr;

    if (g_hasInterposer)
        InterposerLog(L"BF1942PATCH", L"Interposer present");
}
```

The hooks then go in at that `LoadLibraryW`, with the timing caveat above.
Do not call `LoadLibraryW` from your own `DllMain`.

## Alongside LANCommander's own deployment

A game you have patched may also have the Interposer deployed to it by
LANCommander, or by a player who copied `version.dll` in by hand. That puts two
copies in one process. They coordinate:

- **The first copy to load owns the process.** It reads its `.interposer\`
  folder and installs every hook.
- **Every later copy installs nothing.** Each of its `Interposer*` functions
  forwards to the first copy, so your patch's calls work no matter which copy
  the loader bound them to. Files are still redirected, logged and downloaded
  once, not twice.
- **The first copy cannot be unloaded while a later one exists.** This means a
  forwarded call can never land in an unloaded DLL.

Only the first copy's configuration applies. When both copies sit in the game
directory, that is the same `Config.yml` either way.

This requires the copy that loads first to be a version that supports it. An
older Interposer does not know another copy exists, and both will install hooks.

### When LANCommander manages the game

LANCommander's Interposer redistributable owns `.interposer\` in the game
directory:

- **Install** renders `.interposer\Config.yml` from the options set on the
  server and **replaces the file you shipped**. Put your settings in the
  redistributable's options in LANCommander, or the next install loses them.
- **Uninstall** deletes `LANCommander.Interposer.dll` (if it is the Interposer)
  and the `.interposer\` folder. A patch that links the Interposer statically
  will then fail to load. If players might remove the redistributable from a
  patched game, delay-load the Interposer as described above.
