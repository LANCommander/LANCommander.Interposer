# Borderless Fullscreen

The Interposer can convert game windows to borderless fullscreen when they are created. The game runs at its intended resolution but without a title bar, borders, or resize handles, and the window is centered on the monitor it appears on.

This feature is opt-in and must be enabled in `.interposer/Config.yml`.

## What It Does

When a game creates a top-level window, the Interposer:

1. Strips the window border styles (`WS_THICKFRAME`, `WS_DLGFRAME`, `WS_BORDER`)
2. Strips the extended border styles (`WS_EX_DLGMODALFRAME`, `WS_EX_WINDOWEDGE`, `WS_EX_CLIENTEDGE`, `WS_EX_STATICEDGE`)
3. Centers the window on the monitor it was created on

If the game later tries to re-apply border styles — which some engines do after window creation — those attempts are also intercepted and silently discarded.

## Why This Is Useful

Many older games only support exclusive fullscreen mode or windowed mode with a border. Exclusive fullscreen causes problems on modern multi-monitor setups and prevents Alt-Tab from working cleanly. Borderless fullscreen gives the game a full-screen appearance while behaving like a normal window to the operating system.

Common benefits:
- Clean Alt-Tab switching without the display mode flickering
- Window shares the desktop with other applications (useful for streaming or overlay software)
- Consistent behavior across different screen resolutions and DPI settings

## Window Detection

Not every window a game creates is treated as the main game window. The Interposer applies the borderless treatment only to windows that look like a primary game window:

- No parent window (top-level only — dialogs, splash screens, and child windows are left untouched)
- Positive width and height
- Created with `WS_OVERLAPPEDWINDOW` or `WS_POPUP` style bits

Splash screens and other short-lived windows are typically dismissed before the main window appears, so they are generally unaffected in practice.

## Hooked Functions

| Function | Purpose |
|---|---|
| `CreateWindowExW` / `CreateWindowExA` | Strip border styles at creation time; center the window. |
| `SetWindowPos` | Prevent the game from moving or resizing the window after creation. |
| `SetWindowLongW` / `SetWindowLongA` | Prevent the game from re-applying border styles after creation. |

## Enabling Borderless Fullscreen

Set `Borderless: true` in the `Window` section of `.interposer/Config.yml`:

```yaml
Window:
  Borderless: true
```

The default is `false` — if the `Window` section is absent the game window is left untouched.
