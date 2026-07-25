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

    ffmpeg -i <src> -vn -f mp2 -c:a mp2 -b:a 192k -ar 48000 -ac 2 <name>.mp2

192 kbps stereo ≈ 1.4 MB/min (a 3-minute track ≈ 4 MB encoded).

## Shipping + memory model

`spakc` treats `.mp2` args as **`'AUDI'` entries, always stored raw** (MP2 is
already compressed); `deploy.ps1` enumerates every `audioPath` in the startup
scene. At voice start the player reads the clip's encoded bytes from the
entry's byte window in game.spak (loose-file dev fallback) and decodes PCM
progressively from RAM.

Short clips (up to 256 KB) enter a shared 16 MB LRU encoded-byte cache. Multiple
voices playing the same effect share those immutable MP2 bytes while retaining
independent decoder cursors and XAudio2 queues. Active entries are pinned;
only inactive entries can be evicted. Larger clips still retain per-stream
encoded bytes in Auto mode. Stream mode instead feeds `pl_mpeg` through
callback-backed 256 KB range reads. Its encoded working set is bounded to a
conservative 512 KB per voice (one read chunk plus the decoder ring), and loop
rewind restarts range reads at the beginning without making the clip resident.
On Xbox, these reads use `StreamPak`'s one persistent `game.spak` handle; its
file lock serializes each `SetFilePointer + ReadFile` operation with texture and
animation reads.

## The player

`src/audio/AudioPlayer.{h,cpp}` — shared C++03. **One** decode worker services
every stream (cap **24**; HW thread 3 on the console; MP2 decode is ~0.3 ms per
26 ms frame). Voices are XAudio2 via the shared `AudioOut`: **stereo** for 2D
sources, **mono** (worker downmix) for spatial ones. Each voice can queue up to
24 MP2 frames, roughly 576 ms at 48 kHz before pitch adjustment. All voice
parameters — volume, frequency ratio (pitch × doppler), the 3D matrix — are
applied from the host thread each frame; the worker only decodes and submits.
A non-looping clip reports not-playing once its voice drains; loops rewind
seamlessly.

`AudioClipReader` isolates whole-file/range I/O from playback. Reads, MP2 decode,
PCM conversion, and XAudio submission run outside the reconciliation lock while
the stream is pinned against deletion, so audio queue filling cannot stall the
render thread.

## Voice admission

`AudioVoicePolicy` ranks the complete wanted set before reconciliation and
admits at most 24 voices. Spatial sources beyond `maxDist` are culled before a
decoder or XAudio2 voice is allocated. Ranking is deterministic: authored
priority, then class protection (Dialogue, Music, Ambience, Effect), proximity,
an Effect-loop penalty, and finally stable stream key. Explicit extreme
priority can override class protection. A newly admitted higher-ranked voice
displaces the lowest-ranked active voice through the existing short fade, not
an abrupt cut.

The Inspector and scene JSON expose class, priority, and load mode; old scenes
default to Effect, priority 0, and Auto in both parsers. Auto caches clips up to
256 KB, while Resident caches any clip that fits the 16 MB budget. Stream
bypasses the resident cache and incrementally refills the decoder in 256 KB
chunks.

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

`{audioPath, playMode Off/On, volume 0–20, pitch 0.1–4, loop, class, priority,
loadMode, spatial (default on), minDist=1, maxDist=50, doppler=1}` — playMode
**On** at scene start is autoplay. Plays in the editor's **Play preview only**
(edit mode silences everything) and always on the console. No buses, no master
volume; every source is independent. Streams key on object name + attribute
index.

## Lua

    audio.play("Name")           -- from the top (restarts a stopped/finished clip)
    audio.stop("Name")
    audio.is_playing("Name")     -- false after a non-looping clip ends
    audio.set_volume("Name", v)  -- 0..20, live
    audio.set_pitch("Name", v)   -- 0.1..4, live
    audio.set_loop("Name", b)    -- live

Overrides are transient (authored values untouched); the editor clears them
when the Play session ends. See `E:\Projects\360proj\assets\scripts\audiodemo.lua`.
