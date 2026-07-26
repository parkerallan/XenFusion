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

Import in the editor: select the Video attribute's **360p / 480p / 720p**
radio, then drag a video from the Assets panel onto it. A `.mpg` binds
directly and is never resized; other containers transcode on drop via
`ffmpeg` on PATH. The profiles fit within 640x360, 854x480, or 1280x720,
preserve aspect ratio, force even dimensions/YUV420, and retain the existing
MPEG-1 + MP2 program-stream contract:

Video conversion runs as one below-normal-priority background job. The editor
remains interactive and applies the completed `.mpg` to the target attribute
on the main thread; another video drop is disabled while that job is active.

  ffmpeg -i <src> -f mpeg -c:v mpeg1video -q:v <profile quality>
       -vf "scale=<w>:<h>:force_original_aspect_ratio=decrease:force_divisible_by=2,format=yuv420p" -r 30
           -c:a mp2 -b:a 224k -ar 44100 -ac 2 <name>.mpg

Quality scales with the profile: q4 for 360p, q3 for 480p, and q2 for 720p.
360p remains the default for existing scenes. The selected import profile is
editor metadata only; playback always uses the dimensions stored in the MPEG,
so it does not restrict authored size, stretch, aspect, or play behavior.

## Shipping: raw VIDE entries, ranged reads

`spakc build` treats `.mpg` arguments as video: a `'VIDE'` entry whose payload
is the file verbatim, **always stored uncompressed** (`kCodecNone`) even when
the pak is LZX'd — MPEG is already compressed, and a raw payload is what lets
the runtime stream it in place. `deploy.ps1` enumerates every `videoPath` in
the startup scene next to the `model_path` meshes. Nothing ships loose.

At runtime the shared `vid::VideoPlayer` (src/video/VideoPlayer.{h,cpp}) is
handed the entry's byte window (`Want.offset/length`) and reads it through its
**own** overlapped `CreateFileA` handle on game.spak. Plain editor files and
SPAK ranges use the same bounded source: two 256 KB sequential blocks, with
decode consuming one while the other reads ahead. Reads never cross the
declared entry range and video duration never increases buffered memory.

## The player

One decode worker thread per stream (Win32 threads on both targets; pinned to
HW thread 2 on the console — render owns 0, the streaming worker owns 4).
That worker now owns file open, MPEG probe, decode, audio setup, fade, and
media cleanup. `VideoPlayer::Update` creates/signals lightweight stream state
and retires stopped streams without waiting for I/O or joining a worker;
`Shutdown` is the final blocking join. Frames stay **YCbCr**: a 4-slot ring of
padded Y/Cb/Cr planes, picked by the host newest-due-first with late frames
dropped. The `video.hlsl` builtin does limited-range BT.601 YUV->RGB on the
GPU; the CPU never converts pixels.

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
