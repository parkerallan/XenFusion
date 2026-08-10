# Scripting Developer API

Gameplay is scripted in **Lua** (reference Lua 5.4). A script is a `.lua` file
attached to a scene object via a **Script** attribute. The same script runs in the
editor's **Play** preview and on the **Xbox 360** runtime, so behavior matches.

## Contents

1. [Overview](#1-overview)
2. [Lifecycle](#2-lifecycle) — `on_start`, `on_update`, `on_trigger`, `on_trigger_stay`, `on_trigger_exit`, `on_collision`, `on_destroy`
3. [Objects](#3-objects) — `self`, `find`, `find_all`, `find_by_prefix`, `find_by_tag`, `:id`, `:alive`
4. [Physics](#4-physics) — `apply_impulse`, `apply_force`, `apply_torque`, `set_velocity`, `set_angular_velocity`, `physics.raycast`
5. [Transform & visibility](#5-transform--visibility) — `:position`, `:set_position`, `:rotation`, `:set_rotation`, `:scale`, `:set_scale`, `:show`, `:hide`
6. [Attributes](#6-attributes) — `:get`, `:set`, `:attr_count`, `:attr_type`, field reference
7. [Tags](#7-tags) — `:tags`, `:has_tag`, `:add_tag`, `:remove_tag`
8. [Time & timers](#8-time--timers) — `time.delta`, `time.total`, `time.after`, `time.every`, `time.cancel`
9. [Events](#9-events) — `event.on`, `event.emit`
10. [Spawning](#10-spawning) — `spawn`, `:destroy`
11. [Scenes & camera](#11-scenes--camera) — `scene.load`, `scene.is_loading`, `scene.progress`, `scene.name`, `camera.set_active`, `camera.active`
12. [Input](#12-input) — `input.button`, `input.pressed`, `input.released`, `input.axis`, name reference
13. [Video](#13-video) — `video.play`, `video.stop`, `video.is_playing`
14. [Audio](#14-audio) — `audio.play`, `audio.stop`, `audio.is_playing`, `audio.set_volume`, `audio.set_pitch`, `audio.set_loop`
15. [Animator](#15-animator) — `Animator.SetFloat`, `Animator.SetBool`, `Animator.SetTrigger`, `Animator.SetState`
16. [Utility](#16-utility) — `log`
17. [Text](#17-text) — `text.set`
18. [GUI](#18-gui) — `gui.panel`, `gui.label`, `gui.image`, `gui.button`, `gui.root`, `gui.set_focus`, `gui.set_play_mode`, `gui.focus`, `gui.set_paused`, `gui.is_paused`, `gui.clear`
19. [Animated GIFs](#19-animated-gifs) — `image_play_mode`, limits

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

**The frame, in order**

1. [Timers](#8-time--timers) that came due fire.
2. GUI events (confirm/cancel/focus) reach their callbacks.
3. Every `on_update(dt)`.
4. Objects [spawned or destroyed](#10-spawning) this frame are applied, and a
   running [`scene.load`](#11-scenes--camera) advances (handing over when ready).
5. Physics steps; trigger and collision callbacks fire.
6. The frame is drawn.

Anything a script changed — a transform, a light, an overlay, visibility — is
re-derived before drawing, so it shows up on the same frame you changed it.

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

### `on_trigger_stay(entrant, bone_name)` · `on_trigger_exit(entrant, bone_name)`
Same arguments as `on_trigger`, for the rest of the overlap. `on_trigger_stay`
runs **every frame** the object remains inside; `on_trigger_exit` runs once when
it leaves. `on_trigger` itself is still entry-only, so existing scripts are
unaffected.

```lua
-- Damage over time while standing in the fire, and stop when they leave.
function on_trigger_stay(entrant, _)
    if entrant:name() == "player" then event.emit("damage", 5 * time.delta) end
end
function on_trigger_exit(entrant)
    log(entrant:name() .. " got out")
end
```

### `on_collision(other, phase)`
Runs on **both** objects when two ordinary (non-trigger) rigid bodies touch.
`other` is the far object's handle; `phase` is `"enter"`, `"stay"` or `"exit"`.
Unlike a Trigger Volume this is a real contact, so it only fires for objects that
actually collide.

```lua
function on_collision(other, phase)
    if phase == "enter" then
        audio.play("ImpactSfx")
        log("hit " .. other:name())
    end
end
```

### `on_destroy()`
Runs just before the object is removed by [`:destroy()`](#10-spawning). `self` is
still valid, so the handler can still read the object and notify others.

```lua
function on_destroy()
    event.emit("enemy_died", self:name())
end
```

> Handlers you don't define cost nothing — the engine checks before calling, so
> a script that only wants `on_update` pays for nothing else.

---

## 3. Objects

Scripts act on **object handles**. You get a handle two ways:

- **`self`** — the object this script is attached to.
- **`find(name)`** — look up another scene object by name; returns a handle, or
  `nil` if there's no object with that name.
- **`find_all()`** — every live object in the scene, as an array.
- **`find_by_prefix(p)`** — every object whose name starts with `p`.
- **`find_by_tag(t)`** — every object carrying [tag](#7-tags) `t`.
- **`spawn(...)`** — a freshly cloned object (see [Spawning](#10-spawning)).

The search functions skip destroyed objects and return an array you can iterate:

```lua
for _, e in ipairs(find_by_prefix("Enemy")) do e:hide() end
```

Every handle has the [Physics](#4-physics),
[Transform](#5-transform--visibility), [Attribute](#6-attributes) and
[Tag](#7-tags) methods, plus:

### `handle:name()` → `string`
The object's name (as shown in the editor). The usual way to identify who entered
a trigger or which object `find` returned.

```lua
function on_trigger(entrant)
    if entrant:name() == "fox" then log("caught the fox") end
end
```

### `handle:id()` → `number`
The object's slot in the scene. A cheaper identity check than the name if you've
cached it. Reported even for a destroyed object, so it stays usable as a table key.

```lua
log("my id is " .. self:id())
```

### `handle:alive()` → `boolean`
Whether the handle still refers to the object it was made for. Only meaningful
once you [destroy](#10-spawning) things — check it before using a handle you
stored earlier.

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
These require a **Rigid Body** attribute on that object — otherwise they do
nothing. (Position, rotation and scale work on *any* object; those live in
[Transform & visibility](#5-transform--visibility).) Coordinates are world-space;
rotations are in **degrees**.

| Method | Effect |
|---|---|
| `handle:apply_impulse(x, y, z)` | instantaneous kick (jump/knockback) |
| `handle:apply_force(x, y, z)` | continuous force for this step (scale by `dt` yourself) |
| `handle:apply_torque(x, y, z)` | continuous twist for this step |
| `handle:set_velocity(x, y, z)` | set linear velocity (movement) |
| `handle:velocity()` → `x, y, z` | current linear velocity |
| `handle:set_angular_velocity(x, y, z)` | set spin, radians/second per world axis |
| `handle:angular_velocity()` → `x, y, z` | current spin |
| `handle:set_transform(x, y, z [, rx, ry, rz])` | teleport / drive position (+rotation) |
| `handle:position()` → `x, y, z` | current world position |

### `physics.raycast(ox, oy, oz, dx, dy, dz [, maxDistance])`
Fires a ray from `(ox, oy, oz)` along `(dx, dy, dz)` — which need not be
normalized — and returns the **closest solid hit**, or `nil`. `maxDistance`
defaults to 1000. Trigger volumes and animated bone colliders are ignored: a
raycast asks what solid geometry is in the way, not which triggers you are
standing in.

The hit table:

| Field | Meaning |
|---|---|
| `object` | handle of what was hit |
| `distance` | distance from the ray origin |
| `x`, `y`, `z` | world-space hit point |
| `nx`, `ny`, `nz` | surface normal, pointing back toward the origin |

```lua
-- Ground check: is there floor within half a unit below us?
function on_update(dt)
    local x, y, z = self:position()
    local hit = physics.raycast(x, y, z, 0, -1, 0, 0.5)
    grounded = hit ~= nil
    if hit then log("standing on " .. hit.object:name()) end
end
```

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

On an object with **no** Rigid Body this now moves the object in the scene
instead of doing nothing — but prefer `:set_position()` there, since it leaves
rotation alone rather than resetting it to zero.

```lua
function on_update(dt)
    local x, y, z = self:position()
    self:set_transform(x + input.axis("LX") * 4 * dt, y, z)  -- move a platform on X
    -- self:set_transform(x, y, z, 0, 90, 0)                 -- with a 90° Y rotation
end
```

### `position()` → `x, y, z`  ·  `velocity()` → `x, y, z`
Query the object's current world position / linear velocity (three return values
each). `position()` works on any object; `velocity()` needs a Rigid Body.

```lua
local x, y, z    = self:position()
local vx, vy, vz = self:velocity()
local speed = math.sqrt(vx*vx + vz*vz)
```

> **Body type guide:** use **Dynamic** for `apply_impulse` / `set_velocity`
> (physics moves it), **Kinematic** for continuous `set_transform` driving (you
> move it, it shoves others). On a **Static** body these are no-ops.

---

## 5. Transform & visibility

Every handle carries its transform, whether or not the object is simulated:

| Method | Effect |
|---|---|
| `handle:position()` → `x, y, z` | current world position |
| `handle:set_position(x, y, z)` | move it (rotation and scale unchanged) |
| `handle:rotation()` → `rx, ry, rz` | current rotation, degrees |
| `handle:set_rotation(rx, ry, rz)` | re-aim it in place |
| `handle:scale()` → `sx, sy, sz` | current scale |
| `handle:set_scale(s)` / `(sx, sy, sz)` | one argument scales uniformly |
| `handle:visible()` → `boolean` | is it being drawn |
| `handle:set_visible(b)` · `:show()` · `:hide()` | show/hide without destroying |

**Where the transform lives.** If the object has a **Rigid Body**, the simulation
owns its position and rotation — writes go to the body (a teleport for Dynamic, a
drive for Kinematic) exactly as `set_transform` always did. If it does *not*, the
write goes to the scene, so ordinary props, lights and cameras can be moved too.
This is the one thing that silently did nothing before. Scale is never simulated,
so it always belongs to the scene.

```lua
-- Open a door: slide it, no Rigid Body needed.
function on_update(dt)
    local door = find("Door")
    local x, y, z = door:position()
    if opening and y < 4 then door:set_position(x, y + dt * 2, z) end
end
```

Hiding an object keeps it loaded — its mesh stays streamed in and its animation
keeps running — so `:show()` is instant and resumes mid-animation. A hidden
object contributes no light and no overlay, exactly as if its **Visible**
checkbox were off.

---

## 6. Attributes

Two methods reach **every** field of every attribute. Field names are listed in
full below — note they are **not** the keys you see inside a `.scene` file
(the JSON stores a light's brightness as `intensity`; the script name is
`light_intensity`, prefixed so every attribute's fields stay distinct):

### `handle:get(field [, attrIndex])`
Returns the field's value, or `nil` if the object has no such field. Vector
fields return three values.

### `handle:set(field, value [, v2, v3] [, attrIndex])` → `boolean`
Writes it, returning `false` for an unknown field. A vector field accepts three
components, or one applied to all three.

```lua
local lamp = find("Lamp")
lamp:set("light_intensity", lamp:get("light_intensity") * 0.5)  -- dim it
lamp:set("light_color", 1, 0.4, 0.1)                            -- warm it
find("Fader"):set("image_alpha", 0.0)                           -- fade an overlay
```

**Which attribute?** Objects can hold several. With no `attrIndex`, the engine
picks the first attribute whose value for that field is not the default — which
is almost always the one you configured. Pass a 1-based `attrIndex` to be exact:

```lua
log(obj:attr_count())        -- how many attributes
log(obj:attr_type(2))        -- e.g. "Point Light"
obj:set("cam_fov", 70, 3)    -- the Camera is attribute 3
```

Writes are **live only**: in the editor they vanish on Stop and never dirty the
project; on the console they last for the session.

### Field reference

| Attribute | Fields |
|---|---|
| 3D Model | `model_path`, `cast_shadow` |
| Shader | `shader_path` |
| Animator | `animator_controller_path`, `animator_initial_state`, `animator_playback_speed`, `animator_auto_play` |
| Camera | `cam_fov`, `cam_near`, `cam_far`, `cam_active`, `cam_type`, `cam_follow_target`, `cam_follow_offset`, `cam_follow_orbit`, `cam_follow_rot_offset`, `cam_follow_lock`, `cam_follow_smoothing`, `cam_track_speed`, `cam_track_accel`, `cam_track_rot_offset` |
| Lights (all four) | `light_color`, `light_intensity`, `light_range`, `light_inner_deg`, `light_outer_deg`, `light_mode`, `light_volumetric`, `light_volumetric_intensity` |
| Rigid Body | `phys_kind`, `phys_shape`, `phys_size`, `phys_mass`, `phys_lin_damping`, `phys_ang_damping`, `phys_restitution`, `phys_friction`, `phys_gravity`, `phys_gravity_scale`, `phys_lock_rotation` |
| Trigger Volume | `trig_shape`, `trig_size` |
| Image | `image_path`, `image_x`, `image_y`, `image_w`, `image_h`, `image_stretch`, `image_lock_aspect`, `image_tint`, `image_alpha`, `image_priority`, `image_play_mode` |
| Color | `color_x`, `color_y`, `color_w`, `color_h`, `color_stretch`, `color_lock_aspect`, `color_rgb`, `color_alpha`, `color_priority` |
| Skybox | `sky_path`, `sky_rotation` |
| Text | `text_font_path`, `text_value`, `text_x`, `text_y`, `text_w`, `text_h`, `text_font_size`, `text_color`, `text_alpha`, `text_lock_aspect`, `text_priority` |
| Video | `video_path`, `video_x`, `video_y`, `video_w`, `video_h`, `video_stretch`, `video_lock_aspect`, `video_tint`, `video_alpha`, `video_priority`, `video_play_mode`, `video_volume`, `video_muted` |
| Audio | `audio_path`, `audio_play`, `audio_volume`, `audio_pitch`, `audio_loop`, `audio_class`, `audio_priority`, `audio_load_mode`, `audio_spatial`, `audio_min_dist`, `audio_max_dist`, `audio_doppler` |

Changing `phys_*` or `trig_*` updates the stored value but **not** an existing
collider — the body is created when the scene loads. Rebuild by destroying and
re-spawning the object instead.

---

## 7. Tags

Short labels for finding objects by role instead of by name. Set them in the
Inspector (comma-separated, under the object's name) or from script. Matching is
exact and case-sensitive.

| Call | Result |
|---|---|
| `handle:tags()` → `table` | array of the object's tags |
| `handle:has_tag(t)` → `boolean` | |
| `handle:add_tag(t)` → `boolean` | `false` if it already had it |
| `handle:remove_tag(t)` → `boolean` | `false` if it didn't have it |
| `find_by_tag(t)` → `table` | every live object with that tag |

```lua
for _, pickup in ipairs(find_by_tag("pickup")) do
    pickup:set_visible(true)
end
```

Script-side tag changes are live only; they never rewrite the scene file.

---

## 8. Time & timers

| Call | Result |
|---|---|
| `time.delta` | seconds since the last frame (same value `on_update` receives) |
| `time.total` | seconds since the session started |
| `time.after(seconds, fn)` → `id` | run `fn` once, later |
| `time.every(seconds, fn)` → `id` | run `fn` repeatedly |
| `time.cancel(id)` → `boolean` | stop a timer; `false` if it was already done |

Timers fire at the start of a frame, before any `on_update`, so what they change
is visible that same frame. A repeating timer re-arms from its deadline rather
than from when it fired, so it doesn't drift — but a long frame never queues up a
burst of catch-up calls.

```lua
function on_start()
    time.after(3, function() find("Intro"):hide() end)

    local id
    id = time.every(0.5, function()
        blink = not blink
        find("Warning"):set_visible(blink)
        if time.total > 10 then time.cancel(id) end
    end)
end
```

---

## 9. Events

A named message bus, so scripts can talk without holding handles to each other.

| Call | Result |
|---|---|
| `event.on(name, fn)` | subscribe; `fn` receives the payload |
| `event.emit(name [, payload])` | call every subscriber, in subscription order |

Delivery is synchronous. A handler that errors is reported and skipped — the rest
still run. Subscribing during an `emit` takes effect from the *next* emit.

```lua
-- pickup.lua
function on_trigger(entrant)
    event.emit("score", 10)
    self:destroy()
end

-- hud.lua
function on_start()
    score = 0
    event.on("score", function(points)
        score = score + points
        text.set("ScoreLabel", "Score: " .. score)
    end)
end
```

---

## 10. Spawning

### `spawn(source, name [, x, y, z])` → handle | `nil`
Clones a **template object** already in the scene. The clone inherits its
attributes, rotation, scale and tags, and starts visible. `source` is a handle or
a name; the position defaults to the template's own.

Cloning — rather than naming a model file — is what makes this work on the
console: the template is in the scene, so its mesh, textures and scripts are
already cooked into `game.spak` and stream in normally. Author templates hidden
(uncheck **Visible**) and spawn from them.

### `handle:destroy()` → `boolean`
Removes the object: it stops drawing, leaves the simulation, and disappears from
`find_all` / `find_by_tag`. Its `on_destroy()` runs if it defines one.

### `handle:alive()` → `boolean`
Whether the handle still refers to the object it was made for.

```lua
-- Fire a bullet, and clean it up after two seconds.
function on_update(dt)
    if input.button("A") and not firing then
        firing = true
        local x, y, z = self:position()
        local b = spawn("BulletTemplate", "Bullet", x, y + 1, z)
        b:set_velocity(0, 0, 20)
        time.after(2, function() if b:alive() then b:destroy() end end)
    end
end
```

**Handles and destroyed objects.** A destroyed object's slot is reused by a later
spawn, so a handle you kept could otherwise end up pointing at a *different*
object. It cannot: every handle remembers which occupant of the slot it was made
for, and once that object is gone the handle simply stops working — `:alive()`
returns `false`, `:name()` returns `""`, and every setter does nothing. Always
guard a stored handle with `:alive()` rather than assuming it is still valid.

Destruction is applied at the end of the frame (the object is already "dead" to
scripts the moment `destroy()` returns), so removing an object mid-update is safe.

---

## 11. Scenes & camera

### `scene.load(name [, minSeconds])` → `boolean`
Switches scenes. `name` is a scene name (`"Level2"`) or a scene-relative path.
Returns `false` if the scene can't be found, or if a load is already running.

**The swap is not immediate — and that's what makes a loading screen possible.**
The current scene keeps running and rendering while the new one's meshes stream
in. So whatever you put on screen stays up, *and the script that put it there
stays alive to update it*. The hand-over happens as soon as the new scene's
meshes are in and their textures are drawable — no padding.

A loading screen you build is **guaranteed at least one drawn frame**, however
fast the load turns out to be. The swap is deliberately held off until the frame
that started it has been rendered; otherwise the hand-over — which clears the GUI
along with the old scene — could tear your screen down before it ever reached the
display.

`minSeconds` (default `0`) is opt-in: it holds the screen for at least that long
even when the load finishes sooner. Leave it alone unless you specifically want a
screen to linger. A scene whose meshes are already cached (a return trip —
the stream cache survives a swap) has little to read and finishes almost at once,
and a small scene loads in a frame or two. In both cases a fast bar means a fast
load. Use `minSeconds` if you want the screen held regardless.

### `scene.is_loading()` → `boolean` · `scene.progress()` → `0..1`
Whether a load is running, and **the fraction of the bytes it has to read that
have arrived** — weighted by each mesh's size on disc, taken from the pak. It is
real read progress: not a count of assets, and not a timer. Time passing does not
move it. `progress()` is `1` when nothing is loading.

**What the load waits for:** every mesh resident, and every texture those meshes
use *drawable* — small mips registered, which is what the progressive cook exists
for. Full-resolution texture data is **not** waited on; it keeps streaming into
the new scene and sharpens there, exactly as it does during normal play.

In the editor `progress()` is always `1`. There is no streaming system there —
meshes load synchronously the first time they are drawn — so there is no partial
state to report.

### `scene.name()` → `string`
The current scene's name — still the *old* scene until the hand-over completes.

### Worked example: a loading screen

This is `assets/scripts/sceneswap.lua` from the test project, verbatim. It is
attached to an object in **both** scenes, so the one script drives either
direction — it works out where to go from `scene.name()`.

```lua
-- Scene-switch demo: holds a GUI loading screen up while the next scene streams
-- in, driving the bar from scene.progress(). Attached to an object in BOTH
-- scenes (Main -> Scene -> Main), so the same script drives either direction.
--
-- Set AUTO_AFTER to a number of seconds to make the switch happen on its own
-- (handy for capturing it, or on a build with no controller); leave it nil and
-- the switch only happens when A is pressed.
local AUTO_AFTER = nil

local FONT = "assets/Typo_Round_Bold_Demo.otf"

local target      -- the scene we switch TO, decided from the one we're in
local screen, bar -- loading-screen widgets
local switching = false

local function begin_switch()
    if switching or scene.is_loading() then return end
    switching = true

    -- Opaque, so it hides the CURRENT scene — which keeps rendering behind it
    -- for the whole load. That is also what keeps this script alive to run the
    -- progress bar below.
    screen = gui.panel{ x = 0, y = 0, w = 1280, h = 720, color = { 0, 0, 0, 1 } }
    -- No punctuation: this font's cooked atlas has no '.' glyph, so an ellipsis
    -- comes out as three missing-glyph boxes on the console.
    gui.label{ parent = screen, x = 0, y = 300, w = 1280, h = 50, align = "center",
               text = "Loading " .. target, font = FONT, size = 40,
               color = { 1, 1, 1 } }
    -- Track behind the bar, so an empty bar still reads as a bar.
    gui.panel{ parent = screen, x = 440, y = 380, w = 400, h = 18,
               color = { 0.14, 0.14, 0.16, 1 } }
    bar = gui.panel{ parent = screen, x = 440, y = 380, w = 0, h = 18,
                     color = { 1, 0.55, 0.1, 1 } }

    gui.set_paused(true)   -- freeze gameplay while the screen is up
    scene.load(target)
    log("sceneswap: loading " .. target)
end

function on_start()
    target = (scene.name() == "Main") and "Scene" or "Main"
    log("sceneswap: in '" .. scene.name() .. "', A switches to '" .. target .. "'")
    if AUTO_AFTER then time.after(AUTO_AFTER, begin_switch) end
end

function on_update(dt)
    if not switching and input.pressed("A") then
        begin_switch()
    end

    if switching and scene.is_loading() and bar then
        -- 0..1 of the incoming scene's meshes that have streamed in.
        bar:set_size(400 * scene.progress(), 18)
    end
end
```

### `camera.set_active(handle)` → `boolean`
Makes that object's **Camera** attribute the one driving the view, deactivating
any other. Returns `false` if the object has no Camera attribute.

### `camera.active()` → handle | `nil`
The object whose camera is currently driving the view.

```lua
function on_trigger(entrant)
    if entrant:name() == "player" then
        camera.set_active(find("CutsceneCam"))
        time.after(4, function() camera.set_active(find("PlayerCam")) end)
    end
end
```

---

## 12. Input

Scripts always poll **Xbox controls** (`"A"`, `"LX"`, …). On the console these read
a real gamepad. In the editor's Play preview they read a gamepad too, **plus** any
PC keyboard/mouse you've bound to a control in the **Mapping** panel
(View → Mapping) — so you can test without a controller. Binding PC inputs is an
editor testing convenience only; it changes nothing about the script or the
console build.

### `input.button(name)` → `boolean`
True **while** the named button is held — every frame of the press. Use it for
things that should keep happening for as long as you hold the button: moving,
sprinting, crouching, holding a trigger down.

```lua
-- Walk right for as long as DPadRight is held.
function on_update(dt)
    if input.button("DPadRight") then
        local x, y, z = self:position()
        self:set_position(x + 4 * dt, y, z)   -- 4 units/second
    end
end
```

### `input.pressed(name)` → `boolean` · `input.released(name)` → `boolean`
True only on the **one frame** the button goes down, or comes up. A press lasts
several frames, so anything that should happen once per press — opening a menu,
firing a shot, changing scene — belongs here rather than in `input.button`.

```lua
-- Jump once per press. With input.button this would push every frame A is
-- held, which is a rocket rather than a jump.
function on_update(dt)
    if input.pressed("A") then self:apply_impulse(0, 7, 0) end
end
```

The engine stores last frame's button state and compares it with this frame's, so
two cases come out right that a script cannot easily get right on its own:

- If a button is **already down when the game starts** — the title boots, or you
  click Play while holding the key — that is not counted as a press.
- If a button is **still down while [`scene.load`](#11-scenes--camera) runs**, it
  is not counted as a press in the new scene. Loading a scene deletes the running
  script and starts a new copy, so a script cannot remember anything from before
  the change; the engine can.

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

## 13. Video

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

## 14. Audio

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
| `audio.set_volume(name, v)` | linear gain, clamped to `0`–`20` |
| `audio.set_pitch(name, v)` | playback rate, clamped to `0.1`–`4` (also speeds/slows the clip) |
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

## 15. Animator

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

## 16. Utility

### `log(msg)`
Print to the editor **Log** panel as a `[LOG]` entry (or the console's debug
output). Any value is coerced to a string; build messages with `..`. Script
*errors* show separately as `[ERROR]`.

```lua
log("health = " .. 100)
```

---

## 17. Text

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

## 18. GUI

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
| `gui.set_play_mode(widget, mode)` | animated `.gif` texture: `"loop"`, `"once"`, `"off"` |
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
| `texture` | project-relative `.png` / `.jpg` / `.gif` (a `.gif` animates) |
| `slice` | 9-slice border in pixels — the border keeps its size, the middle stretches |
| `play_mode` | animated `.gif` only: `"loop"` (default), `"once"`, `"off"` |
| `text` · `font` · `size` | caption, project-relative `.ttf` / `.otf`, pixel size |
| `align` · `valign` | `left` `center` `right` · `top` `middle` `bottom` |
| `wrap` | wrap text at the widget's width |
| `visible` · `enabled` · `focusable` | booleans; buttons are focusable unless you say otherwise |
| `on_confirm` · `on_cancel` · `on_focus` | callbacks, called with the widget |

A `.gif` `texture` animates on its own embedded frame delays, per widget — two
widgets on the same file keep separate clocks. The same limits apply as to an
animated Image attribute (see **Animated GIFs** below), and `slice` is ignored:
a GIF always draws as a plain stretched quad.

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
[Input](#12-input).

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

To open it from a button instead of at startup, use
[`input.pressed`](#12-input) — `input.button` reports a button as **held**, so it
would toggle the menu every frame the button is down:

```lua
function on_update(dt)
    if input.pressed("Start") then
        if menu then close_menu() else open_menu() end
    end
end
```
---

## 19. Animated GIFs

Point an **Image** attribute at a `.gif` and it animates. A `gui.image` widget
does the same with a `.gif` `texture`. Timing always comes from the delays
stored in the GIF itself — there is no speed control.

The only control is the play mode, `image_play_mode` on the attribute (or
`play_mode` / `gui.set_play_mode` on a widget):

| Value | Name | Behaviour |
|---|---|---|
| `2` | Loop *(default)* | runs forever |
| `1` | Play Once | plays through, then holds the last frame |
| `0` | Off | holds frame 1 |

```lua
find("Logo"):set("image_play_mode", 1)   -- play it once, then stop on the end
```

Switching play mode restarts from frame 1, which is also how you replay a
finished **Play Once**.

Playback belongs to the *attribute*, not the file: two objects showing the same
GIF animate independently. In the engine viewport a GIF sits frozen on frame 1
while you are editing and animates once you press **Play**, matching the Video
attribute; **Stop** rewinds it.

### Limits

- **64 frames.** The console stores the frames as one Xbox 360 array texture,
  whose array size is a 6-bit hardware field. A longer GIF fails the cook with a
  message naming the limit, rather than shipping something wrong. Frame
  *dimensions* are not the constraint (8192 px), frame *count* is.
- **Cooked, never decoded on console.** The frames are compressed to DXT offline
  by the deploy step. A path a script builds at run time
  (`"anim/" .. n .. ".gif"`) is invisible to that scan and will not be in the
  pak — the same rule as every other asset.
- **Loop counts are ignored.** A GIF's own "play N times" extension is not read;
  `Loop` loops forever.
- Frame delays under one display frame are limited by the frame rate.
- On a GUI widget, `slice` (9-slice) is ignored for a GIF.

Rough resident cost on console, DXT1 with mips — there is a per-frame floor of
about 24 KB from texture tiling, so small frames cost more than raw pixel maths
suggests:

| frames × size | total |
|---|---|
| 64 × 128² | 2.0 MB |
| 64 × 256² | 4.0 MB |
| 64 × 512² | 12.0 MB |
