# XenFusion

**An open source Xbox 360 engine**

XenFusion is a game engine targeting the Xbox 360 — scene authoring,
rendering, physics, animation and scripting done through DirectX 9. You can build on Windows with the runtime to a `.xex` binary or a `.iso` disc image.

<img width="1424" height="890" alt="demo" src="https://github.com/user-attachments/assets/c1d1e77c-db19-439b-b034-579bc32b0059" />

<img width="800" height="480" alt="demo" src="https://github.com/user-attachments/assets/f0be2d2b-7640-4492-9012-251555bbe72b" />

---

## Features

Everything in a scene is an object, and behaviour comes from **attributes** you
attach to it in the Inspector. These are the current attributes available under
**Add Attribute**:

| Attribute | What it does |
|---|---|
| **3D Model** | Static or skinned mesh with a six-slot material — albedo, normal (height packed in the alpha channel), emission, metallic, roughness and clearcoat — plus cube-map environment reflections. Alpha cutout and blend modes for foliage and hair. |
| **Shader** | A custom HLSL shader as a standalone object, compiled to Xenos microcode when you deploy. |
| **Script** | A Lua 5.4.7 behaviour script. The API covers transforms, physics, input, audio, video, GUI menus and live attribute access. |
| **Animator** | State-machine animation controller — states, transitions, conditions and crossfades over compressed clips, plus bone modifiers for jiggle physics and collision. |
| **Camera** | Fixed, Follow or Track. Track cameras move along a clamped B-spline path with in-engine gizmos. |
| **Directional Light** | Sun-style light with a shadow map, usable as a baked or realtime source. |
| **Point Light** | Omnidirectional light; up to four render simultaneously. |
| **Spot Light** | Cone light with smooth falloff and an optional volumetric beam; up to two render simultaneously. |
| **Environment Light** | Ambient light that lifts the whole scene rather than casting from a direction. |
| **Skybox** | Equirectangular sky, drawn as the background and folded into the environment capture that feeds reflections. |
| **Rigid Body** | Bullet 2.82 rigid body. Box, sphere, capsule, cylinder, convex-hull or exact-mesh collider, static or dynamic. |
| **Trigger Volume** | Non-solid box, sphere, capsule or cylinder that reports overlaps to scripts instead of blocking movement. |
| **Image** | 2D screen-space image, for HUD and overlay art. |
| **Text** | TrueType text rendering. |
| **Video** | MPEG-1 video overlay with MP2 audio, streamed from disc. |
| **Audio** | MP2 playback, either flat 2D or spatialised through X3DAudio with distance falloff. |

- Beyond attributes, the engine also provides Lua-driven `gui.*` retained menus,
lightmap baking, shaders to support material types and shadows, and asset streaming. This isn't a definitive list, it is subject to change.
---

## How to use

> ### Disclaimer
>
> **XenFusion does not provide any proprietary software or tooling.**
>
> Building a game for the Xbox 360 requires Microsoft's Xbox 360 SDK and the
> Visual Studio version it integrates with. None of it is included in this
> repository, in any release, or distributed by this project in any form, and
> none will be provided. You must supply your own legally obtained copy of every
> component listed below. The same applies to Xenia — obtain it from its own
> project.
>
> This repository contains only original engine code and the open source
> libraries listed under `third_party/`.

### What you need to supply

| Component | Why it's needed |
|---|---|
| **Xbox 360 SDK (XDK) 2.0.21256.3** | Supplies `fxc.exe` (the Xenos shader compiler), `XDiscBld.exe` (disc images), `Bundler.exe`, the win32 `xcompress`/`xgraphics` libraries, and the `Xbox 360` MSBuild platform the console project builds against. |
| **Visual Studio 2010** | Provides the `v100` toolset that the XDK integrates with. Required for the console target and for the `spakc` asset cooker. |
| **.NET Framework 4.0 MSBuild** | Drives the console build. Ships with Windows at `C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe` — normally already present. |
| **Xenia Canary** | Optional. Lets you test builds on your PC instead of on hardware. |

### Point the engine at them

Open the **Settings** panel and set the paths under **Toolchain**:

- **Xbox 360 SDK (XDK) 2.0.21256.3** — the root of your XDK install, for example
  `C:\Program Files (x86)\Microsoft Xbox 360 SDK`
- **Xenia emulator** — the folder containing `xenia_canary.exe`

Then, under **Build Configuration**:

- **Build output folder** — where the `.xex`, `deploy\` folder and `.iso` are
  written. Leave it empty to use the engine's own `runtime\` folder.
- **Configuration** — `Release` or `Debug` for the console build.
- **ISO name** — the disc image filename. Leave blank for `<project>.iso`.

Paths are saved as soon as you pick them, so this is a one-time setup.

### Build your game

With the paths set, everything runs from the **Build** menu:

| Menu item | What it does |
|---|---|
| **Rebuild Engine Shaders** | Recompiles the built-in shaders into your project. |
| **Bake Lighting** | Bakes lightmaps and light probes for the selected scene. |
| **Build and Run XEX** | Compiles the runtime, cooks all assets into `game.spak`, and launches the result in Xenia. |
| **Build .iso Image** | Same, then packs the deploy folder into an XDVDFS disc image. |
| **Clean Build** | Clears build artifacts. |

---

## Engine Developers

This section is for building the engine itself. **None of the Xbox 360 toolchain
above is needed to build XenFusion** — you only need it to build a game for the
console.

### Requirements

- **Visual Studio 2022** with the *Desktop development with C++* workload
- **CMake 3.20** or newer
- **Direct3D 9** — `d3d9.lib` and `d3dcompiler.lib` come from the Windows SDK
  that installs with the C++ workload. The legacy DirectX SDK is *not* required.
- **An internet connection for the first configure** — CMake fetches Dear ImGui,
  nlohmann/json, ImGuiColorTextEdit, ImGuizmo and Assimp. Bullet, Lua, xatlas,
  stb and pl_mpeg are vendored in `third_party/`.

### Build

Configure once:

```bash
cmake -S . -B build
```

Then build:

```bash
cmake --build build --config Release
```

The engine lands at `build\Release\XenFusion.exe`, with `fonts\`, `branding\`
and `shaders\` staged alongside it.

For a Debug build:

```bash
cmake --build build --config Debug
```

**Debug builds keep a console window attached**, so engine logs and any script
`print()` output go straight to stdout where you can watch them. Release builds
are windowed with no console.

The first configure compiles Assimp from source and takes a while; subsequent
builds are incremental.

### Layout

| Path | Contents |
|---|---|
| `src/` | The engine — renderer, panels, animation, physics, scripting, audio, video |
| `runtime/` | The Xbox 360 game runtime, plus the deploy and ISO scripts |
| `tools/` | Host-side cookers (`animc`, `lightc`) |
| `third_party/` | Vendored libraries — Bullet, Lua, xatlas, stb, pl_mpeg |
| `notes/` | Subsystem documentation — audio, lighting, streaming, video |
### Contributing

All PRs are welcome

## Further Reading

- [`DeveloperAPI.md`](DeveloperAPI.md) — the full Lua scripting API for making your game
- [`runtime/README.md`](runtime/README.md) — More info on how the runtime is built
- [`notes/`](notes/) — Various subsystem design notes that can help engine contributors

## Releases

Prebuilt engines are released [here](https://github.com/parkerallan/360engine/releases)
