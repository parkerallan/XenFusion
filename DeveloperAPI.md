# Scripting Developer API

Gameplay is scripted in **Lua** (reference Lua 5.4). A script is a `.lua` file
attached to a scene object via a **Script** attribute. The same script runs in the
editor's **Play** preview and on the **Xbox 360** runtime, so behavior matches.

## Contents

1. [Overview](#1-overview)
2. [Lifecycle](#2-lifecycle) — `on_start`, `on_update`, `on_trigger`
3. [Objects](#3-objects) — `self`, `find`, `:id`
4. [Physics](#4-physics) — `apply_impulse`, `set_velocity`, `set_transform`, `position`, `velocity`
5. [Input](#5-input) — `input.button`, `input.axis`, name reference
6. [Video](#6-video) — `video.play`, `video.stop`, `video.is_playing`
7. [Audio](#7-audio) — `audio.play`, `audio.stop`, `audio.is_playing`, `audio.set_volume`, `audio.set_pitch`, `audio.set_loop`
8. [Utility](#8-utility) — `log`

---

## 1. Overview

**Attach a script**
1. Assets panel → right-click → **New Script** (keep it in `assets/scripts`).
2. Select an object → **Add Attribute → Script** → drag the `.lua` onto it.
3. Add a Dynamic **Rigid Body** if the object should move under physics.
4. **Play** in the editor (drive with WASD / Space), or Build-and-Run for the
   console (gamepad).

**How scripts run**
- You define the [lifecycle](#2-lifecycle) functions; the engine calls them.
- `on_update(dt)` runs **before** the physics step each frame, so any impulse,
  velocity, or transform you set there takes effect that same frame.
- Scripts are sandboxed: `math`, `string`, `table`, `coroutine`, `utf8` are
  available; `io`, `os`, and `require` are **not** (no file/OS access on console).

---

## 2. Lifecycle

Functions you *define* in a script; all are optional.

### `on_start()`
Runs once, when Play begins (editor) or the title boots (console).

```lua
function on_start()
    log("player ready")     -- one-time setup / initialise variables
end
```

### `on_update(dt)`
Runs every frame, before the physics step. `dt` = seconds since the last frame
(use it to stay frame-rate independent).

```lua
function on_update(dt)
    -- If this object falls off the world, reset it to the start.
    local x, y, z = self:position()
    if y < -20 then
        self:set_transform(0, 5, 0)
    end
end
```

### `on_trigger(entrant)`
Runs on an object that has **both** a **Trigger Volume** attribute and this
script, the moment *another* object enters that volume. Fires **once per entry**
(not per frame, not on exit).

- **`self`** is the trigger (this object).
- **`entrant`** is the object that entered — a full [handle](#3-objects) with every
  [Physics](#4-physics) method plus `:name()` and `:id()`. (The parameter name is
  yours to choose; this doc calls it `entrant`.)

```lua
-- Jump pad: launch whatever enters this trigger straight up.
function on_trigger(entrant)
    entrant:apply_impulse(0, 15, 0)
    log(entrant:name() .. " hit the jump pad")
end
```

React to a specific object by **name** (this is the check you usually want):

```lua
function on_trigger(entrant)
    if entrant:name() == "fox" then
        log("the fox reached the goal!")
    end
end
```

> The overlap is bounding-box level, so `entrant` can be any object whose box
> overlaps the volume — filter by `:name()` (or `:id()`) if you only care about
> some.

---

## 3. Objects

Scripts act on **object handles**. You get a handle two ways:

- **`self`** — the object this script is attached to.
- **`find(name)`** — look up another scene object by name; returns a handle, or
  `nil` if there's no object with that name.

Every handle has the [Physics](#4-physics) methods plus:

### `handle:name()` → `string`
The object's name (as shown in the editor). The usual way to identify who entered
a trigger or which object `find` returned.

```lua
function on_trigger(entrant)
    if entrant:name() == "fox" then log("caught the fox") end
end
```

### `handle:id()` → `number`
The object's index in the scene. A cheaper identity check than the name if you've
cached it.

```lua
log("my id is " .. self:id())
```

```lua
-- find() example: chase another object.
function on_update(dt)
    local goal = find("Goal")
    if goal then
        local gx = goal:position()             -- first return value = x
        local x, y, z    = self:position()
        local vx, vy, vz = self:velocity()
        self:set_velocity((gx - x) > 0 and 3 or -3, vy, 0)
    end
end
```

---

## 4. Physics

Methods called on an [object handle](#3-objects) (`self`, or a `find` result).
They require a **Rigid Body** attribute on that object — otherwise they do
nothing. Coordinates are world-space; rotations are in **degrees**.

| Method | Effect |
|---|---|
| `handle:apply_impulse(x, y, z)` | instantaneous kick (jump/knockback) |
| `handle:set_velocity(x, y, z)` | set linear velocity (movement) |
| `handle:set_transform(x, y, z [, rx, ry, rz])` | teleport / drive position (+rotation) |
| `handle:position()` → `x, y, z` | current world position |
| `handle:velocity()` → `x, y, z` | current linear velocity |

### `apply_impulse(x, y, z)`
An instantaneous force. Best on a **Dynamic** body. This is how you jump. Wakes a
sleeping body.

```lua
function on_update(dt)
    -- Jump when A is pressed and we're roughly grounded.
    local vx, vy, vz = self:velocity()
    if input.button("A") and math.abs(vy) < 0.5 then
        self:apply_impulse(0, 7, 0)
    end
end
```

### `set_velocity(x, y, z)`
Set linear velocity directly (units/second). Read the current velocity first if
you want to keep an axis (e.g. preserve gravity on Y).

```lua
function on_update(dt)
    local vx, vy, vz = self:velocity()
    -- Horizontal from the stick; keep physics-driven vertical velocity.
    self:set_velocity(input.axis("LX") * 6, vy, -input.axis("LY") * 6)
end
```

### `set_transform(x, y, z [, rx, ry, rz])`
Move (and optionally rotate) the object. It's a **teleport** for a Dynamic body
(clears its velocity), and the way to drive a **Kinematic** body each frame (a
Kinematic body pushes Dynamic objects it hits). Rotation args are optional.

```lua
function on_update(dt)
    local x, y, z = self:position()
    self:set_transform(x + input.axis("LX") * 4 * dt, y, z)  -- move a platform on X
    -- self:set_transform(x, y, z, 0, 90, 0)                 -- with a 90° Y rotation
end
```

### `position()` → `x, y, z`  ·  `velocity()` → `x, y, z`
Query the object's current world position / linear velocity (three return values
each).

```lua
local x, y, z    = self:position()
local vx, vy, vz = self:velocity()
local speed = math.sqrt(vx*vx + vz*vz)
```

> **Body type guide:** use **Dynamic** for `apply_impulse` / `set_velocity`
> (physics moves it), **Kinematic** for continuous `set_transform` driving (you
> move it, it shoves others). On a **Static** body these are no-ops.

---

## 5. Input

Scripts always poll **Xbox controls** (`"A"`, `"LX"`, …). On the console these read
a real gamepad. In the editor's Play preview they read a gamepad too, **plus** any
PC keyboard/mouse you've bound to a control in the **Mapping** panel
(View → Mapping) — so you can test without a controller. Binding PC inputs is an
editor testing convenience only; it changes nothing about the script or the
console build.

### `input.button(name)` → `boolean`
True while the named button is held.

```lua
if input.button("A") then self:apply_impulse(0, 7, 0) end
```

### `input.axis(name)` → `number`
Analog value: sticks `-1..1`, triggers `0..1`. Stick deadzone is applied.

```lua
local move = input.axis("LX")   -- -1 (left) .. +1 (right)
```

### Name reference

| Kind | Names |
|---|---|
| Buttons | `A` `B` `X` `Y` `LB` `RB` `Start` `Back` `LS` `RS` `DPadUp` `DPadDown` `DPadLeft` `DPadRight` |
| Axes | `LX` `LY` `RX` `RY` (sticks) · `LT` `RT` (triggers) |

**Editor testing (Mapping panel).** Open **View → Mapping** to bind PC keys/mouse
to Xbox controls so you can drive scripts without a gamepad. Columns: the Xbox
control, the exact call it answers (e.g. `input.button("A")`, `input.axis("LX")`),
and the PC input you bind. **Nothing is bound by default** — click a binding and
press a key or mouse button (Esc clears; right-click for mouse-move options). A
stick axis has a `-` and `+` row (bind one key to each side). Mappings save per
project (`input_mappings.ini`); the console ignores them.

---

## 6. Video

Control an object's **Video** attribute (the screen-space video overlay — see
`runtime/VIDEO.md` for the format and attribute reference). These are globals
taking an object **name** (like `find`), not handle methods. On an object with
no Video attribute they do nothing / return `false`.

Video plays in the editor's **Play** preview and on the console; edit mode
always shows the frozen first frame. Changes made here are **transient** — the
scene's authored Play Mode is never modified, and stopping the editor preview
restores it.

| Function | Effect |
|---|---|
| `video.play(name [, loop])` | play the object's video **from the beginning** — once, or looping when `loop` is `true` |
| `video.stop(name)` | stop and hide it (releases the decoder) |
| `video.is_playing(name)` → `boolean` | true while it's running |

### `video.play(name [, loop])`
Starts playback from the first frame with the attribute's volume/mute.
Plays **once** by default (`is_playing` goes `false` after the last frame);
pass `true` as the second argument to loop until stopped. Calling it on a
video that already finished or was stopped **replays it from the top**; on one
that is currently playing it only switches once/loop (no restart).

A play-once video **hides itself** when it ends (after its last frame has
displayed) — fire and forget. The usual cutscene pattern: author the attribute
with **Play Mode: Off** so nothing shows at boot, then trigger it from
gameplay:

```lua
-- Attach to an object with a Trigger Volume + this script; the object
-- "Cutscene" has a Video attribute authored with Play Mode = Off.
function on_trigger(entrant)
    if entrant:name() == "fox" then
        video.play("Cutscene")            -- plays once, hides itself when done
    end
end
```

```lua
video.play("Menu", true)                  -- background loop until video.stop
```

### `video.stop(name)`
Stops playback and hides the overlay. The decoder is released — a later
`video.play` starts over from the beginning (there is no pause/resume).

```lua
function on_update(dt)
    if input.button("B") then
        video.stop("Cutscene")   -- skip the cutscene
    end
end
```

### `video.is_playing(name)` → `boolean`
`true` while the video is running. A **Loop** video reports `true` until
stopped; a **Play Once** video reports `false` after its last frame — the way
to know a cutscene finished:

```lua
local started = false

function on_update(dt)
    if input.button("Start") and not started then
        started = true
        video.play("Intro")
    end
    if started and not video.is_playing("Intro") then
        log("intro over - back to gameplay")
        started = false
    end
end
```

> A `video.play`/`video.stop` is visible to `video.is_playing` in the same
> frame, but the overlay itself changes on the next drawn frame. See
> `assets/scripts/videotoggle.lua` in the test project for a runnable example.

---

## 7. Audio

Control an object's **Audio** attribute (a 2D or 3D-positional sound source —
see `runtime/AUDIO.md` for the format and attribute reference). Globals taking
an object **name**, like the `video` table. On an object with no Audio
attribute they do nothing / return `false`.

Audio plays in the editor's **Play** preview and on the console; edit mode is
always silent. Changes are **transient** — the authored attribute values are
never modified, and stopping the preview restores them. A **3D-spatialized**
source pans and fades with the camera automatically; scripts only control
what plays.

| Function | Effect |
|---|---|
| `audio.play(name)` | start the clip **from the beginning** (restarts a stopped/finished one) |
| `audio.stop(name)` | silence it (releases the voice) |
| `audio.is_playing(name)` → `boolean` | true while it's audible (a non-looping clip reports false after it ends) |
| `audio.set_volume(name, v)` | linear gain, `0`–`20` |
| `audio.set_pitch(name, v)` | playback rate, `0.1`–`4` (also speeds/slows the clip) |
| `audio.set_loop(name, b)` | toggle looping (applies live) |

```lua
-- One-shot triggered SFX: the object "Chime" has an Audio attribute authored
-- with Play Mode = Off and Loop = off.
function on_trigger(entrant)
    if entrant:name() == "fox" then
        audio.play("Chime")               -- plays once; is_playing goes false after
    end
end
```

```lua
-- Engine hum that follows speed: pitch rides the object's velocity.
function on_update(dt)
    local vx, vy, vz = self:velocity()
    local speed = math.sqrt(vx*vx + vy*vy + vz*vz)
    audio.set_pitch("EngineHum", 0.8 + speed * 0.05)
end
```

> `audio.play` on a clip that is already playing does nothing; to restart one
> mid-play, `audio.stop` it and `audio.play` on a later frame. Whether a clip
> loops comes from the attribute (or `audio.set_loop`) — `play` doesn't take a
> loop argument like `video.play` does.

---

## 8. Utility

### `log(msg)`
Print to the editor **Log** panel as a `[LOG]` entry (or the console's debug
output). Any value is coerced to a string; build messages with `..`. Script
*errors* show separately as `[ERROR]`.

```lua
log("health = " .. 100)
```

---