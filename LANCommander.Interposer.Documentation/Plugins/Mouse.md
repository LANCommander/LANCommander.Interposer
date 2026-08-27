---
sidebar_label: Mouse
---

# Mouse Plugin

`LANCommander.Interposer.Plugin.Mouse.dll` rewrites the buffered mouse stream on
its way from DirectInput to the game. It fixes two things that older engines get
wrong on modern hardware: stair-stepped mouse look, and an axis ratio the game
gives you no way to correct.

Both were needed to make Aliens versus Predator 2 playable, and both are general
enough to apply to other titles of that era.

## Stair-stepping

DirectInput buffered mode hands the game a queue of many small axis events.
Engines of this era drain that queue with `GetDeviceData(want = 1)` and apply one
event per frame. A 1000 Hz mouse produces roughly sixteen events per frame at
60 fps, so the game consumes one sixteenth of your movement per frame and takes
a quarter of a second to catch up with a flick — which reads as the view
stuttering or "stepping" rather than tracking the mouse.

With `CoalesceAxisEvents` on, every X event in a batch is summed into a single X
event and every Y event into a single Y event. The game accumulates deltas
anyway, so the sum is exactly equivalent — it just arrives when the movement
happened instead of trickling out over the following frames.

Button and wheel events pass through untouched, in their original order.

## Axis ratio

Some engines have an X:Y sensitivity ratio that cannot be reached from their own
configuration. AvP2 is the worked example:

- LithFix applies `nScaleY *= 2` for AvP2 and no other LithTech game, i.e. an
  intended X:Y ratio of 1:2.
- The engine's own scales are `0.002711` and `0.002983` — a ratio of 1.10.
- So the correction needed is `2 / 1.10 = 1.82`.

Editing the engine's `scale "##mouse" "##y-axis"` line achieves nothing, because
the engine rewrites those lines from its internal state every time it shuts down
cleanly. They are output, not input. Scaling the event data works, because
nothing overwrites it.

`XMultiplier` and `YMultiplier` scale the deltas directly. A fractional
remainder is carried per axis, so slow movement is not repeatedly truncated to
zero: at 1.82, a raw delta of 1 becomes 2, not 1.

## Configuration

```yaml
Plugins:
  Mouse:
    CoalesceAxisEvents: 'true'
    XMultiplier: '1.00'
    YMultiplier: '1.00'
```

| Option | Default | Meaning |
|---|---|---|
| `CoalesceAxisEvents` | `true` | Sum each axis into a single event per read |
| `XMultiplier` | `1.00` | Horizontal scale; `1.00` leaves the axis alone |
| `YMultiplier` | `1.00` | Vertical scale |

Values are quoted strings — plugin configuration is read as text.

Multipliers must be greater than 0 and no more than 100; anything outside that
falls back to `1.00` rather than silently disabling the mouse. Numbers are
parsed independently of the process locale, so `1.82` means 1.82 even in a game
that has set a comma-decimal locale.

If coalescing is off and both multipliers are `1.00`, the plugin does not
register at all and the input path is left completely untouched.

### For Aliens versus Predator 2

```yaml
DirectInput:
  FixLegacyDeviceEnumeration: true
  DeviceFilter:
    Enabled: true
    Classes: ['Mouse', 'Keyboard']

Plugins:
  Mouse:
    CoalesceAxisEvents: 'true'
    XMultiplier: '1.00'
    YMultiplier: '1.82'
```

## Installing

Copy `LANCommander.Interposer.Plugin.Mouse.dll` from the release archive's
`Plugins\` directory into `.interposer\Plugins\` next to the Interposer DLL, then
add the `Plugins.Mouse` section above. The plugin writes its defaults into
`Config.yml` on first run if the section is missing.

Use the architecture that matches the game, the same as for the Interposer
itself.

:::caution Requires the DirectInput fix
The Interposer only routes the mouse through a transform for games it is
bridging, so this plugin needs `DirectInput.FixLegacyDeviceEnumeration: true`.
Games that drive DirectInput 8 natively are not affected by it.
:::

## Checking it is working

Set `Logging.Plugins: true` to confirm the plugin loaded and what it read:

```
[MOUSE]           coalesce=on  X=1.00  Y=1.82
```

Set `Logging.DirectInput: true` and `Logging.Level: Debug` to confirm the
Interposer identified the axes in the game's data format. Without this line the
plugin has nothing to act on and passes every batch through unchanged:

```
[DINPUT FORMAT]   mouse axis offsets  ->  X=12  Y=16  Z=20  objects=7
```
