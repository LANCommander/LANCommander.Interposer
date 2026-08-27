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

Some games from the DirectInput 3 / 7 era hang or crash on startup on Windows, before they ever reach a menu. This is a bug in the legacy `dinput.dll` that ships with modern versions Windows.

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

On a machine with enough HID devices attached, that write lands outside the allocation. This usually results a hang at startup or an access violation.

### The fix

With `FixLegacyDeviceEnumeration: true`, the Interposer serves `DirectInputCreateA` and `DirectInputCreateEx` from `dinput8.dll` and hands the game an object it can drive as `IDirectInput3A` or `IDirectInput7A`.

:::note ANSI only
Only the ANSI entry points are bridged. `DirectInputCreateW` is always passed through to the real `dinput.dll`, and `DirectInputCreateEx` is passed through when it asks for a Unicode interface. Every affected game seen so far uses the ANSI API.
:::

### Getting it loaded

Games of this era very often import `dinput.dll` and nothing else that can be proxied so the usual load methods cannot reach them. Use the **dinput.dll** load method: `Loader.Method: DInput` on the redistributable, or copy `dinput.dll` out of the release ZIP's architecture directory for a manual install.

## Filtering devices

`DeviceFilter` controls which devices a game sees when it enumerates them. It applies both to games bridged by the fix above and to games that use DirectInput 8 natively.

This is an **allow-list**, the same model as network adapter filtering: a device is **kept if it matches any** configured filter, and every non-matching device is hidden from the enumeration. If both lists are empty the feature is a no-op and every device is shown unchanged.

| Option | Meaning |
|---|---|
| `Enabled` | Apply the filters below. `false` by default. |
| `Classes` | Device classes to allow |
| `Names` | Case-insensitive ECMAScript regex matched against each device's instance name **and** product name |

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

Names are case-insensitive.

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

Tthe recommended configuration for a DirectInput 3/7 game that only needs mouse and keyboard is:

```yaml
DirectInput:
  FixLegacyDeviceEnumeration: true

  DeviceFilter:
    Enabled: true
    Classes: ['Mouse', 'Keyboard']
```

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
