---
sidebar_label: DirectInput
sidebar_position: 9
---

# DirectInput

The `DirectInput` section covers two things: a compatibility fix for games that ask for DirectInput 3 or 7, and a device filter that controls which input devices a game is allowed to see.

```yaml
DirectInput:
  FixLegacyDeviceEnumeration: false

  DeviceFilter:
    Enabled: false
    Classes: []
    Names:   []
```

## Fixing legacy device enumeration

Some games from the DirectInput 3 / 7 era hang or crash on startup on modern Windows, before they ever reach a menu. The cause is usually not the game: it is a bug in the legacy `dinput.dll` that ships with Windows.

### The problem

While enumerating the HID device set, the legacy `dinput.dll` walks an internal array of 116-byte device records and never checks the bound:

```
10012928: mov  0xc4(%edi),%edx        ; edx = cursor into record array
1001292e: lea  0x74(%edx),%eax        ; stride 116 bytes
10012931: mov  %eax,0xc4(%edi)        ; bump cursor -- NO bounds check
10012937: mov  0x94(%edi),%eax
1001293d: mov  %edx,0x4(%eax,%esi,1)
10012941: movl $0x2710,0x40(%edx)     ; <<< writes past the end of the block
```

On a machine with enough HID devices attached, that write lands outside the allocation. What the player sees depends on luck: usually a hang at startup, sometimes an access violation. Enabling full page heap turns the silent overrun into an immediate fault at `dinput.dll+0x12941`, which is how it was pinned down.

The behaviour reproduces in a ~70-line program that only calls `DirectInputCreateA(hinst, 0x0700, ...)` followed by `EnumDevices` — no game code involved:

| Test | Result |
|---|---|
| DirectInput **7**, no page heap | hangs |
| DirectInput **7**, with page heap | crashes on the out-of-bounds write |
| DirectInput **8**, no page heap | clean — 20 devices enumerated, `S_OK` |
| DirectInput **8**, with page heap | clean |

DirectInput 8 walks the identical devices without trouble. A game that asks for version `0x0300` or `0x0700` lands on the broken path and has no way to opt out.

Filtering devices inside the enumeration callback does not help — by the time your callback runs, `dinput.dll` has already walked the whole HID stack internally. Only avoiding the legacy implementation fixes it.

### The fix

With `FixLegacyDeviceEnumeration: true`, the Interposer serves `DirectInputCreateA` and `DirectInputCreateEx` from `dinput8.dll` and hands the game an object it can drive as `IDirectInput3A` or `IDirectInput7A`.

This works because the vtable layouts line up: `IDirectInput7A` matches `IDirectInput8A` for slots 0–8, and the device interfaces match for slots 0–28. Three things are translated on the way through:

- **`QueryInterface` aliasing.** After `CreateDevice`, games of this era immediately ask the device for `IID_IDirectInputDevice2A` — that is the interface version where `Poll()` lives. A DirectInput 8 object answers `E_NOINTERFACE`, and the game then silently discards the device. The bridge answers the pre-DX8 device IIDs (`IDirectInputDeviceA`, `...Device2A`, `...Device7A`) and the pre-DX8 `IDirectInput` IIDs with the same object plus an `AddRef`, which is safe precisely because the vtables are layout-compatible.

- **`GetDeviceData` conversion.** `DIDEVICEOBJECTDATA` grew from 16 to 20 bytes in DirectX 8. A pre-DX8 game passes `cbObjectData = 16`, which DirectInput 8 rejects outright — measured at 6907 calls with zero successes. The bridge converts the records in both directions.

- **`CreateDevice`** still goes to DirectInput 8 for real, so the game gets genuine mouse and keyboard input.

Everything else forwards to DirectInput 8 untouched.

:::note ANSI only
Only the ANSI entry points are bridged. `DirectInputCreateW` is always passed through to the real `dinput.dll`, and `DirectInputCreateEx` is passed through when it asks for a Unicode interface. Every affected game seen so far uses the ANSI API.
:::

### Getting it loaded

Games of this era very often import `dinput.dll` and nothing else that can be proxied — no `version.dll`, no `dinput8.dll` — so the usual load methods cannot reach them. Use the **dinput.dll** load method: `Loader.Method: DInput` on the redistributable, or the `Proxy.DInput` release ZIP for a manual install.

That build replaces `dinput.dll` next to the game executable. When the fix is enabled it serves the exports itself and the broken system `dinput.dll` is never loaded at all. When the fix is disabled it is a plain passthrough, and the rest of the Interposer still applies.

The fix also works under the injector, the `version.dll` proxy and the ASI loader, where it installs hooks over the real `dinput.dll` exports instead. That path does map the system DLL into the process, but its enumeration code is never entered.

:::caution The game may have saved a broken config
Games that rewrite their own key bindings on exit — the LithTech engine is one — will write back a config with bindings missing if they ran with broken input, and each subsequent run erodes it further. The symptom is that menus work and the mouse buttons work, but the mouse axes and keyboard do nothing. That is a leftover config problem, not a DirectInput one, and it outlives the fix. Restore the game's config from a backup if input is still dead after enabling the fix.
:::

## Filtering devices

`DeviceFilter` controls which devices a game sees when it enumerates them. It applies both to games bridged by the fix above and to games that use DirectInput 8 natively.

This is an **allow-list**, the same model as network adapter filtering: a device is **kept if it matches any** configured filter, and every non-matching device is hidden from the enumeration. If both lists are empty the feature is a no-op and every device is shown unchanged.

| Option | Meaning |
|---|---|
| `Enabled` | Apply the filters below. `false` by default. |
| `Classes` | Device classes to allow |
| `Names` | Case-insensitive ECMAScript regex matched against each device's instance name **and** product name |

### Why filter at all

Games of a certain age pick the first device that looks plausible and bind to it for the rest of the session. A machine with a racing wheel, a flight stick, an RGB keyboard that also enumerates as a game controller, and a streaming deck gives such a game many chances to pick wrong — and often no way to change its mind. Hiding everything but what the game should use is usually faster than fighting its device-selection code.

### Classes

| Class | Devices |
|---|---|
| `Mouse` | Mice and other pointing devices |
| `Keyboard` | Keyboards |
| `Joystick` | Joysticks |
| `Gamepad` | Gamepads and game controllers |
| `Driving` | Steering wheels and pedal sets |
| `Flight` | Flight sticks, yokes and throttles |
| `FirstPerson` | Controllers built for first-person games |
| `ScreenPointer` | Light guns, touch screens and digitizers |
| `Remote` | Remote controls |
| `DeviceControl` | Devices that control other devices, such as a hardware control panel |
| `Supplemental` | Add-ons that supplement another device, such as separate rudder pedals or a throttle quadrant |
| `Device` | Anything that fits no other class |

Names are case-insensitive. An unrecognized class name is reported in the session log and ignored, so a typo does not silently hide the class you meant to allow.

The class is read from the low byte of the device's `dwDevType`. DirectInput 3/7 only ever distinguished the first four — `Device`, `Mouse`, `Keyboard` and `Joystick` — and the rest arrived with DirectInput 8. The two eras number their types differently, but the ranges do not overlap, so the same class names work for bridged and native games alike.

### Names

```yaml
DirectInput:
  DeviceFilter:
    Enabled: true
    Classes: ['Mouse', 'Keyboard']
    Names:   ['^Wireless Controller$']
```

Partial matches count, so anchor with `^` and `$` for exact matching. A device is kept if the pattern matches either its instance name or its product name.

### Skipping the enumeration entirely

Listing nothing but `Mouse` and/or `Keyboard`, with no `Names` rules, has a useful side effect: those two devices need no enumeration to be created, so the Interposer answers the enumeration directly instead of running it.

That matters because a real enumeration opens and classifies the whole HID stack, and the cost does not depend on what was asked for:

| Configuration | Startup to menu |
|---|---|
| One `EnumDevices` call (all devices) | 22.0 s |
| One call (game controllers only) | 21.9 s |
| Two calls (pointer + keyboard) | 41.9 s |
| Answered without enumerating | 3.9 s |

So the recommended configuration for a DirectInput 3/7 game that only needs mouse and keyboard is:

```yaml
DirectInput:
  FixLegacyDeviceEnumeration: true

  DeviceFilter:
    Enabled: true
    Classes: ['Mouse', 'Keyboard']
```

Adding any `Names` rule, or any class beyond those two, turns the shortcut off — an arbitrary device can only be found by really enumerating.

## Logging

Set `Logging.DirectInput: true` to record DirectInput activity in the session log:

```
2026-08-26 19:31:02  [DINPUT]          DirectInputCreateA
2026-08-26 19:31:02  [DINPUT BRIDGE]   created IDirectInput8A  ->  enumeration answered from the filter
2026-08-26 19:31:02  [DINPUT ENUM]     enumerated devices  ->  2 found, 2 shown
2026-08-26 19:31:02  [DINPUT DEVICE]   CreateDevice  ->  GUID_SysMouse
2026-08-26 19:31:02  [DINPUT DEVICE]   QueryInterface  ->  aliased pre-DX8 device IID
```

Seeing both `CreateDevice` lines and the aliased `QueryInterface` is the signal that the game accepted the devices. If the aliasing line is missing, expect working menus and no in-game input.

### Listing every device

Raising `Logging.Level` to `Debug` adds a line per device that the enumeration turned up, and a line for each one the filter removed with the reason why:

```
[DINPUT FOUND]    Mouse                  ->  class=Mouse  type=0x00000112
[DINPUT FOUND]    HID Keyboard Device    ->  class=Keyboard  type=0x00000113
[DINPUT FOUND]    Wireless Controller    ->  class=Gamepad  type=0x00010115
[DINPUT HIDDEN]   Wireless Controller    ->  no class or name rule matched
[DINPUT ENUM]     enumerated devices     ->  3 found, 2 shown
```

This is the quickest way to find out what a device actually calls itself before writing a `Names` pattern for it, and to confirm a rule is doing what you expect.

`Trace` additionally logs each `Names` pattern that was evaluated and rejected for a device, in the same style as file redirect rules:

```
[DINPUT RULE]     Wireless Controller    ->  no match: ^Xbox
```

Both are gated on `Logging.DirectInput` as well as the level, and a busy HID stack produces a line per device per enumeration call, so leave them off unless you are diagnosing something.
