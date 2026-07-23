# Audio playback

Sound on both renderers — the PC editor's Play preview and the 360 runtime —
mirroring the reference engine's model: an **"Audio" attribute** on a scene
object is a sound source, reconciled every frame by the host into voices on a
shared player. 2D by default off, **3D-positional by default on**: a spatial
source pans and fades against the active camera automatically.

## Format + cook

One format: a **raw MP2 elementary stream (`.mp2`)**, decoded by the vendored
pl_mpeg's standalone audio decoder (endian-clean, already shipped for video).
Import in the editor: drag audio from the Assets panel onto an Audio
attribute. `.mp2` binds directly; `.wav/.mp3/.ogg/.flac/...` transcode on drop
via `ffmpeg` on PATH:

    ffmpeg -i <src> -vn -f mp2 -c:a mp2 -b:a 192k -ar 44100 -ac 2 <name>.mp2

192 kbps stereo ≈ 1.4 MB/min (a 3-minute track ≈ 4 MB encoded).

## Shipping + memory model

`spakc` treats `.mp2` args as **`'AUDI'` entries, always stored raw** (MP2 is
already compressed); `deploy.ps1` enumerates every `audioPath` in the startup
scene. At voice start the player reads the clip's **encoded bytes once** —
the entry's byte window straight out of game.spak (loose-file dev fallback) —
and decodes PCM **progressively** (~150 ms ahead) from RAM. Rationale: encoded
audio is small, and a single sequential read at start can never contend with
the mesh/texture streaming worker's disk-head schedule mid-playback (an
underrun pops; +4 MB of RAM doesn't).

## The player

`src/audio/AudioPlayer.{h,cpp}` — shared C++03. **One** decode worker services
every stream (cap **8**; HW thread 3 on the console; MP2 decode is ~0.3 ms per
26 ms frame). Voices are XAudio2 via the shared `AudioOut`: **stereo** for 2D
sources, **mono** (worker downmix) for spatial ones. All voice parameters —
volume, frequency ratio (pitch × doppler), the 3D matrix — are applied from
the host thread each frame; the worker only decodes and submits. A non-looping
clip reports not-playing once its voice drains; loops rewind seamlessly.

## Spatialization (X3DAudio)

`src/audio/Spatial3D.{h,cpp}` wraps `X3DAudioCalculate` (XDK `x3daudio.lib`;
merged into `xaudio2` on PC): mono emitter → stereo gains + doppler ratio.
The audible model:

- full volume inside **Min Distance**;
- exact **inverse (minDist / distance)** falloff beyond it;
- **silent past Max Distance** (hard cutoff — the default curve never
  reaches zero on its own);
- doppler from real emitter/listener velocities (derived from successive
  frame positions), scaled by **Doppler Factor** (0 disables).

Listener = the active camera (position + basis from the view matrix); a
spatial emitter rides its object's **live physics pose**. Flipping the
attribute's 3D Spatialize recreates the stream (the voice's channel count is
fixed at open).

## The attribute

`{audioPath, playMode Off/On, volume 0–20, pitch 0.1–4, loop, spatial (default
on), minDist=1, maxDist=50, doppler=1}` — playMode **On** at scene start is
autoplay. Plays in the editor's **Play preview only** (edit mode silences
everything) and always on the console. No buses, no master volume; every
source is independent. Streams key on object name + attribute index.

## Lua

    audio.play("Name")           -- from the top (restarts a stopped/finished clip)
    audio.stop("Name")
    audio.is_playing("Name")     -- false after a non-looping clip ends
    audio.set_volume("Name", v)  -- 0..20, live
    audio.set_pitch("Name", v)   -- 0.1..4, live
    audio.set_loop("Name", b)    -- live

Overrides are transient (authored values untouched); the editor clears them
when the Play session ends. See `E:\Projects\360proj\assets\scripts\audiodemo.lua`.

## Debugging

The player logs via OutputDebugString: `audio: stream open <rate>Hz <bytes>b
<key>`, `audio: clip read failed`, `audioout: <XAudio2 call> failed <hr>`.
In Xenia these appear at `debugprint_trap_log = true` + `log_level = 3`.
