---
sidebar_label: Rich Presence
sidebar_position: 8
---

# Rich Presence

Rich presence lets games display detailed status information in Discord (and potentially other platforms in the future). When configured, the Interposer connects to Discord's local IPC pipe and sets the game's activity automatically on startup.

## Prerequisites

1. **Create a Discord Application** at the [Discord Developer Portal](https://discord.com/developers/applications). You only need the **Application ID** — no bot token is required.
2. **Upload assets** (optional) — if you want custom images, upload them under **Rich Presence > Art Assets** in the developer portal. The asset names you assign there are what you use for `LargeImage` and `SmallImage` below.

## Configuration

Add a `RichPresence` section to `.interposer\Config.yml`:

```yaml
RichPresence:
  Discord:
    Enabled: true
    ApplicationId: "123456789012345678"

  # Default activity shown when the game starts.
  # Plugins can override any of these fields at runtime.
  Type: 0
  Name: "My Game"
  Details: "In Menu"
  State: "Idle"
  LargeImage: "logo"
  LargeImageText: "My Game v1.0"
  SmallImage: ""
  SmallImageText: ""
  Button1Text: ""
  Button1Url: ""
  Button2Text: ""
  Button2Url: ""
```

### Discord Settings

| Key | Type | Description |
|---|---|---|
| `Enabled` | bool | Set to `true` to connect to Discord on startup. Default: `false`. |
| `ApplicationId` | string | Your Discord application's ID (found in the developer portal). Required. |

### Default Activity Fields

These fields define the presence shown immediately when the game starts. All are optional — omit any you don't need.

| Key | Type | Description |
|---|---|---|
| `Type` | int | Activity type: `0` Playing, `1` Streaming, `2` Listening, `3` Watching, `5` Competing. Default: `0`. |
| `Name` | string | Display name shown as "Playing **Name**". |
| `Details` | string | First line of detail text below the game name. |
| `State` | string | Second line of detail text. |
| `LargeImage` | string | Asset key (uploaded in the developer portal) or an `https://` URL for the large image. |
| `LargeImageText` | string | Tooltip shown when hovering over the large image. |
| `SmallImage` | string | Asset key or URL for the small image overlay. |
| `SmallImageText` | string | Tooltip shown when hovering over the small image. |
| `Button1Text` | string | Label for the first button (max 32 characters). |
| `Button1Url` | string | URL opened when the first button is clicked. Must be `http://` or `https://`. |
| `Button2Text` | string | Label for the second button. |
| `Button2Url` | string | URL opened when the second button is clicked. |

:::note Buttons require both text and URL
A button only appears if **both** its text and URL are set. Setting only the text or only the URL has no effect.
:::

## How It Works

1. During DLL initialization (after hooks are installed), the Interposer reads the `RichPresence` section from `Config.yml`.
2. If `Discord.Enabled` is `true`, it opens a local named pipe (`\\.\pipe\discord-ipc-N`) and performs the IPC handshake using the configured Application ID.
3. The default activity is sent immediately after connecting.
4. If Discord is not running, a log entry is written and the game continues normally — rich presence is entirely optional and never blocks the game.
5. If Discord restarts while the game is running, the next presence update (from a plugin) will automatically reconnect.

## Verifying

Look for `[RICH PRESENCE]` entries in the session log:

```
2025-06-01 14:30:45  [RICH PRESENCE]    Discord IPC connected
```

If Discord is not running, you'll see:

```
2025-06-01 14:30:45  [RICH PRESENCE]    Discord IPC connection failed (is Discord running?)
```

## Examples

### Minimal — just show "Playing My Game"

```yaml
RichPresence:
  Discord:
    Enabled: true
    ApplicationId: "123456789012345678"
```

The application name configured in the Discord Developer Portal is used as the game name when no other fields are set.

### Full presence with images and buttons

```yaml
RichPresence:
  Discord:
    Enabled: true
    ApplicationId: "123456789012345678"
  Type: 0
  Details: "Main Menu"
  State: "Waiting for players"
  LargeImage: "game-logo"
  LargeImageText: "My Game v2.1"
  SmallImage: "faction-blue"
  SmallImageText: "Blue Team"
  Button1Text: "Join Server"
  Button1Url: "https://example.com/join"
  Button2Text: "Download Game"
  Button2Url: "https://example.com/download"
```

### Plugin-driven presence

If a plugin is installed that updates rich presence at runtime (e.g. showing the current map or score), you only need the Discord connection settings in the config. The plugin handles the rest:

```yaml
RichPresence:
  Discord:
    Enabled: true
    ApplicationId: "123456789012345678"
  LargeImage: "game-logo"
  LargeImageText: "My Game"
```

See [Rich Presence Plugin API](/Interposer/Plugins/RichPresence) for details on writing plugins that update presence.

## Extensibility

The rich presence system is designed with multiple backends in mind. Discord is the first supported backend, but the architecture allows additional backends (e.g. a LANCommander-native presence service) to be added in the future. All backends receive the same presence data — plugins don't need to know which backends are active.
