---
sidebar_label: Rich Presence
sidebar_position: 3
---

# Rich Presence Plugin API

Plugins can update the game's rich presence at runtime to show dynamic information such as the current map, score, player count, or match progress. The API is backend-agnostic — updates are pushed to all active backends (currently Discord, with more planned).

All exported functions use the C calling convention and UTF-8 strings. Resolve them at runtime via `GetProcAddress`, the same way as other Interposer plugin APIs.

## Resolving the API

Resolve the function pointers from the `HMODULE` passed to your `InterposerPluginInit` entry point:

```cpp
using FnSetDetails     = void (*)(const char* details);
using FnSetState       = void (*)(const char* state);
using FnSetTimestamps  = void (*)(int64_t start, int64_t end);
using FnSetLargeImage  = void (*)(const char* key, const char* text);
using FnSetSmallImage  = void (*)(const char* key, const char* text);
using FnSetParty       = void (*)(const char* id, int size, int max);
using FnSetButton      = void (*)(int index, const char* text, const char* url);
using FnSetType        = void (*)(int type);
using FnSetName        = void (*)(const char* name);
using FnUpdate         = void (*)();
using FnClear          = void (*)();

static FnSetDetails    pfnSetDetails    = nullptr;
static FnSetState      pfnSetState      = nullptr;
static FnSetTimestamps pfnSetTimestamps = nullptr;
static FnSetLargeImage pfnSetLargeImage = nullptr;
static FnSetSmallImage pfnSetSmallImage = nullptr;
static FnSetParty      pfnSetParty      = nullptr;
static FnSetButton     pfnSetButton     = nullptr;
static FnSetType       pfnSetType       = nullptr;
static FnSetName       pfnSetName       = nullptr;
static FnUpdate        pfnUpdate        = nullptr;
static FnClear         pfnClear         = nullptr;

static bool ResolvePresenceAPI(HMODULE hInterposer)
{
    pfnSetDetails    = (FnSetDetails)   GetProcAddress(hInterposer, "InterposerSetPresenceDetails");
    pfnSetState      = (FnSetState)     GetProcAddress(hInterposer, "InterposerSetPresenceState");
    pfnSetTimestamps = (FnSetTimestamps)GetProcAddress(hInterposer, "InterposerSetPresenceTimestamps");
    pfnSetLargeImage = (FnSetLargeImage)GetProcAddress(hInterposer, "InterposerSetPresenceLargeImage");
    pfnSetSmallImage = (FnSetSmallImage)GetProcAddress(hInterposer, "InterposerSetPresenceSmallImage");
    pfnSetParty      = (FnSetParty)     GetProcAddress(hInterposer, "InterposerSetPresenceParty");
    pfnSetButton     = (FnSetButton)    GetProcAddress(hInterposer, "InterposerSetPresenceButton");
    pfnSetType       = (FnSetType)      GetProcAddress(hInterposer, "InterposerSetPresenceType");
    pfnSetName       = (FnSetName)      GetProcAddress(hInterposer, "InterposerSetPresenceName");
    pfnUpdate        = (FnUpdate)       GetProcAddress(hInterposer, "InterposerUpdatePresence");
    pfnClear         = (FnClear)        GetProcAddress(hInterposer, "InterposerClearPresence");
    return pfnUpdate && pfnClear;
}
```

## API Reference

### `InterposerSetPresenceDetails`

```cpp
void InterposerSetPresenceDetails(const char* details);
```

Set the first detail line (e.g. `"Playing on de_dust2"`). Pass `""` or `nullptr` to clear.

---

### `InterposerSetPresenceState`

```cpp
void InterposerSetPresenceState(const char* state);
```

Set the second detail line (e.g. `"Score: 7 - 3"`). Pass `""` or `nullptr` to clear.

---

### `InterposerSetPresenceTimestamps`

```cpp
void InterposerSetPresenceTimestamps(int64_t start, int64_t end);
```

Set elapsed or remaining time. Both values are Unix epoch seconds.

- **Elapsed time** (counts up): set `start` to the match start time, `end` to `0`.
- **Remaining time** (counts down): set `start` to `0`, `end` to the match end time.
- **Clear**: set both to `0`.

```cpp
// Show elapsed time since now
#include <ctime>
pfnSetTimestamps(std::time(nullptr), 0);
pfnUpdate();
```

---

### `InterposerSetPresenceLargeImage`

```cpp
void InterposerSetPresenceLargeImage(const char* key, const char* text);
```

Set the large image. `key` is an asset key (uploaded in the Discord Developer Portal) or an `https://` URL. `text` is the tooltip shown on hover. Either can be `nullptr` to clear.

---

### `InterposerSetPresenceSmallImage`

```cpp
void InterposerSetPresenceSmallImage(const char* key, const char* text);
```

Set the small image overlay. Same semantics as `SetPresenceLargeImage`.

---

### `InterposerSetPresenceParty`

```cpp
void InterposerSetPresenceParty(const char* id, int size, int max);
```

Show party information (e.g. "2 of 8"). `id` is an arbitrary string identifying the party. `size` is the current member count, `max` is the maximum. Set `size` and `max` to `0` to clear.

---

### `InterposerSetPresenceButton`

```cpp
void InterposerSetPresenceButton(int index, const char* text, const char* url);
```

Set a clickable button. `index` is `0` for Button 1 or `1` for Button 2. Both `text` and `url` must be non-empty for the button to appear. `url` must be `http://` or `https://`. Discord limits button labels to 32 characters.

---

### `InterposerSetPresenceType`

```cpp
void InterposerSetPresenceType(int type);
```

Set the activity type: `0` Playing, `1` Streaming, `2` Listening, `3` Watching, `5` Competing.

---

### `InterposerSetPresenceName`

```cpp
void InterposerSetPresenceName(const char* name);
```

Set the display name shown as "Playing **name**".

---

### `InterposerUpdatePresence`

```cpp
void InterposerUpdatePresence();
```

Flush all pending field changes to every active backend. **You must call this after setting fields** — the setter functions only stage changes in memory. Nothing is sent to Discord until you call `InterposerUpdatePresence`.

---

### `InterposerClearPresence`

```cpp
void InterposerClearPresence();
```

Reset all fields to the defaults from `Config.yml` and clear the activity from all backends. Use this when returning to a menu or idle state.

## Usage Pattern

The typical pattern is:

1. Set one or more fields using the setter functions.
2. Call `InterposerUpdatePresence()` to push the changes.

Only call `InterposerUpdatePresence` once per logical state change — don't call it after every individual setter.

```cpp
// Player joined a match
pfnSetDetails("de_dust2 - Competitive");
pfnSetState("Score: 0 - 0");
pfnSetTimestamps(std::time(nullptr), 0);
pfnSetParty("lobby-abc", 5, 5);
pfnSetSmallImage("rank-gold", "Gold Nova");
pfnUpdate();  // one call sends everything
```

When the match ends:

```cpp
pfnClear();  // resets to Config.yml defaults and clears Discord activity
```

## Complete Example

```cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <ctime>

using FnSetDetails    = void (*)(const char*);
using FnSetState      = void (*)(const char*);
using FnSetTimestamps = void (*)(int64_t, int64_t);
using FnSetParty      = void (*)(const char*, int, int);
using FnUpdate        = void (*)();
using FnClear         = void (*)();

static FnSetDetails    pfnSetDetails    = nullptr;
static FnSetState      pfnSetState      = nullptr;
static FnSetTimestamps pfnSetTimestamps = nullptr;
static FnSetParty      pfnSetParty      = nullptr;
static FnUpdate        pfnUpdate        = nullptr;
static FnClear         pfnClear         = nullptr;

extern "C" __declspec(dllexport) void WINAPI InterposerPluginInit(HMODULE hInterposer)
{
    pfnSetDetails    = (FnSetDetails)   GetProcAddress(hInterposer, "InterposerSetPresenceDetails");
    pfnSetState      = (FnSetState)     GetProcAddress(hInterposer, "InterposerSetPresenceState");
    pfnSetTimestamps = (FnSetTimestamps)GetProcAddress(hInterposer, "InterposerSetPresenceTimestamps");
    pfnSetParty      = (FnSetParty)     GetProcAddress(hInterposer, "InterposerSetPresenceParty");
    pfnUpdate        = (FnUpdate)       GetProcAddress(hInterposer, "InterposerUpdatePresence");
    pfnClear         = (FnClear)        GetProcAddress(hInterposer, "InterposerClearPresence");

    if (!pfnUpdate) return;

    // Set initial presence
    pfnSetDetails("Joining server...");
    pfnSetTimestamps(std::time(nullptr), 0);
    pfnUpdate();
}

// Call this from your game-state hook when the map changes
void OnMapLoaded(const char* mapName, int playerCount, int maxPlayers)
{
    if (!pfnUpdate) return;

    pfnSetDetails(mapName);

    char stateBuf[64];
    wsprintfA(stateBuf, "Players: %d / %d", playerCount, maxPlayers);
    pfnSetState(stateBuf);

    pfnSetParty("server-1", playerCount, maxPlayers);
    pfnUpdate();
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
```

```yaml
# .interposer\Config.yml
RichPresence:
  Discord:
    Enabled: true
    ApplicationId: "123456789012345678"
  LargeImage: "game-logo"
  LargeImageText: "My Game"
  Button1Text: "Website"
  Button1Url: "https://example.com"
```

## Thread Safety

All rich presence API functions are thread-safe. They use internal locking, so you can call them from any thread without external synchronization.

## Notes

- If rich presence is not configured (no `RichPresence` section or `Discord.Enabled` is `false`), all API functions are safe to call but do nothing.
- The Interposer handles reconnection automatically. If Discord restarts, the next `InterposerUpdatePresence` call will reconnect and resend the current state.
- Presence updates are rate-limited by Discord to roughly one update every 15 seconds. Sending more frequently is safe but Discord will throttle them.
