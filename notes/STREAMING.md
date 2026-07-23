# Xbox 360 asset compression + streaming — design

A purpose-built, 360-optimized asset pipeline. **Not** a port of the PC engine (that
one has no compression and no per-asset streaming — it parses FBX/PNG at runtime and
keeps whole scenes resident). This is built around what the 360 hardware and XDK are
actually good at.

## Platform realities that drive every decision here

- **The DVD is CAV, ~16.5 MB/s at the *outer edge* only.** Inner tracks run roughly
  half that. Individual seeks cost ~100 ms+; a **layer change** is worse. Raw throughput
  is not the enemy — seeks and layer changes are. So the win from compression on this
  platform is **effective bandwidth** (fewer/shorter/more-sequential reads), *not* disk
  space. This biases us toward a good-ratio codec, subject to the CPU limit below.
- **The 360 has no hardware decompression.** Unlike the PS3 (SPU-offloaded zlib) or
  Xbox One (LZ move engines), *all* decompress runs on the PowerPC cores. Pipeline
  throughput is therefore `min(disc delivery of compressed bytes, CPU decompress rate)`.
  LZX gives the best ratio but is the slowest to decompress of the practical options;
  measure it on hardware before assuming one pinned worker keeps up. The console has
  **6 HW threads**, mostly idle while streaming — a burst can fan decompression across
  a few of them (chunk the entry) if the single worker becomes the bottleneck.
- **Bake to final GPU layout offline — never transcode at runtime.** id Tech 5 / RAGE
  compressed megatextures with JPEG-XR and transcoded to GPU format on the fly; that
  CPU cost is exactly what caused its notorious texture pop-in. We do the opposite:
  runtime "loading" is *only* decompress + pointer fixup, zero format conversion.
- **HDD install is a second delivery path.** Installed, there are no seeks and throughput
  is steadier — the same `game.spak` serves both, but from HDD we can afford a smaller
  read-ahead window and more random access. Track which path we opened (`game:\` vs the
  installed location) even if phase 1 only logs it.

## The core 360 trick: bake to final GPU layout, stream compressed, register by pointer fixup

Real 360 titles used **XPR2 (Xbox Packed Resource)** — see the XDK's
`Source/Samples/Common/AtgResource.cpp`. The insight: do *all* the expensive work
offline. Each texture is baked into its **final tiled DXT layout with a prebuilt
`D3DTexture` header**; each mesh into a native big-endian vertex/index buffer with its
header. At runtime, "loading" a resource is:

1. read its bytes into physically-contiguous memory, and
2. fix up the resource's base address — `XGOffsetBaseTextureAddress(tex, vidmem, vidmem)`.

**No `CreateTexture`, no pixel copy, no DXT/tiling/endian work on the console.** The
decompressed bytes *are* the GPU resource. We wrap that in LZX compression, a streaming
container, a worker thread, and a residency manager.

### Verified on Xenia (boot-time probe, StreamProbe.cpp — since removed)
- `XMemCompress`/`XMemDecompress` (LZX): round-trips byte-exact (256 KB → 1 KB → 256 KB, hr=0).
- `XPhysicalAlloc(..., PAGE_WRITECOMBINE)`: returns usable GPU-visible memory.
- `CreateThread` + `XSetThreadProcessor(t, 4)`: worker runs pinned to HW thread 4.

## On-disk format: `game.spak`

All multi-byte fields **big-endian** (baked on the PC cooker; the console reads native).

```
Header:  magic 'SPAK' | version | entryCount | sectorSize(2048) | tocOffset
TOC[entryCount], sorted by nameHash (binary search), fixed-size entries:
    nameHash (FNV-1a of the asset's relative path)
    type     (TEX2D | VBUF | IBUF | BUNDLE)
    flags    (bit0 = compressed; bits1-3 = codec id: 0 NONE / 1 LZX / 2 fast-reserved)
    diskOffset (sector-aligned) | compressedSize
    sysMemSize (resource headers) | vidMemSize (GPU data)
Data: per entry, an XPR2-style chunk {sysmem header block + vidmem data block},
      compressed as a unit, sector-aligned. Entries are emitted in **load-sequence
      order** (not name order) so streaming reads march forward across the disc.
```

**Codec is pluggable, not hardcoded.** The codec lives in `flags` so the reader
dispatches per entry and we can swap without a re-cook. Phase 1 ships **LZX only**
(best ratio, SDK-native via `XMemCompress`) — this is the default, not a commitment.
If on-hardware measurement shows the worker can't sustain
`disc_rate / ratio` MB/s of decompress, we either fan LZX decompress across HW threads
or mark hot assets with a faster codec id while leaving cold/bulk data on LZX. The
format doesn't change either way.

**Disc layout is part of the cooker's contract.** `deploy.ps1` must place `game.spak`
on a **single layer, toward the outer edge** of the ISO, and the cooker must emit
entries in the order the runtime will request them, so seeks are forward and short and
no layer change ever falls mid-stream.

> **XMemDecompress gotcha:** the API reads past the end of the input and needs **16
> trailing zero bytes** on the compressed buffer or it fails intermittently. The cooker
> pads every compressed chunk by 16 (still sector-aligned); the reader must not trip on it.

## Runtime components

- **StreamPak** — opens `game:\game.spak` with ONE persistent overlapped handle; loads
  the TOC; lookup by `nameHash` (binary search). No per-asset re-open (a DVD killer).
- **StreamThread** — one worker pinned to HW thread 4. Pulls load requests from a
  priority queue; per request: overlapped `ReadFile` of `[diskOffset, compressedSize]`
  → decode per the entry's codec id (`XMemDecompress` for LZX; passthrough for NONE)
  into sysmem (`new BYTE[]`) + vidmem (`XPhysicalAlloc` WC 4K) → push a "ready to
  register" record. Requests sorted by `diskOffset` to batch seeks. If profiling shows
  decompress is the bottleneck, a large entry can be split and decoded across additional
  idle HW threads before registration (out of scope for phase 3, but the codec dispatch
  is structured to allow it).
- **Registration (render thread)** — drains completions each frame, budgeted N/frame:
  fix up the XPR2 headers (`XGOffsetBaseTextureAddress`, VB/IB base pointers) into live
  `D3DTexture*` / `D3DVertexBuffer*`. D3D header touches stay on the render thread.
- **StreamCache (residency)** — ref-counted handles with a memory budget (e.g. 128 MB
  of 512). `Request(handle, priority)`: resident → bump refcount + lastUseFrame; else
  enqueue + return a placeholder (2×2 magenta / empty mesh) until resident. Over budget
  → LRU-evict unreferenced resident handles (`XPhysicalFree` + free sysmem).
- **Triggers** — lazy + frustum-culled: each frame `SceneRuntime` builds the world-space
  view frustum (Gribb-Hartmann from `view*proj`) and only requests objects whose bounding
  sphere is visible; first touch enqueues an async load. Culled objects stop being touched,
  go cold, and get LRU-evicted. An **auto cinematic camera** (slow orbit + periodic gaze-pan
  away from the cluster) drives visibility so eviction/re-stream actually cycle. Demo budget
  is 2 MB (`SceneCache::Init` arg in `SceneRuntime`); bump to 128 for ship behaviour.

## Offline cooker: `spakc` (PC Win32 tool — `runtime/tools/spakc/`)

**Phase 1 (built): Bundler-based textures.** `spakc` writes a one-line `.rdf` and
runs the XDK's `Bundler.exe` (found via `XEDK`) to bake the source image into a
console-native XPR2 (final tiled DXT5 layout + full mip chain + `D3DTexture`
header — Microsoft's own cooker, so the tiling is correct by construction). It
then LZX-compresses the whole XPR2 (`XMemCompress`, win32 `xcompress.lib`) and
writes the `.spak`. It **self-verifies** by decompressing its own payload and
re-checking the XPR2 magic. Links only `xcompress.lib`; no tiling code of its own.
`cook.ps1` builds it and cooks one texture into a deploy folder.

**Later: custom tiling (deferred swap behind the same format).** Replace the
Bundler call with `d3dx9` + `xgraphics` directly, so there's no external tool and
we can cook meshes the same way:
- **textures**: `D3DXCreateTextureFromFile` → DXT1/5 + full mip chain →
  `XGTileTextureLevel` into Xenos tiled layout → build `D3DTexture` header → XPR2 chunk.
- **meshes**: M360 blob → native big-endian VB/IB + declaration → headers → XPR2 chunk.
- compress each chunk with the entry's codec (LZX via `XMemCompress` in phase 1),
  **pad the compressed buffer with 16 trailing zero bytes** (XMemDecompress overrun),
  keep sector alignment; write big-endian TOC + data in **load-sequence order**.
Wired into `deploy.ps1`, which places `game.spak` on a single outer-edge layer of the
ISO; the game then ships **one `game.spak`** instead of loose assets.

## Constraints
- C++03, big-endian, no STL threads → Win32 `CreateThread` + `XSetThreadProcessor`,
  `Interlocked*`, events. 32-bit address space (512 MB unified) → 32-bit offsets.

## Build phases
1. **Cooker + format + reader + synchronous** load→decompress→register of one texture
   (prove the XPR2 register path actually renders in Xenia). — **BUILT.**
   `SpakFormat.h` (shared format), `spakc` cooker (Bundler + LZX, host round-trip
   self-verified), `StreamPak` (`Open`/`Find`/`LoadTextureSync`), and a
   `SceneRuntime` probe that draws the streamed texture on a floating quad. Cooker
   round-trip and the `.xex` link are green on the PC; final visual confirmation is
   launching the deploy in Xenia (checker card floating above the scene).
2. Meshes through the same path; `SceneRuntime` pulls assets from `StreamCache`.
   — **BUILT (Bundler + option B).** Meshes stream as a `MESH` payload (native-BE
   VB/IB + baked alpha kind + texture nameHashes); the runtime `CreateBuffer`s +
   copies them (zero-copy buffer registration deferred). `StreamCache` resolves
   meshes/textures by hash, load-on-touch, cache-and-hold (no eviction yet);
   `SceneRuntime.ResolveMesh` prefers the cache and falls back to the raw D3DX
   loader for anything uncooked. `spakc build` cooks a whole scene (deduped
   textures, alpha classified at cook time); `deploy.ps1 -Spak` wires it in.
   PC-verified (15-entry scene pak, LZX round-trip OK); Xenia visual pending.
3. **Worker thread**: async load off the render thread; completion queue; placeholders.
   Measure sustained decompress MB/s here — if the worker can't hold `disc_rate / ratio`,
   revisit codec / thread fan-out before building residency on top of it.
   — **BUILT.** `StreamCache` runs one `CreateThread` worker pinned via
   `XSetThreadProcessor(t, 4)`: it pops the lowest-`diskOffset` request (locked
   queue), does `ReadBlob` (read + `XMemDecompress`), and pushes a completion.
   The render thread drains ≤8/frame (`Update`) and does the D3D creation
   (`CreateVertexBuffer`/`RegisterTextureFromBlob`) — cache maps stay render-thread
   only, so no lock on them. `GetMesh` returns NULL+`inPak` while loading (skip the
   draw, no stall); textures show a 2×2 magenta placeholder until resident. The
   worker logs decoded MB/s per load (the measurement gate). PC-linked; Xenia visual
   pending. Zero-copy buffer registration + multi-thread decode fan-out still open.
4. **Residency**: budget + LRU eviction + refcounting.
   — **BUILT.** `StreamCache` tracks resident bytes (entry sysmem+vidmem) against a
   budget (default 128 MB via `Init`'s `budgetMB`); each `GetMesh`/texture request
   stamps `lastUse = frame`. `EvictIfOverBudget` (end of `Update`) LRU-evicts the
   coldest resident assets — but only those not drawn in the last `kEvictGraceFrames`
   (2) frames, so in-use assets are never pulled and a working set over budget
   exceeds it rather than thrashing. Eviction frees the resource + erases the record
   (next request re-streams it); borrowed mesh→texture pointers stay safe because
   they re-resolve every frame. **Refcount note:** recency (grace-window last-use)
   is the reference signal — the request-per-frame draw pattern holds no cross-frame
   handles, so explicit acquire/release refcounts are unnecessary. Consequence: on a
   fixed-camera, all-visible scene nothing goes cold, so eviction stays dormant
   (correct) — it only fires once culling / camera movement makes assets cold.
5. **Cutover**: deploy ships `game.spak` only; stop shipping raw assets.
   — **BUILT.** `deploy.ps1` no longer copies the raw `assets/` folder and always
   cooks `game.spak` (spakc build + scene enumeration, previously the opt-in
   `-Spak`). The image is `default.xex` + `game.proj` + `scenes/` + `shaders/*.cso`
   + **`game.spak`** (all meshes + textures) — no raw `.mesh`/`.png`. `build_iso.ps1`
   inherits it. The runtime's raw-loader fallback (`Content.GetMesh`) is now
   vestigial in the shipped image (kept only for running against a source tree in
   dev); everything resolves from the stream. Demo scaffolding (cinematic camera,
   frustum-cull driver, sim latency, tiny budget) was removed — fixed camera + 128 MB
   budget restored.
