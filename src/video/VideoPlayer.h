#ifndef VIDEO_VIDEOPLAYER_H
#define VIDEO_VIDEOPLAYER_H

// Shared video playback core (mirrors the Vulkan engine's VideoPlaybackManager):
// per-stream decode worker thread over the vendored pl_mpeg (MPEG-1 + MP2),
// a small ring of decoded YUV frames, and a display clock the host advances.
// Strict C++03 + Win32 threads so the same source compiles into the VS2022
// editor and the VS2010 Xbox 360 runtime (like PhysicsWorld / ScriptVM).
//
// Streams are keyed by the host (object name + attribute index); renaming an
// object restarts its video. The host calls Update() once per frame with the
// full wanted set — new keys open, vanished keys close, dt=0 freezes the clock
// (the editor's edit-mode first-frame preview).
//
// Frames stay YUV (Y full res, Cb/Cr half) — the video.hlsl builtin converts
// to RGB on the GPU. Plane rows are pl_mpeg's macroblock-padded width; the
// display area is width x height at the top left (uv max = width / planeWidth).
//
// Audio (MP2 -> XAudio2) is Phase 3; volume/muted are carried but unused.

#ifdef _XBOX
#include <xtl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <string>
#include <vector>

namespace vid
{
    // Play modes (matches the Video attribute's play_mode).
    enum PlayMode { PlayOff = 0, PlayOnce = 1, PlayLoop = 2 };

    // What the host wants playing this frame. With length == 0 the path is a
    // plain .mpg file; with length > 0 it names a container (game.spak) and the
    // stream is the [offset, offset+length) byte range inside it — the console's
    // in-pak video, read progressively through the player's own file handle.
    struct Want
    {
        std::string  key;      // stable stream identity (object name # attr index)
        std::string  path;     // .mpg file, or the container when length > 0
        int          playMode; // PlayMode
        unsigned int offset;   // byte range within the container (length > 0)
        unsigned int length;

        // Audio: when audible, the MP2 track plays through XAudio2 and the
        // video clock slaves to the samples-played cursor. The editor passes
        // audible only in Play mode — flipping it restarts the stream from the
        // top (which is also the Vulkan editor's Play/Stop behavior). Volume
        // and mute apply live.
        bool  audible;
        float volume; // 0..1
        bool  muted;

        Want() : playMode(PlayLoop), offset(0), length(0),
                 audible(true), volume(1.0f), muted(false) {}
    };

    // A decoded frame, valid until the next Update() call.
    struct Frame
    {
        const unsigned char* y;   // padded planes, top-left display area
        const unsigned char* cb;
        const unsigned char* cr;
        int yW, yH;               // padded plane sizes (row pitch = width)
        int cW, cH;               // chroma planes (half res, padded)
        int width, height;        // display size within the Y plane
        unsigned frameId;         // bumps when the picture changes (re-upload cue)
    };

    struct VideoStream; // internal (VideoPlayer.cpp)

    class VideoPlayer
    {
    public:
        VideoPlayer();
        ~VideoPlayer();

        // Reconcile the wanted set and advance every open stream's clock by dt
        // seconds (clamped; 0 = hold, which still decodes the first frame).
        void Update(const Want* wants, int count, float dt);

        // The frame to show for a key, or false if none decoded yet (host skips
        // the draw). Pointers are stable until the next Update().
        bool GetFrame(const std::string& key, Frame& out) const;

        // True while the key's stream is open and hasn't finished (a Play-Once
        // that ran out reports false; a Loop always reports true).
        bool IsPlaying(const std::string& key) const;

        // Whether a stream exists for the key at all (open, playing or ended).
        // Lets hosts distinguish "finished" from "not opened yet".
        bool HasStream(const std::string& key) const;

        // Close every stream (also called by the destructor).
        void Shutdown();

    private:
        VideoStream* Find(const std::string& key) const;

        std::vector<VideoStream*> m_streams;

        // Streams beyond this many wants are ignored (warned once by the host).
        enum { kMaxStreams = 4 };

        // Non-copyable (owns threads and D3D-agnostic buffers).
        VideoPlayer(const VideoPlayer&);
        VideoPlayer& operator=(const VideoPlayer&);
    };
}

#endif // VIDEO_VIDEOPLAYER_H
