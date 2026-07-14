# 360engine runtime (Xbox 360 game)

This is the **shipping game runtime** — the code that is built into the Xbox 360
title and runs on the console / Xenia. It is a stripped-down version of the
editor's `SceneRenderer`: it renders the scene the editor authored (meshes +
custom-shader objects) from a **fixed camera**, with **no grid, gizmo, selection
highlight, ImGui, or keyboard/mouse input**.

Unlike the editor (CMake + VS2022, PC Direct3D 9), the runtime is a separate
target built with **VS2010 + the Xbox 360 XDK** (there is no CMake path for the
console). It reuses the editor's on-disk binary formats — `.scene` / `.proj`
JSON and the `M360` `.mesh` blobs — and the built-in material, whose single
canonical source is the editor's `src/shaders/standard.hlsl` (`deploy.ps1`
compiles that same file to Xenos microcode), so a scene made in the editor
renders the same here.

## Layout

```
runtime/
  game360.sln / game360.vcxproj   hand-authored VS2010 "Xbox 360" project (toolset 2010-01)
  deploy.ps1                      assembles a Xenia-loadable folder
  build_iso.ps1                   packs the deploy folder into an XDVDFS .iso
  src/
    main.cpp          entry point + fixed-timestep game loop
    XboxRenderer.*    D3D9x device init (XGetVideoMode -> present params -> CreateDevice) + frame
    SceneData.*       .proj / .scene loader (self-contained tiny JSON parser)
    Content.*         mesh blob + texture (D3DX) + shader (D3DXCompileShaderFromFile) loading, endian-safe
    SceneRuntime.*    the stripped renderer: fixed camera, mesh material pass + custom-shader pass
    RtMath.h          hand-rolled LH row-major matrices (matches the editor)
    Endian.h          little-endian <-> native helpers (the 360 is big-endian)
```

## Build

From any shell (the machine env var `XEDK` points at the XDK, so MSBuild's
Xbox 360 platform resolves the includes/libs):

```
"C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe" ^
    runtime\game360.vcxproj /p:Configuration=Release /p:Platform="Xbox 360"
```

Output: `runtime\Release\game360.xex`. (Debug config also works.)

**Build output location.** By default artifacts (`<Config>\`, `deploy\`, the
`.iso`, `game.xgd`, `buildrun.log`) land in `runtime\`, but every script takes
`-OutDir <dir>` to redirect all of them out of the source tree (MSBuild's
`OutDir`/`IntDir` are pointed there too, so `obj\` follows). In the editor this is
**Settings > Build Configuration > Build output folder**, alongside the
Release/Debug toggle and the ISO name.

The project can also be opened in Visual Studio 2010 (with the XDK installed) and
built from the IDE — same result.

## Deploy + run in Xenia

```
powershell -ExecutionPolicy Bypass -File runtime\deploy.ps1 -Project E:\Projects\360proj -Config Release
```

This assembles `runtime\deploy\` with `default.xex` plus the project content laid
out as the runtime expects under `game:\`:

```
game:\default.xex
game:\game.proj            (optional; startupScene, else scenes\Main.scene)
game:\scenes\*.scene
game:\game.spak            (ALL meshes + textures, cooked + LZX-compressed - see STREAMING.md)
game:\shaders\*.cso        (precompiled Xenos shaders) + *.dir (custom //@ state)
```

Meshes and textures ship **only** inside `game.spak` (no raw `.mesh`/`.png` in the
image); the runtime streams them from it. `deploy.ps1` cooks the pak automatically
(building `tools\spakc` and enumerating the startup scene's meshes), so the deploy
requires the XDK. See [STREAMING.md](STREAMING.md) for the streaming subsystem.

Then launch it (Xenia mounts the folder holding the `.xex` as `game:\`):

```
xenia_canary.exe runtime\deploy\default.xex
```

## Build a disc image (.iso)

```
powershell -ExecutionPolicy Bypass -File runtime\build_iso.ps1 -Project E:\Projects\360proj -Xedk "<xdk-root>"
```

Xbox 360 discs are **not** ISO-9660/UDF — they use Microsoft's XDVDFS / XGD
layout, so the image is built by the XDK's `XDiscBld.exe` (not `mkisofs`) from a
small `.xgd` layout file. `build_iso.ps1` builds the `.xex`, runs `deploy.ps1` to
assemble the `game:\` tree, writes a `game.xgd` that adds `deploy\` to the disc
root, and packs `runtime\<project>.iso` (XGD2). The result mounts directly in
Xenia (`xenia_canary.exe runtime\<project>.iso`) or can be burned for a console.

`XDiscBld` prints `NxeArt`/`Xdb` *warnings* (dashboard tile art and a debug
database, required only to **submit** a title to Microsoft) and exits non-zero
because of them — the script ignores the exit code and checks that the `.iso` was
actually produced, since the image itself is valid without those.

In the editor this is the **Build > Build .iso Image** menu item (needs the XDK
path set in Settings > Toolchain, since `XDiscBld` lives under it).

The first launch shows "Skipping draw - pipeline not ready" for a few frames
while Xenia translates the shaders to its backend; it caches them, so later
launches draw immediately.

## Notes / known limitations (first cut)

- **Fixed camera** — the editor's default orbit framing (yaw 0.6, pitch 0.5,
  distance 20, target origin). No input; a scene-defined or animated camera is a
  follow-up.
- **Shaders ship as precompiled Xenos `.cso` — no HLSL in the game.** `deploy.ps1`
  compiles every shader (standard + custom) offline with the XDK's PC-side
  `fxc.exe` (found via the machine `XEDK`) into `game:\shaders\<stem>_{vs,ps}.cso`,
  and bakes each custom shader's `//@` render directives into a tiny
  `<stem>.dir` sidecar. The runtime loads the `.cso` with
  `CreateVertexShader`/`CreatePixelShader` + `D3DXGetShaderConstantTable` (matrix
  constants) and reads render state from the `.dir` — no HLSL compilation, no
  `.hlsl` shipped. (The editor's own `.cso` are SM3.0 *PC* bytecode for its
  viewport and won't run on Xenos, so they're unused here.) The built-in material
  has a single source — the editor's `src/shaders/standard.hlsl`, which
  `deploy.ps1` compiles (no separate runtime copy); a compile-HLSL-at-load
  fallback remains only for running against a source tree in dev.
- **Alpha: cutout + blend passes are implemented.** Each mesh's diffuse alpha is
  classified with stb (no alpha → Opaque; see-through holes → Cutout; uniform
  translucency → Blend), matching the editor. Opaque + cutout draw first (cutout =
  alpha test on the diffuse alpha + alpha-to-coverage, depth-correct); blended
  meshes (glass) draw last with **depth test on, depth write off** — so opaque
  geometry in front correctly occludes the glass, while the transparent surface
  doesn't write depth. Still deferred vs. the editor: the cutout alpha-dilation
  that removes hair fringe (D3DX owns the texels here), and back-to-front sorting
  of blended meshes against each other (only matters with multiple overlapping
  transparent objects; a single glass object is correct).
- **MSAA + alpha-to-coverage via predicated tiling** (`XboxRenderer`). A 720p MSAA
  colour+depth surface overflows the 10MB EDRAM, so the scene is rendered once
  into a tile-sized MSAA target and the runtime replays it per screen tile (3
  bands of 1280×256 at 4× MSAA, each filling EDRAM), resolving into a full-screen
  front buffer that `Swap` displays. This gives full 1280×720 with 4× MSAA and
  Xenos-native alpha-to-coverage (`D3DRS_ALPHATOMASKENABLE`) for smooth cutout
  edges. The custom present needs `DisableAutoBackBuffer` **and**
  `DisableAutoFrontBuffer` + `D3DCREATE_BUFFER_2_FRAMES` — without disabling the
  auto front buffer the runtime scans out its own empty buffer (black screen).
- **Endianness.** The 360 is big-endian; the editor's blobs are little-endian.
  Vertex AND index buffers must hold **native (big-endian)** data — Xenia does not
  apply the vertex/index declaration's 8IN32 fetch swap, so `Endian.h`'s
  `StoreNativeFromLE32` converts the LE `.mesh` data on upload and the procedural
  quad/cube are written native. (Wrong endianness = geometry invisible/off-screen.)
- **Xenia frame requirements.** `BeginScene`/`EndScene` must wrap the frame or
  nothing presents; render states and the viewport are set explicitly every frame
  (the 360 doesn't guarantee PC defaults); matrix constants go through
  `ID3DXConstantTable::SetMatrix` guarded on a non-zero handle (a null handle
  faults the guest — a custom shader may omit `gWorld`).
- **C++03 only** — the XDK toolset predates C++11 (no in-class member
  initializers, no braced-temporaries, etc.), so the runtime source stays plain.
```
