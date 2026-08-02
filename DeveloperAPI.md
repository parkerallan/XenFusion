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
8. [Animator](#8-animator) — `Animator.SetFloat`, `Animator.SetBool`, `Animator.SetTrigger`, `Animator.SetState`
9. [Utility](#9-utility) — `log`
10. [Text](#10-text) — `text.set`
11. [GUI](#11-gui) — `gui.panel`, `gui.label`, `gui.image`, `gui.button`, `gui.set_focus`, `gui.set_paused`, `gui.clear`

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

### `on_trigger(entrant, bone_name)`
Runs on an object that has **both** a **Trigger Volume** attribute and this
script, the moment *another* object enters that volume. Fires **once per entry**
(not per frame, not on exit).

- **`self`** is the trigger (this object).
- **`entrant`** is the object that entered — a full [handle](#3-objects) with every
  [Physics](#4-physics) method plus `:name()` and `:id()`. (The parameter name is
  yours to choose; this doc calls it `entrant`.)
- **`bone_name`** is the animated bone name for a Collision bone modifier in
    Trigger mode. It is `nil` for a regular scene Trigger Volume. Existing
    one-argument handlers remain valid because Lua ignores unused arguments.

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

## 8. Animator

Control an object's **Animator** attribute and its authored controller. Animator
functions are globals taking the target object **name**, followed by a parameter
or state name. The target must have an Animator attribute with a valid controller;
otherwise the call does nothing.

The table name and method names are case-sensitive: use `Animator.SetBool`, not
`anim.set_bool`, `anim.set_state`, or `Engine.Animator.SetBool`.

| Function | Effect |
|---|---|
| `Animator.SetFloat(object, parameter, value)` | set a numeric controller parameter |
| `Animator.SetBool(object, parameter, value)` | set a boolean controller parameter |
| `Animator.SetTrigger(object, parameter)` | set a one-shot controller trigger |
| `Animator.SetState(object, state)` | switch directly to a state and restart it |

Parameter and state names should use the exact spelling and capitalization from
the Animator controller. Parameters persist until changed. Setting a float and a
bool with the same name replaces the previous parameter type.

### `Animator.SetFloat(object, parameter, value)`

Sets a numeric parameter used by transition conditions such as `speed > 0.2` or
`health <= 0`. Call it whenever the gameplay value changes, or every frame when
the value is derived from movement.

```lua
function on_update(dt)
    local vx, vy, vz = self:velocity()
    local speed = math.sqrt(vx * vx + vz * vz)
    Animator.SetFloat(self:name(), "speed", speed)
end
```

### `Animator.SetBool(object, parameter, value)`

Sets a persistent boolean parameter. A bare condition such as `isMoving` passes
when the value is `true`; `!isMoving` passes when it is `false`.

```lua
local move_speed = 6.0

function on_update(dt)
    local vx, vy, vz = self:velocity()
    local mx = input.axis("LX") * move_speed
    local mz = -input.axis("LY") * move_speed
    self:set_velocity(mx, vy, mz)

    local speed = math.sqrt(mx * mx + mz * mz)
    Animator.SetBool(self:name(), "isMoving", speed > 0.2)
    Animator.SetBool(self:name(), "isRunning", speed > 4.5)
end
```

For example, a controller can use these ordered transitions:

| From | To | Condition |
|---|---|---|
| `Survey` | `Walk` | `isMoving` |
| `Walk` | `Survey` | `!isMoving` |
| `Walk` | `Run` | `isRunning` |
| `Run` | `Walk` | `!isRunning` |

### `Animator.SetTrigger(object, parameter)`

Sets a one-shot trigger. A transition with a bare condition matching the trigger
name can consume it. The trigger remains set until a matching transition is
evaluated, so it can be fired before that transition's source state or exit time
is active.

```lua
local attack_was_down = false

function on_update(dt)
    local attack_is_down = input.button("X")
    if attack_is_down and not attack_was_down then
        Animator.SetTrigger(self:name(), "attack")
    end
    attack_was_down = attack_is_down
end
```

The edge check in this example prevents a held button from setting the trigger
again every frame.

### `Animator.SetState(object, state)`

Immediately switches to an existing controller state. The new state starts at
time zero, any active cross-fade is cancelled, and no transition condition or
exit time is evaluated. An unknown state name does nothing.

```lua
function on_start()
    Animator.SetState(self:name(), "Survey")
end
```

Prefer parameters and authored transitions for normal gameplay because they keep
the controller's blend duration, transition order, and exit-time behavior.
`SetState` is intended for hard overrides such as resets or teleports; do not call
it every frame, because each call restarts the state.

### Transition conditions

Each transition supports one condition expression. Logical `and`/`or` expressions
are not supported; use ordered transitions and intermediate states when more than
one decision is needed.

| Parameter kind | Supported condition forms |
|---|---|
| Always | empty condition |
| Boolean | `grounded`, `!grounded`, `grounded == true`, `grounded != false` |
| Float | `speed == 1`, `speed != 0`, `speed >= 4.5`, `speed <= 1`, `speed > 0.2`, `speed < 8` |
| Trigger | bare trigger name, such as `attack` |

Transitions are checked in controller order, and at most one transition occurs
per animation update. A transition with **Has Exit Time** enabled is considered
only after the active state's playback time reaches its exit time. When a
transition succeeds, its authored blend duration cross-fades the previous and new
clips.

Animator parameters written by `on_update` are evaluated on the next animation
update. This keeps behavior consistent between editor Play preview and the Xbox
360 runtime. Animator calls are transient and do not modify the saved controller
or scene.

---

## 9. Utility

### `log(msg)`
Print to the editor **Log** panel as a `[LOG]` entry (or the console's debug
output). Any value is coerced to a string; build messages with `..`. Script
*errors* show separately as `[ERROR]`.

```lua
log("health = " .. 100)
```

---

## 10. Text

### `text.set(name, value)`

Replaces the displayed value of the named object's first **Text** attribute.
The change is visible in the same frame and remains active for the current Play
session. It does not modify the saved scene; stopping editor Play restores the
authored value.

```lua
function on_update(dt)
    text.set("ScoreLabel", "Score: " .. 1250)
end
```

Text is UTF-8 input with Basic Latin and Latin-1 Supplement glyph support.
Unsupported or malformed characters render as `?`. Values are bounded to 4,096
UTF-8 bytes and 2,048 rendered glyphs.

---

## 11. GUI

Retained mode GUI to build menus from lua scripts. `gui.panel`, `gui.label`, `gui.image` and `gui.button`
each create a widget and return a **handle**; keep the handle to change or
destroy the widget later. Widgets can nest, and a child is positioned relative
to its parent.

Positions and sizes are in pixels in a **1280×720** space, origin top-left.
Menus work in the editor's **Play** preview and on the console. They are
**transient** — stopping the preview destroys them, and nothing is written to
the scene.

| Function | Effect |
|---|---|
| `gui.panel{ … }` → widget | a background plate — a texture, or a flat colour when it has none |
| `gui.label{ … }` → widget | a line of text, no background |
| `gui.image{ … }` → widget | a texture |
| `gui.button{ … }` → widget | a plate plus a caption; **focusable** by default |
| `gui.set_focus(widget)` | move the highlight to a widget |
| `gui.focus()` → widget | the focused widget, or `nil` |
| `gui.set_paused(b)` | pause gameplay while the menu stays live |
| `gui.is_paused()` → `boolean` | true while paused |
| `gui.root()` → widget | the screen — the default parent |
| `gui.clear()` | destroy every widget |

Each constructor takes one options table. Omitted options keep their default, so
`gui.label{ text = "Hi", font = F }` is valid.

```lua
local plate = gui.panel{ anchor = "center", w = 520, h = 380,
                         color = { 0.05, 0.06, 0.09, 0.88 } }

gui.label{ parent = plate, anchor = "top", y = 30, w = 520, h = 64,
           text = "PAUSED", font = "assets/menu.otf", size = 52, align = "center" }
```

### Widget options

| Option | Effect |
|---|---|
| `parent` | widget to nest inside (default: the screen) |
| `anchor` | which point of the parent to attach to — see **Anchoring** |
| `x`, `y` | offset from that anchor point |
| `w`, `h` | size |
| `color` | a label's text colour; every other widget's background |
| `focus_color` | background while focused |
| `text_color` · `focus_text_color` | caption colour on widgets that also have a background |
| `texture` | project-relative `.png` / `.jpg` |
| `slice` | 9-slice border in pixels — the border keeps its size, the middle stretches |
| `text` · `font` · `size` | caption, project-relative `.ttf` / `.otf`, pixel size |
| `align` · `valign` | `left` `center` `right` · `top` `middle` `bottom` |
| `wrap` | wrap text at the widget's width |
| `visible` · `enabled` · `focusable` | booleans; buttons are focusable unless you say otherwise |
| `on_confirm` · `on_cancel` · `on_focus` | callbacks, called with the widget |

On a button, `color` is the plate and `text_color` is the caption. Set both
pairs — with only `color`/`focus_color`, a focused button's caption renders in
the highlight colour and disappears.

### Anchoring

`anchor` names a point on the **parent**; the child's matching point is placed
there, then offset by `x`/`y`. A child moves when its parent moves.

| Anchor | Names |
|---|---|
| Top | `topleft` `top` `topright` |
| Middle | `left` `center` `right` |
| Bottom | `bottomleft` `bottom` `bottomright` |

```lua
gui.label{ anchor = "bottomright", x = -16, y = -16, w = 200, h = 40,
           text = "v1.0", font = F, align = "right" }   -- inset from the corner
```

### Widget methods

| Method | Effect |
|---|---|
| `:set_visible(b)` · `:set_enabled(b)` | hiding a widget hides its children too |
| `:set_pos(x, y)` · `:set_size(w, h)` · `:set_anchor(name)` | placement |
| `:set_color(r,g,b,a)` · `:set_focus_color(r,g,b,a)` | background / label colour |
| `:set_text_color(r,g,b,a)` · `:set_focus_text_color(r,g,b,a)` | caption colour |
| `:set_text(s)` · `:set_font(path)` · `:set_font_size(px)` | text |
| `:set_texture(path)` · `:set_align(h, v)` · `:set_wrap(b)` | appearance |
| `:set_focusable(b)` | make a non-button selectable, or a button not |
| `:text()` → `string` · `:visible()` → `boolean` · `:alive()` → `boolean` | read back |
| `:rect()` → `x, y, w, h` | where the widget ended up on screen |
| `:destroy()` | destroy the widget **and its children** |

Calls on a destroyed widget do nothing and `:alive()` reports `false`, so a
handle kept across a menu being torn down is safe to use.

### Navigation

Menus are driven by the controller. The D-pad and left stick move the highlight
to the nearest widget in that direction, **A** raises `on_confirm`, and **B**
raises `on_cancel`. A held direction repeats. Hidden, disabled and non-focusable
widgets are skipped, and the highlight stops at the edge of a menu rather than
wrapping.

Call `gui.set_focus` when a menu opens so the player sees a highlight straight
away. In the editor these read a gamepad plus whatever you bound in the
**Mapping** panel — a control with nothing bound never reports pressed. See
[Input](#5-input).

### Worked example: a pause menu

```lua
-- Attach as a Script attribute on any object. Opens on Play; LY moves the
-- highlight, A confirms.
local FONT = "assets/Typo_Round_Bold_Demo.otf"

local menu = nil

local function close_menu()
    if menu then
        menu:destroy()                     -- takes the labels and buttons with it
        menu = nil
    end
    gui.set_paused(false)
end

local function open_menu()
    if menu then return end

    menu = gui.panel{
        anchor = "center", w = 520, h = 380,
        color = { 0.05, 0.06, 0.09, 0.88 },
    }

    gui.label{                             -- child of the plate, moves with it
        parent = menu, anchor = "top", y = 30, w = 520, h = 64,
        text = "PAUSED", font = FONT, size = 52, align = "center",
        color = { 1.0, 0.55, 0.15, 1.0 },
    }

    local first = nil
    local captions = { "Resume", "Restart", "Quit" }
    for i = 1, #captions do
        local button = gui.button{
            parent = menu, anchor = "center",
            y = -60 + (i - 1) * 74, w = 400, h = 58,   -- one anchor, vary y
            text = captions[i], font = FONT, size = 30,
            color            = { 0.16, 0.17, 0.22, 1.0 },
            focus_color      = { 1.00, 0.55, 0.15, 1.0 },
            text_color       = { 1.00, 1.00, 1.00, 1.0 },
            focus_text_color = { 0.05, 0.05, 0.06, 1.0 },
            on_confirm = function(self)
                if self:text() == "Resume" then close_menu() end
            end,
            on_cancel = function() close_menu() end,
        }
        first = first or button
    end

    gui.set_focus(first)
    gui.set_paused(true)
end

function on_start()
    open_menu()
end
```

To open it from a button instead of at startup, edge-detect the press —
`input.button` reports a button as **held**, not as newly pressed:

```lua
local was_down = false

function on_update(dt)
    local down = input.button("Start")
    if down and not was_down then
        if menu then close_menu() else open_menu() end
    end
    was_down = down
end
```