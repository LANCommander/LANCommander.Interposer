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
    LANCommander.Interposer.h                the Interposer* API, plain C
    LANCommander.Interposer.Comms.h          voice chat: common definitions
    LANCommander.Interposer.Comms.Client.h   voice chat: the game client side
    LANCommander.Interposer.Comms.Server.h   voice chat: the game server side
    LANCommander.Interposer.Export.h         the export macros all of them share
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

The voice headers declare the `interposer_comms_*` functions: proximity and
group voice chat over Opus, built into the same DLL. They are plain C89 and need
nothing from `<windows.h>`, so they can be included from an old game SDK as they
are. Voice is driven entirely by your patch -- there is nothing to set in
`Config.yml` -- and that includes its logging: call
`interposer_comms_set_session_logging(INTERPOSER_COMMS_LOG_INFO)` to have the
voice library's messages written to the session log beside the Interposer's own.

The import library contains only the `Interposer*` and `interposer_comms_*`
functions. The DLL also
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

## Embedding the Interposer in your patch

A patch that ships as one file, such as a `dinput.dll` proxy or an `.asi`, can
carry the Interposer inside it instead of beside it. The patch links
`LANCommander.Interposer.Static`, and there is no `LANCommander.Interposer.dll`
to ship or to lose.

> The release SDK does not include the static library yet. Today it builds only
> from this repository, as the CMake target
> `LANCommander::Interposer::Embedded`.

The library is MSVC C++ built with the static CRT (`/MT`), so your patch must
build with MSVC or clang-cl, also `/MT`. Linking it takes two settings, and the
CMake target applies both:

- **Link the whole archive** (`/WHOLEARCHIVE:LANCommander.Interposer.Static.lib`).
  Your DLL must export every `Interposer*` function, as
  `LANCommander.Interposer.dll` does. That is how a second Interposer in the
  process, and the plugins in `.interposer\Plugins\`, find them. Without the
  flag, the linker drops each object your patch never calls into, and its
  exports go with it.
- **Define `INTERPOSER_STATIC`**, so the header declares the functions without
  `__declspec(dllimport)`.

Then attach the Interposer from your own `DllMain`, before your patch installs
anything:

```c
#include <LANCommander.Interposer.Embedded.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        InterposerEmbeddedAttach(instance);
        PatchInstall();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        PatchRemove();
        InterposerEmbeddedDetach(reserved != NULL);
    }

    return TRUE;
}
```

From then on the embedded Interposer behaves like the DLL deployed under your
patch's file name, with the features you built it with (see "Choosing
features" below). Its `.interposer\` folder is the one beside your patch. Its
hooks are installed even without a `Config.yml`, and they change nothing until
one turns a feature on. It also takes part in the coordination described below:
if another Interposer is already loaded, `InterposerEmbeddedAttach` returns
`FALSE`, installs nothing, and your patch's calls forward to the other copy.

### A patch that is `dinput.dll` or `dinput8.dll`

Your patch serves the game's `DirectInputCreateA`, `DirectInputCreateEx` or
`DirectInput8Create` itself. Call the matching `InterposerDirectInputCreateA`,
`InterposerDirectInputCreateEx` or `InterposerDirectInput8Create` from that
export, and wrap the object it returns. That keeps
`DirectInput.FixLegacyDeviceEnumeration` and `DirectInput.DeviceFilter` in
effect, provided the library carries `DIRECTINPUT` (see "Choosing features").
With neither configured, or without that feature, these functions call the real
DLL in System32.

Exporting these functions is also how another Interposer recognises your patch.
An Interposer that LANCommander deployed as `version.dll` would otherwise detour
the `DirectInputCreateA` of whatever module is named `dinput.dll`. That is your
patch, and for a game it bridges, the call would never reach your wrapper.

### Choosing features

Whatever the static library carries ends up in your patch, so every feature is
opt-in. Out of the box the library carries only its core: `Config.yml`, the
session log, the plugin config functions and the launcher's event channel. Each
feature has a CMake option, off by default. Turn on the ones your patch uses
before your `add_subdirectory` of this repository:

```cmake
set(INTERPOSER_FEATURE_FILES   ON CACHE BOOL "" FORCE)
set(INTERPOSER_FEATURE_FASTDL  ON CACHE BOOL "" FORCE)
set(INTERPOSER_FEATURE_MATCHES ON CACHE BOOL "" FORCE)
add_subdirectory(${LANCOMMANDER_INTERPOSER_DIR} interposer EXCLUDE_FROM_ALL)
```

A feature in the **Needs** column must be turned on too. Configure stops with an
error naming both otherwise.

| Option (`INTERPOSER_FEATURE_...`) | What it carries | Needs |
|---|---|---|
| `REGISTRY` | the virtual registry (`Registry`) | |
| `FILES` | the file hooks and `FileRedirects` | |
| `FASTDL` | FastDL downloads (`FastDL`) | `FILES` |
| `DIRECTINPUT` | the DirectInput bridge, `DeviceFilter`, the mouse transform | |
| `AUDIO` | the audio device filter (`AudioDevices`) | |
| `OSVERSION` | the reported Windows version (`OsVersion`) | |
| `IDENTITY` | the user and computer name overrides (`Player`) | |
| `NETWORK` | `DnsRedirects`, `NetworkAdapters`, the adapter enumeration | |
| `RELAY` | the network relay and IPX (`NetworkRelay`) | `NETWORK` |
| `RICH_PRESENCE` | rich presence and Discord (`RichPresence`) | |
| `MATCHES` | match statistics (`MatchStatistics`) | |
| `VOICE` | voice chat, the `interposer_comms_*` API, with ENet, Opus and miniaudio | |
| `PLUGINS` | loading `.interposer\Plugins\` | |

A feature you do not turn on is left out of the code, not out of the API. Each
of its functions is still exported, and it answers the way it would with the
feature switched off:

- `InterposerRelayGetStatus` reports `Disabled`.
- `InterposerMatchStart` returns 0.
- Every voice call returns `INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED`, except the
  `*_init` helpers, the version functions and the log settings.
- `InterposerDirectInputCreateA` and its siblings call the real DLL in System32.

Your patch therefore links and runs unchanged, and so do plugins and other
copies of the Interposer that look the functions up by name. To ask at run time
what is really there, call `InterposerGetFeatures`, which returns an
`INTERPOSER_FEATURE_FLAG_*` bit for each feature this build carries.

A `Config.yml` section for a feature that was not built in has no effect. The
session log says so with a `[CONFIG]` line when the section is there.

Some features matter beyond their own section:

- Without `FILES` the Interposer does not see libraries load. It cannot hook a
  library the game loads later (`ws2_32`, `dinput8`, `dsound`) for `NETWORK`,
  `DIRECTINPUT` and `AUDIO`.
- Without `NETWORK`, FastDL cannot probe the servers the game connects to.
  It downloads only from a `FastDL.BaseUrl` or `InterposerSetFastDLBaseUrl`.

To build nothing but the library, set `INTERPOSER_EMBEDDED_ONLY` as well. Then
none of the DLL, the injector, the tests or the plugins is defined. Unless you
turn `VOICE` on, ENet, Opus and miniaudio are then not looked for, so your
`vcpkg.json` can leave them out. MinHook and yaml-cpp are always needed.

On Linux the switches work the same way. Link your host with
`-ffunction-sections -fdata-sections` and `-Wl,--gc-sections` to discard what
no export reaches, as MSVC's `/OPT:REF` does on Windows.

#### When your patch loads first

The first Interposer to load owns the process, even if it was built with less
than a later copy (see [below](#alongside-lancommanders-own-deployment)). If
your patch loads before the Interposer LANCommander deployed, the features you
did not turn on are off for the whole process, whatever LANCommander
configured. The later copy says so in your patch's session log:

```
[INSTANCE]  the Interposer that owns this process was built without features this copy carries, so they are off: Registry, Relay
```

So turn on more than your patch calls when players may run it beside
LANCommander's deployment: in particular, any feature LANCommander's
redistributable may configure for the game.

## Reporting matches

A patch can report the matches it sees to LANCommander, for its leaderboards and match
history: who joined, kills, laps, the final standings, a demo. Call the `InterposerMatch*`
functions as the game reports them. The Interposer batches, retries and keeps what it
has not delivered on disk:

```c
InterposerMatchInfo info = { sizeof(info) };
info.externalId = roundId;
info.map        = mapName;

DWORD match = InterposerMatchStart(&info);
InterposerMatchEvent(match, "kill", gameTimeMs, -1, "steam:STEAM_0:1:1", "bot:3", "{\"weapon\":\"rocket\"}");
InterposerMatchAddStatistic(match, "steam:STEAM_0:1:1", NULL, "kills", 1);
InterposerMatchEnd(match, "completed", FALSE);
```

Where the reports go is configuration, not code. A dedicated server sends them itself
with a token. A client's go through LANCommander's launcher. See
[Match Statistics](./MatchStatistics.md).

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
