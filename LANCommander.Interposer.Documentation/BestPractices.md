---
sidebar_label: Best Practices
sidebar_position: 8
---

# Best Practices

This guide collects the conventions we recommend when building an Interposer configuration for a game. The goal is for every Interposer-managed game on a system to behave the same way: store its data in predictable, per-user, portable locations, run without administrator privileges, and play nicely with the rest of the operating system. Following these patterns also makes configurations easier to share, audit, and maintain across a library of titles.

## Saved Games

### A Brief History of Save File Locations on Windows

Over three decades of PC gaming, the question of *where to put save files* has been answered roughly one new way per generation of Windows... and never authoritatively. Why not have a quick history lesson:

- **DOS / Win9x era** — Saves landed inside the game's own install directory, alongside the executable. There was no concept of "user data" because there were no users. Quake 1, Quake 3, Doom, Heretic, Duke Nukem 3D, Half-Life 1, Unreal Tournament 99, all of them write their save files into subfolders under the install directory.
- **Windows XP** — With NT-derived multi-user support reaching home PCs, games began moving saves into `%USERPROFILE%\My Documents\<Game>\`. There was no standard for the subdirectory layout, so every publisher invented its own.
- **Windows Vista (2007)** — Two important things happened. First, UAC made `Program Files` non-writable for non-elevated processes, so games that still tried to save into their install directory broke outright (or got silently shovelled into the per-user `VirtualStore`). Second, Microsoft introduced the **Known Folders** system, including a brand new folder dedicated to game saves: `%USERPROFILE%\Saved Games` (`FOLDERID_SavedGames`).
- **Games for Windows – LIVE** — A handful of GfWL-era titles adopted `%USERPROFILE%\Documents\My Games\<Game>\`, which became a popular but unofficial standard.
- **Modern era** — Most contemporary games settle on `%APPDATA%` (`Roaming`) or `%LOCALAPPDATA%`, with a smaller group using the official `Saved Games` folder.

The result is a library where different generations of games scatter their data across half a dozen different roots.

### Recommended Pattern: Always Use `Saved Games`

Even though Microsoft never enforced it, the `Saved Games` known folder is the only location on Windows that was *explicitly designed* for game save data. It is per-user, writable without elevation, included by default in Windows backup and File History, and recognized by tools like the GameSave Manager family. We recommend redirecting every Interposer-managed game's saves to:

```
%USERPROFILE%\Saved Games\<Publisher>\<Game>\
```

This gives every title in your library a consistent, predictable home, makes per-user saves trivial on shared machines, and lets you back up an entire game library by archiving a single folder.

### Examples

#### Quake 3 Arena

Quake 3 stores its config and saved games inside `baseq3\` in the install directory:

```yaml
FileRedirects:
  - Pattern: 'C:\\Games\\Quake3\\baseq3\\(q3config\.cfg|.*\.sav)'
    Replacement: '%USERPROFILE%\Saved Games\id Software\Quake III Arena\$1'
```

Because file redirects auto-create the destination directory tree on write (since v1.0.3), the first time the game writes its config the `Saved Games\id Software\Quake III Arena\` path is created automatically.

#### Unreal Tournament 99

UT99 keeps save slots in `System\Save\` and the user config in `System\UnrealTournament.ini`:

```yaml
FileRedirects:
  - Pattern: '.*\\UnrealTournament\\System\\Save\\(.+)'
    Replacement: '%USERPROFILE%\Saved Games\Epic Games\Unreal Tournament\Save\$1'
  - Pattern: '.*\\UnrealTournament\\System\\(UnrealTournament|User)\.ini'
    Replacement: '%USERPROFILE%\Saved Games\Epic Games\Unreal Tournament\$1.ini'
```

The leading `.*\\` pattern lets the same rule cover both the default install path and any portable copy, regardless of where the game lives on disk.

#### Half-Life 1 (GoldSrc)

Half-Life writes save games to `<gamedir>\SAVE\`:

```yaml
FileRedirects:
  - Pattern: '.*\\Half-Life\\valve\\SAVE\\(.+)'
    Replacement: '%USERPROFILE%\Saved Games\Valve\Half-Life\$1'
```

### Tips

- **Use single-quoted YAML strings** for patterns so backslashes pass through literally.
- **Anchor with `.*\\` instead of a hard-coded drive letter** when possible — the same rule then works for every install location.
- **Capture the filename, not the directory**, with `(.+)` at the tail of the pattern, and substitute it with `$1` in the replacement. One rule then covers an arbitrary number of files in the directory.
- **Use `Saved Games` for true save data**, but `%APPDATA%` is fine for transient shell state (recently-opened lists, window positions, etc.). Match what modern games already do.
- **Verify with the file log**, turn on `Logging.Files: true` and look for `[FILE REDIRECT]` entries to confirm the rule is firing on the path you expected.

## FastDL: Notes for Server Administrators

[FastDL](/Interposer/FastDL) lets the Interposer download missing or out-of-date game assets from an HTTP server before the game opens them. This section covers what the *server* side needs to look like so that clients can use it.

### How the Client Talks to the Server

When the game opens a file under a configured `FastDL.Paths` prefix, the Interposer:

1. Sends a `HEAD` request to `<BaseUrl>/<remote-prefix>/<relative-path>` to check whether the server has the file.
2. If the response is `200 OK`, sends a `GET` for the same URL and writes the body to the overlay cache.
3. If the response includes an `X-Checksum-CRC32` header, that value is recorded alongside the cached file. On the next open, the header is fetched again and the cached file is reused if and only if the checksums match.
4. The cached file's handle is returned to the game.

This means a FastDL server is almost always a stock static-file HTTP server. There is no special wire protocol; anything that can serve directories (nginx, Apache, Caddy, IIS, even `python -m http.server` for testing) works as long as it speaks `HEAD` and `GET` correctly.

### Mirroring the Game's Directory Layout

The remote URL is constructed by appending the game's *relative path beneath the configured local prefix* to the remote prefix. The directory layout on the server therefore has to mirror the directory layout the game expects on disk.

#### Worked Example: Battlefield 1942 Custom Maps

Battlefield 1942 ships maps as `.rfa` archive files inside `Mods\<mod>\Archives\bf1942\Levels\<map>\`. Servers running custom maps need every connecting client to have those `.rfa` files locally, which historically meant a manual download from the server's website. With FastDL, the same server can serve the maps automatically.

**Server side**: sit an HTTP server in front of the BF1942 mod files. With nginx:

```nginx
server {
    listen 80;
    server_name fastdl.example.com;

    location /bf1942/ {
        alias /srv/fastdl/bf1942/;
        autoindex off;
    }
}
```

The directory served at `/bf1942/` should mirror the on-disk structure starting from the directory you intend to use as the local prefix:

```
/srv/fastdl/bf1942/
    Mods/
        bf1942/
            Archives/
                bf1942/
                    Levels/
                        El_Alamein.rfa
                        Wake.rfa
                        custom_map_1.rfa
                        custom_map_2.rfa
        dc_final/
            Archives/
                bf1942/
                    Levels/
                        dc_map_1.rfa
                        dc_map_2.rfa
```

**Client side**: point the local `Mods` directory at this remote root in the player's `.interposer\Config.yml`:

```yaml
FastDL:
  Enabled: true
  BaseUrl: 'http://fastdl.example.com/'
  Paths:
    - Local: 'C:\Games\Battlefield 1942\Mods'
      Remote: bf1942/Mods
```

When BF1942 tries to load `C:\Games\Battlefield 1942\Mods\dc_final\Archives\bf1942\Levels\dc_map_1.rfa`, the Interposer transparently fetches `http://fastdl.example.com/bf1942/Mods/dc_final/Archives/bf1942/Levels/dc_map_1.rfa`, drops it in the overlay cache, and hands the file to the game. The player joins the server with no manual download.

### Recommended Server Configuration

- **Serve from a stock static HTTP server.** No special application logic is required.
- **Match the game's path structure exactly.** The Interposer does not rewrite paths between client and server beyond the prefix swap; whatever the game asks for under the local prefix becomes the URL under the remote prefix.
- **Send `X-Checksum-CRC32` headers** when you can. Without them, files are downloaded once and cached forever. With them, the client transparently re-downloads on every change without administrator intervention. A future minimal web server as a companion to Interposer may be created to assist with this.
- **Restrict by extension on the client.** Set `FastDL.AllowedExtensions` so a misconfigured server cannot push, for example, a malicious `.exe` into the cache. For BF1942 a sensible whitelist would be `[.rfa, .con, .ssm]`.
- **Leave `BlockSensitiveFiles: true`.** This prevents the FastDL system from ever overwriting `Config.yml`, `Registry.reg`, the interposer DLL, the injector, or the game executable.
- **Consider `ProbeConnections` for LAN deployments.** If your FastDL server lives at the same address as the game's master/lobby server, set `FastDL.ProbeConnections: true` and leave `BaseUrl` empty. The Interposer will discover the server automatically by probing addresses the game has already connected to. This removes the need for every client to be configured with a hard-coded server address.

### Testing a FastDL Server

The simplest end-to-end test:

1. Spin up the HTTP server with the directory tree in place.
2. From any browser or `curl`, fetch one of the URLs directly. You should get the file back. If you get a `403` or `404`, the path layout on disk does not match what the client will request.
3. Delete the file from the *client's* game directory.
4. Launch the game with the Interposer attached and `Logging.Files: true` and `FastDL.LogDownloads: true` enabled.
5. Look for a `[FASTDL]` line in the session log followed by a `[FILE OVERLAY]` line on subsequent reads.

If the `[FASTDL]` line is missing, the file path the game is asking for is not under any configured `Paths` prefix. Check the `[FILE READ]` entries in the log to see the exact path the game is opening, and adjust the `Local` prefix to match.

## Registry Emulation: Use the Original Hive Path

[Registry emulation](/Interposer/RegistryEmulation) intercepts registry calls and serves them from a `.reg` file inside the game directory. Two Windows mechanisms quietly rewrite registry paths underneath the application, and they catch authors of `Registry.reg` files off guard often enough to deserve their own section.

### What WOW64 Registry Redirection Is and Why It Exists

When a 32-bit process runs on 64-bit Windows, its access to certain registry keys under `HKEY_LOCAL_MACHINE\SOFTWARE` is silently redirected by the kernel into a parallel namespace at `HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node`. The same is true for `HKEY_CLASSES_ROOT\CLSID` and a handful of related subtrees.

The reason is COM. A 32-bit application registering an in-process COM server records the path to a 32-bit DLL. A 64-bit application registering the same CLSID records the path to a 64-bit DLL. If both writes landed in the same key, whichever process ran second would overwrite the other and break it. The `Wow6432Node` shadow tree gives 32-bit processes their own reflection of `HKLM\SOFTWARE`, so 32-bit and 64-bit installations of the same component can coexist.

For a 32-bit game on 64-bit Windows, the practical effect is that any time the game writes to:

```
HKEY_LOCAL_MACHINE\SOFTWARE\<Publisher>\<Game>
```

the value actually lives at:

```
HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\<Publisher>\<Game>
```

even though the game itself sees the original path through every registry API.

A second mechanism, **UAC registry virtualization**, redirects writes to `HKLM\SOFTWARE\` from non-elevated 32-bit processes into a per-user shadow store at `HKEY_CURRENT_USER\Software\Classes\VirtualStore\MACHINE\SOFTWARE\...`. This is what stopped Vista from outright breaking every legacy installer and configuration tool that assumed it could write anywhere under `HKLM`.

### Author Your `Registry.reg` Against the Original Path

Both of these mechanisms produce the same headache: if you `regedit.exe → Export` a key from your machine to use as a starting `Registry.reg`, you may end up with the *virtualized* path (`Wow6432Node\...` or `VirtualStore\MACHINE\...`) instead of the path the game actually thinks it is writing to. The Interposer's path matching is exact, so a `Registry.reg` written against the virtualized path will not match the calls the game makes.

**The rule is: always write `Registry.reg` against the original, non-virtualized hive path the game's source code uses.**

Concretely, that means:

- Use `HKEY_LOCAL_MACHINE\SOFTWARE\<Publisher>\<Game>` — **not** `HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\<Publisher>\<Game>`.
- Use `HKEY_LOCAL_MACHINE\SOFTWARE\<Publisher>\<Game>` — **not** `HKEY_CURRENT_USER\Software\Classes\VirtualStore\MACHINE\SOFTWARE\<Publisher>\<Game>`.

The Interposer normalizes the WOW64 and `VirtualStore` path forms back to their canonical `HKLM\SOFTWARE\...` equivalents before matching them against the virtual registry, so a `Registry.reg` written against the original path will catch reads and writes regardless of which shadow tree the OS would have routed them through.

### Finding the Right Path

If you don't know the exact path a game uses, the most reliable workflow is:

1. Run the game **once with the Interposer attached**, with `Logging.Registry: true` set in `.interposer\Config.yml`.
2. Quit the game and inspect the session log under `.interposer\Logs\`.
3. Look for `[REG OPEN]`, `[REG READ]`, and `[REG WRITE]` entries. The Interposer logs the **canonical** path (with `Wow6432Node` and `VirtualStore` collapsed), so the path you see is exactly the path you should put in `Registry.reg`.
4. Create `.interposer\Registry.reg` containing `[<canonical path>]` and the values you observed, then re-launch.

### A Working Example

A 32-bit game wants to remember whether music is enabled. Its source code does:

```c
RegSetValueExW(hkey, L"Music", 0, REG_DWORD, (BYTE*)&value, 4);
```

against `HKEY_LOCAL_MACHINE\SOFTWARE\Acme\Wonder Game`. Run by a non-elevated user on 64-bit Windows, the OS quietly routes this all the way to `HKEY_CURRENT_USER\Software\Classes\VirtualStore\MACHINE\SOFTWARE\Wow6432Node\Acme\Wonder Game`. The session log, however, shows:

```
2026-04-11 19:00:01  [REG WRITE]  HKEY_LOCAL_MACHINE\SOFTWARE\ACME\WONDER GAME\MUSIC
```

Your `Registry.reg` should therefore contain:

```
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\Acme\Wonder Game]
"Music"=dword:00000001
```

The Interposer takes care of catching all three forms (canonical, `Wow6432Node`, `VirtualStore`) and serving them from this single entry.
