# FastDL

FastDL (Fast Download) automatically downloads game content from an HTTP server when the game tries to open a file that is missing or out of date. The download is transparent to the game — by the time `CreateFileW` returns, the file is already present on disk.

This feature was originally inspired by Valve's FastDL system for Half-Life / Goldsrc servers, which allowed servers to distribute custom content (maps, textures, sounds) to clients. The Interposer's FastDL works similarly but is server-engine agnostic: any game that opens files through `CreateFileW` can benefit from it.

## How It Works

1. The game calls `CreateFileW` to open a file for reading.
2. The file hook checks whether the path falls under a configured `fastDLPaths` prefix.
3. If it does, the hook sends a HEAD request to the FastDL server to check whether the file exists.
4. If the server has the file, it is downloaded (GET request) to the overlay directory.
5. The hook opens the downloaded file and returns that handle to the game.

If the server returns a `X-Checksum-CRC32` header, the Interposer compares it against the local file's checksum. If the checksums match, no re-download occurs — only changed files are fetched.

## Enabling FastDL

Set `Enabled: true` and configure a base URL in `.interposer/Config.yml`:

```yaml
FastDL:
  Enabled: true
  BaseUrl: 'http://fastdl.lan/'
```

`BaseUrl` must end with a trailing slash and must be reachable from the client machine over HTTP or HTTPS.

## Mapping Local Paths to the Server

`Paths` maps local directory prefixes to sub-paths under `BaseUrl`. Only files beneath a configured prefix are eligible for download — paths that don't match any prefix are ignored.

```yaml
FastDL:
  Paths:
    - Local: 'C:\Games\Quake3\baseq3'
      Remote: baseq3
```

With this configuration, when the game opens `C:\Games\Quake3\baseq3\maps\q3dm1.bsp`, the Interposer fetches:

```
http://fastdl.lan/baseq3/maps/q3dm1.bsp
```

Multiple mappings are supported:

```yaml
FastDL:
  Paths:
    - Local: 'C:\Games\Quake3\baseq3'
      Remote: baseq3
    - Local: 'C:\Games\Quake3\team0'
      Remote: team0
```

Leave `remote` empty to map directly under `baseUrl`:

```yaml
FastDL:
  Paths:
    - Local: 'C:\Games\MyGame\assets'
      Remote: ''
```

This would fetch `C:\Games\MyGame\assets\textures\wall.tga` from `http://fastdl.lan/textures/wall.tga`.

## Overlay Directory

By default, downloaded files are written to a `.interposer\Downloads\` folder inside the DLL's directory, not directly into the game directory. This keeps the game's own files unmodified.

```yaml
FastDL:
  UseDownloadDirectory: true      # default: true
  DownloadDirectory: ''           # empty = <dlldir>\.interposer\Downloads
```

To specify a different location:

```yaml
FastDL:
  DownloadDirectory: 'C:\FastDLCache\MyGame'
```

Relative paths are resolved relative to the DLL's directory. The overlay directory is checked on every file open before downloading — if a file already exists there, the cached version is served immediately without a network request.

:::caution Direct game directory downloads
Setting `UseDownloadDirectory: false` causes downloads to overwrite files directly in the game directory. This is not recommended — it can corrupt the local game installation, and there is no way to distinguish downloaded files from original game files.
:::

## File Extension Filtering

To restrict downloads to specific file types, set `AllowedExtensions`. An empty list (the default) allows all file types:

```yaml
FastDL:
  AllowedExtensions: [.pk3, .bsp, .wav, .tga]
```

Leading dots are optional: `.pk3` and `pk3` are treated the same way.

## Freshness Checking with CRC32

If the FastDL HTTP server includes a `X-Checksum-CRC32` response header on GET requests, the Interposer will compare the server's checksum against the locally cached file. If the checksums match, the cached file is used and no download occurs. If they differ, the file is re-downloaded.

Without the header, the Interposer downloads the file whenever it is not present in the overlay cache. It does not re-download files that are already cached.

To serve the CRC32 header with nginx:

```nginx
location /fastdl/ {
    add_header X-Checksum-CRC32 $upstream_http_x_checksum_crc32;
}
```

The CRC32 value should be the unsigned decimal integer of the ISO 3309 / ITU-T V.42 checksum of the file.

## Overriding the Base URL at Launch

To change the FastDL server URL without editing `.interposer/Config.yml`, pass `--fastdl-url` to the injector:

```
Injector.exe --fastdl-url http://192.168.1.10/ --launch "C:\Games\Quake3\quake3.exe"
```

The URL is passed to the DLL via a named memory-mapped file (`Local\InterposerFastDL_<pid>`) and overrides the `BaseUrl` in the config. This is useful when the server address changes between sessions or when the same game installation is used with different servers.

## Sensitive File Protection

By default, FastDL will not overwrite the following files even if the server offers them:

- `LANCommander.Interposer.dll` and the DLL it was loaded from
- `.interposer\Config.yml`
- `.interposer\Registry.reg`
- The injector executables (`LANCommander.Interposer.Injector.exe`, `Injector.exe`)
- The game's own executable

This protection is on by default and can only be disabled explicitly:

```yaml
FastDL:
  BlockSensitiveFiles: false    # not recommended
```

## Logging Downloads

FastDL operations are logged to the main log file when `logDownloads` is enabled (it is on by default):

```yaml
fastDL:
  logDownloads: true
```

Download log lines use the `[FASTDL]` verb:

```
2025-03-14 12:00:03  [FASTDL]       http://fastdl.lan/baseq3/maps/q3dm1.bsp  ->  C:\Games\Quake3\.interposer\Downloads\baseq3\maps\q3dm1.bsp
```

When a file is served from the overlay cache without re-downloading, the line uses the `[FILE OVERLAY]` verb:

```
2025-03-14 12:00:04  [FILE OVERLAY]  C:\Games\Quake3\baseq3\maps\q3dm1.bsp  ->  C:\Games\Quake3\.interposer\Downloads\baseq3\maps\q3dm1.bsp
```

## Full FastDL Configuration Reference

```yaml
FastDL:
  Enabled: false                    # Set to true to enable FastDL
  BaseUrl: 'http://fastdl.lan/'     # Root URL; trailing slash required
  LogDownloads: true                # Log download attempts
  AllowedExtensions: []             # Whitelist of extensions; empty = allow all
  UseDownloadDirectory: true        # Write to overlay dir (recommended)
  DownloadDirectory: ''             # Empty = <dlldir>\.interposer\Downloads
  BlockSensitiveFiles: true         # Prevent overwriting interposer files
  Paths:
    - Local: 'C:\Games\Quake3\baseq3'
      Remote: baseq3
```
