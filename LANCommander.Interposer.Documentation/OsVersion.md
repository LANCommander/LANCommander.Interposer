# OS Version

The Interposer can report a different version of Windows to the game than the one it is actually running on. This is an important feature of Windows' built-in compatibility shim engine (AppCompat), but as only _specific_ Windows API functions are hooked, it allows for granular control without involving other compatibility shims that may introduce unwanted functionality.

## Why Games Need This

Games from the XP era frequently branch on the OS version to pick a code path, and simply have no case for anything newer than what existed when they shipped. Windows reports version 6.2 to any executable without a compatibility manifest, so on a modern machine those branches fall through.

Most games will handle this well, but a select set of games might fail outright. For example, the game _The Lord of the Rings: The Battle for Middle-earth II_ throws an access violation exception if a version of 6.2 is reported. The commonly-suggested fix in the past has been to set the compatiblity mode in Windows to some version of Windows XP. However, this isn't the cleanest fix as it will force the game to run with administrator privileges and subsequentially any registry entries that the game uses will pull from HKLM instead of HKCU or the VirtualStore.

## Configuration

```yaml
OsVersion:
  Version: WindowsXP
```

The `Version` option can be used to easily represent a certain version of Windows using some default values for major/minor/build versions. The names here are matched loosely, so `Windows 8.1`, `windows8.1` and `Windows81` all select the same version.

| `Version` | Reports | Service pack | Product type |
|---|---|---|---|
| `None` | Fallback to the current OS version | | |
| `Windows2000` | 5.0.2195 | Service Pack 4 | Workstation |
| `WindowsXP` | 5.1.2600 | Service Pack 3 | Workstation |
| `WindowsXPx64` | 5.2.3790 | Service Pack 2 | Workstation |
| `WindowsServer2003` | 5.2.3790 | Service Pack 2 | Server |
| `WindowsVista` | 6.0.6002 | Service Pack 2 | Workstation |
| `Windows7` | 6.1.7601 | Service Pack 1 | Workstation |
| `Windows8` | 6.2.9200 | | Workstation |
| `Windows81` | 6.3.9600 | | Workstation |
| `Windows10` | 10.0.19045 | | Workstation |
| `Windows11` | 10.0.22631 | | Workstation |
| `Custom` | Specify a custom set of version numbers | | |

Build numbers are the last shipped build of each version, which is what a fully patched machine of that era would have reported.

`WindowsXPx64` and `WindowsServer2003` are the same 5.2.3790 and differ only in product type. Pick the server one only if the game takes a different path on server SKUs, though this is likely to be extremely rare or nonexistent.

### Overriding Individual Fields

Each of the keys below overrides a single field of the version selected above. They are all optional.

| Key | Type | Description |
|---|---|---|
| `Major` | int | Major version number. |
| `Minor` | int | Minor version number. |
| `Build` | int | Build number. |
| `ServicePack` | string | The `szCSDVersion` string, e.g. `'Service Pack 3'`. The trailing number also sets `wServicePackMajor` |
| `ProductType` | choice | `Workstation` (default) or `Server`. |

So this reports 5.1.2180 — Windows XP as it shipped, before any service pack:

```yaml
OsVersion:
  Version: WindowsXP
  Build: 2180
```

With `Version: Custom` there is no version to start from and the fields supply everything. A `Custom` version with no `Major` is ignored rather than reporting 0.0.0, and the reason is written to the log.

## Hooked Functions

| Function | DLL | Notes |
|---|---|---|
| `GetVersion` | kernel32 | Returns the packed form: major in the low byte, minor in the next, build in the high word, top bit clear for NT. |
| `GetVersionExA` | kernel32 | Fills `OSVERSIONINFOA` or `OSVERSIONINFOEXA`, whichever the caller sized its struct for. |
| `GetVersionExW` | kernel32 | As above, for `OSVERSIONINFOW` / `OSVERSIONINFOEXW`. |
| `RtlGetVersion` | ntdll | Deliberately more than a compatibility layer covers. |

## Compared to Compatibility Mode

Setting a compatibility layer on the executable achieves a similar result. The hook has three practical advantages:

- **It travels with the game.** A compatibility layer is per-user registry state under `HKCU\...\AppCompatFlags\Layers`. Reinstalling the game through LANCommander does not restore it, unless specified in an install script.
- **No elevation.** The pre-Vista layers (`WINXPSP3`, `WINXPSP2`, `WIN98`) force the process to run elevated. This can break game launches and also disables UAC registry virtualization, so a game that relies on `VirtualStore` redirection quietly loses its settings.

What it does not do is cover the *other* shims a compatibility layer applies. `HIGHDPIAWARE`, for instance, is a separate fix and not a version lie; if you need it, set it on its own.

## Verifying It Worked

Turn on the log:

```yaml
Logging:
  OsVersion: true
```

The startup line records what will be reported, and each entry point records itself the first time the game calls it:

```
2025-03-14 12:00:01  [OSVERSION]       WindowsXP  ->  5.1.2600 Service Pack 3 (workstation)
2025-03-14 12:00:01  [HOOK INIT]       kernel32!GetVersionExA
2025-03-14 12:00:03  [OSVERSION]       GetVersionExA  ->  5.1.2600 Service Pack 3
```
