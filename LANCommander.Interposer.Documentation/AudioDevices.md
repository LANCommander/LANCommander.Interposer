---
sidebar_label: Audio Devices
sidebar_position: 11
---

# Audio Devices

Games take the first audio device Windows reports, or the system default, and stick with it. That was fine when a machine had one sound card. On a modern one it means a capture card, a virtual audio cable, a streaming mixer, an idle HDMI output on a monitor with no speakers, or a headset that was unplugged at launch can all quietly become "the" device — and the player hears nothing, or the whole LAN hears their microphone.

The `AudioDevices` section presents the game a filtered view of the machine's audio devices, output and input alike, so that it lands on the ones you want without disabling or reordering anything on the host.

```yaml
AudioDevices:
  Enabled: true
  Output:
    - 'Speakers (NVIDIA Broadcast)'
    - 'Speakers (High Definition Audio Device)'
    - 'USB\VID_0965&PID_9001'
  Input:
    - 'Voicemeeter Input (VB-Audio Voicemeeter VAIO)'
    - 'USB\VID_0965&PID_9001'
```

## How It Works

There is one list per direction. Each entry names a device, and a device is **kept if any entry in its direction's list matches it**. Everything else is hidden from the enumeration APIs.

Whether a direction is filtered at all is decided by whether its key is **present**, not by what is in it:

| | Meaning |
|---|---|
| key omitted | That direction is not filtered — every device is shown |
| key present, empty list | Every device of that direction is hidden |
| key present with entries | Only devices matching an entry are shown |

So `Input: []` hides every microphone from the game, while leaving `Input` out altogether leaves microphones alone. If neither key is present the feature is a no-op regardless of `Enabled`, and nothing is hooked at all — leaving the section at its defaults costs the game nothing.

| Option | Meaning |
|---|---|
| `Enabled` | Apply the lists below. `false` by default. |
| `Output` | Playback devices the game may see — speakers, headphones, HDMI |
| `Input` | Recording devices the game may see — microphones, line-in |

:::caution Entries are literal text, not regular expressions
Unlike [`NetworkAdapters.Names`](/Interposer/NetworkAdapters) and [`DirectInput.DeviceFilter.Names`](/Interposer/DirectInput#filtering-devices), these entries are matched **case-insensitively as a plain substring**. That is deliberate: the strings you paste in here — `Speakers (NVIDIA Broadcast)`, `USB\VID_0965&PID_9001` — are full of characters a regex would read as syntax, and you should not have to escape them.
:::

## Naming a Device

Each entry is compared against **every string the device is known by**, so you can name a device whichever way you have it written down:

| Write | Example | Where to find it |
|---|---|---|
| The friendly name | `Speakers (High Definition Audio Device)` | Windows sound settings |
| The short description | `Speakers` | Windows sound settings |
| A **hardware ID** | `USB\VID_0965&PID_9001` | Device Manager → the device → Details → Hardware Ids |
| The device instance path | `USB\VID_0965&PID_9001\5&1a2b3c&0&2` | Device Manager → Details → Device instance path |
| The Core Audio endpoint ID | `{0.0.0.00000000}.{a1b2c3d4-...}` | `InterposerEnumAudioDevices`, or a `[AUDIO]` log line |
| The DirectSound device GUID | `{def00000-9c6d-47ed-aaf1-4dda8f2b5c03}` | A DirectSound device picker |

The friendly name is the obvious choice, and the right one most of the time.

**Prefer a hardware ID when the name is unstable.** Friendly names change when a driver updates, when Windows renumbers two identical devices, or when a monitor with a different model name is plugged into the same output. A hardware ID identifies the physical device and survives all of that; the instance path additionally pins the port it is plugged into, which is how you tell two identical headsets apart.

Because matching is a substring, a short entry can match more than you meant — `USB` on its own would match every USB device on the machine. Paste in enough to be unique. Conversely you can deliberately shorten an entry: `NVIDIA Broadcast` matches both `Speakers (NVIDIA Broadcast)` and `Microphone (NVIDIA Broadcast)`, in their respective lists.

### How one entry reaches three different APIs

The three ways a game can enumerate audio report wildly different amounts about the same device. winmm reports a name truncated to 31 characters and no identifier at all; DirectSound reports a description and a GUID; only Core Audio reports everything.

To make one entry work everywhere, the Interposer builds an **identity table** from Core Audio the first time it filters anything, and resolves the other two APIs back into it — winmm through the endpoint ID hidden in its device interface path, DirectSound through the endpoint GUID. Hardware IDs come from walking the device tree upwards, because an endpoint's own instance path says nothing about the hardware but its parents do.

The practical consequence is that `USB\VID_0965&PID_9001` filters a winmm game correctly even though winmm has never heard of hardware IDs. If Core Audio cannot be reached at all, the filter degrades to matching on whatever name the game's own API reported, and says so in the log.

## Coverage

- **winmm** — `waveOutGetNumDevs`, `waveOutGetDevCapsA/W`, `waveOutOpen` and the four `waveIn` equivalents
- **dsound** — `DirectSoundEnumerateA/W`, `DirectSoundCaptureEnumerateA/W`
- **Core Audio** — `IMMDeviceEnumerator::EnumAudioEndpoints`, `GetDefaultAudioEndpoint` and `GetDevice`, reached through `CoCreateInstance`

Core Audio was introduced in Windows Vista. On Windows XP only winmm and DirectSound exist, and neither reports endpoint IDs, instance paths or hardware IDs, so entries can only match a device's name there — and winmm truncates names to 31 characters.

### Renumbering and the default device

The winmm APIs address devices by index, so hiding one renumbers the rest. `waveOutGetNumDevs` reports how many devices survived the filter, and every call that takes an index translates the game's index back to the real one. A game that walks `0..waveOutGetNumDevs()-1` sees a contiguous list of exactly the devices you allowed.

Games that never enumerate at all — the ones that just ask for "the default device" — are steered too. When the filter hides at least one device of that direction:

- `waveOutOpen`/`waveInOpen` with `WAVE_MAPPER` open the first allowed device instead. If that device rejects the format the game asked for, the call falls back to `WAVE_MAPPER`, because the mapper also performs format negotiation and sound on the wrong device beats no sound at all.
- `GetDefaultAudioEndpoint` returns the first allowed endpoint when the system default is hidden.

When the filter hides nothing of that direction, both are left alone: the system default is visible anyway, and steering it would override the user's own Windows setting for no reason.

DirectSound's enumeration always begins with a "Primary Sound Driver" entry that stands for the system default. It is filtered like any other device, which is usually what you want — a game that takes the first entry then lands on the device you chose rather than on the system default. Add `Primary Sound Driver` to the list to keep it.

## Logging

Set `Logging.Audio: true` to record enumeration activity in the session log, and `Logging.Level: Debug` to list each device and the verdict on it:

```
2026-09-06 15:28:08  [AUDIO]           identity table built  ->  25 endpoints
2026-09-06 15:28:08  [AUDIO FOUND]     Speakers (High Definition Audio Device)  ->  Output  matched Speakers (High Definition Audio Device)
2026-09-06 15:28:08  [AUDIO FOUND]     Speakers (NVIDIA Broadcast)  ->  Output  matched USB\VID_0956&PID_9001
2026-09-06 15:28:08  [AUDIO HIDE]      Digital Audio (S/PDIF) (High Definition Audio Device)  ->  Output  no entry matched
2026-09-06 15:28:08  [AUDIO ENUM]      waveOutGetNumDevs  ->  2 of 14 output devices shown
2026-09-06 15:28:08  [AUDIO HIDE]      Microphone (NVIDIA Broadcast)  ->  Input  no devices are allowed in this direction
2026-09-06 15:28:08  [AUDIO ENUM]      waveInGetNumDevs  ->  0 of 11 input devices shown
```

`[AUDIO FOUND]` names the entry that kept each device, which is the quickest way to confirm a rule is doing what you meant. The `[AUDIO ENUM]` verb tells you which API the game used — the first thing to check when a filter appears to have had no effect.

At `Logging.Level: Trace` an `[AUDIO RULE]` line is written for every entry evaluated and rejected, for working out why an entry did not match. Expect one line per device per entry.

## Reading the Device List

Plugins and host applications can enumerate the canonical device list, with each device's allowed/hidden status, through the exported `InterposerEnumAudioDevices`. In .NET this is surfaced as `InterposerService.EnumerateAudioDevices()` returning `AudioDeviceInfo` records:

| Property | Meaning |
|---|---|
| `Name` | The device's friendly name, e.g. `Speakers (Realtek High Definition Audio)` |
| `Description` | The device's description, e.g. `Speakers` |
| `Id` | The Core Audio endpoint ID |
| `InstanceId` | The device instance path |
| `HardwareId` | The most specific hardware ID of the physical device behind the endpoint |
| `Direction` | `Output`, `Input`, or `Unknown` |
| `IsDefault` | `true` if this is the system default endpoint for its direction |
| `Allowed` | `true` if the device passes the configured filter, `false` if it is hidden from the game |

This reports the real device set regardless of the filter, and every string in the record is one an entry will match on — so a picker UI can list every device, show which ones the game currently sees, and hand the user the exact text to paste into their config.
