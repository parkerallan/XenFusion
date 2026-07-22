# Video playback

Full-motion video on both renderers — the PC editor viewport and the 360
runtime — mirroring the Vulkan engine's Video2D design: a **"Video" attribute**
drawn as a screen-space overlay quad over the finished 3D frame, decoded by a
shared player, with its MP2 audio track on XAudio2 and the video clock slaved
to the audio cursor.

## Format + cook

The engine plays exactly one format: **MPEG-1 video + MP2 audio in an MPEG-PS
container (`.mpg`)**, decoded by the vendored single-header
`third_party/plmpeg/pl_mpeg.h` (endian-clean integer decoder — byte-identical
output on x64 and the big-endian Xenon, verified by CRC).

Import in the editor: drag a video from the Assets panel onto a Video
attribute. A `.mpg` binds directly; other containers transcode on drop via
`ffmpeg` on PATH (blocking; the exact cook):

    ffmpeg -i <src> -f mpeg -c:v mpeg1video -q:v 6 -vf "scale=640:-2" -r 30
           -c:a mp2 -b:a 224k -ar 44100 -ac 2 <name>.mpg

640x360 @ 30fps, ~q6 is the reference quality (a 2-minute clip lands around
30 MB). Decode cost in Xenia is ~2 ms/frame video + 0.3 ms/frame audio on the
dedicated worker.

## Shipping: raw VIDE entries, ranged reads

`spakc build` treats `.mpg` arguments as video: a `'VIDE'` entry whose payload
is the file verbatim, **always stored uncompressed** (`kCodecNone`) even when
the pak is LZX'd — MPEG is already compressed, and a raw payload is what lets
the runtime stream it in place. `deploy.ps1` enumerates every `videoPath` in
the startup scene next to the `model_path` meshes. Nothing ships loose.

At runtime the shared `vid::VideoPlayer` (src/video/VideoPlayer.{h,cpp}) is
handed the entry's byte window (`Want.offset/length`) and reads it through its
**own** `CreateFileA` handle on game.spak — ~128 KB sequential reads via
pl_mpeg buffer callbacks, never the whole clip in memory, and no contention
with the streaming worker's handle. On PC (and as a dev fallback) `length = 0`
plays a plain file path.

## The player

One decode worker thread per stream (Win32 threads on both targets; pinned to
HW thread 2 on the console — render owns 0, the streaming worker owns 4).
Frames stay **YCbCr**: a 4-slot ring of padded Y/Cb/Cr planes, picked by the
host newest-due-first with late frames dropped. The `video.hlsl` builtin does
limited-range BT.601 YUV->RGB on the GPU; the CPU never converts pixels.

- Editor textures: three `D3DFMT_L8` MANAGED textures, LockRect per new frame.
- 360 textures: `D3DFMT_LIN_L8` (linear — row memcpy, no tiling, no endian
  work; L8 is bytes), double-buffered so uploads never touch the set the
  tiling replay may still reference.
- 360 draw: last inside the tiled pass; separate alpha blending
  (`SRCBLENDALPHA = ZERO`) scales the tile target's glow mask by
  (1 - video alpha) so bloom never halos through the overlay.
- Editor draw: after the bloom combine, alpha writes masked (ImGui samples the
  viewport texture's alpha).

Audio (src/video/AudioOut.{h,cpp}): MP2 frames decode on the worker into an
s16 stereo XAudio2 source voice (8-slab ring, ~150 ms queued). Once samples
play, the clock becomes `firstAudioPts + SamplesPlayed / rate` — audio is the
master; silent clips (or no audio device) advance on host dt. The console
links `xaudio2.lib` + `xmcore.lib`; on PC, `CreateMasteringVoice` needs COM
initialized on the calling thread (AudioOut handles it).

Editor semantics (the Vulkan mirror): edit mode drives the player with dt = 0
and `audible = false` — a frozen, silent first frame. The Play preview passes
real dt + audible; flipping audibility recreates the stream, so Play restarts
videos from the top and Stop returns to the frozen frame.

## The attribute

Position/size live in a **1280x720 reference space**; stretch-to-screen
ignores them. Aspect lock derives height from the clip. `priority`: higher
draws first = behind (Vulkan convention). Play modes: Off / Play Once / Loop
(Loop is the default; Off releases the decoder; a finished Play Once hides
itself). Volume/mute apply live.
Streams are keyed by object name + attribute index — renaming an object
restarts its video. Cap: 4 streams in the editor / 2 on the console
(`vid::VideoPlayer::kMaxStreams`).

## Lua

    video.play("Name")        -- play once from the top (replays if finished/stopped)
    video.play("Name", true)  -- same, but looping until stopped
    video.stop("Name")        -- forces Off (releases the decoder)
    video.is_playing("Name")  -- true while running; false once a play-once ends

A play-once video hides itself when it ends (after the last frame has shown
for its full frame period); `is_playing` flips false at the same moment.

Overrides are transient (the authored scene value is untouched); the editor
clears them when the Play session ends. See
`E:\Projects\360proj\assets\scripts\videotoggle.lua` for a working example.
