#ifndef AUDIO_AUDIOPLAYER_H
#define AUDIO_AUDIOPLAYER_H

// Shared audio playback core (mirrors the Vulkan engine's AudioEngine +
// per-frame reconciler): keyed streams decoded from raw MP2 elementary
// streams (pl_mpeg's standalone plm_audio) into XAudio2 voices (AudioOut).
// Strict C++03 + Win32 threads — the same source compiles into the VS2022
// editor and the VS2010 Xbox 360 runtime.
//
// The host calls Update() once per frame with the full wanted set; new keys
// open, vanished/not-playing keys close. ONE worker thread services every
// stream: it reads the clip's encoded bytes once at voice start (whole file
// or a byte window of the pak — the Vulkan engine's exact strategy: encoded
// bytes live in RAM, PCM decodes progressively, disk is touched only at
// start). Short clips share a bounded encoded-byte cache across voices; each
// voice retains its own decoder cursor and PCM queue.
//
// A non-looping stream that reaches its end reports IsPlaying() == false once
// the voice drains (the host's one-shot lifecycle); restarting is the host's
// job via a fresh key (the video system's restart-generation pattern).
//
// Spatialization (X3DAudio) lands in a later phase; Want already carries the
// 2D fields only.

#ifdef _XBOX
#include <xtl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <string>
#include <vector>

#include "audio/AudioClipReader.h"
#include "audio/Spatial3D.h"
#include "audio/AudioVoicePolicy.h"

namespace aud
{
    // What the host wants audible this frame. With length == 0 the path is a
    // plain .mp2 file; with length > 0 it names a container (game.spak) and
    // the clip is the [offset, offset+length) byte range inside it.
    struct Want
    {
        std::string  key;    // stable stream identity (object name # attr index [# gen])
        std::string  path;
        unsigned int offset;
        unsigned int length;
        bool  loop;
        float volume;        // linear, 0..20 (inspector clamp)
        float pitch;         // 0.1..4 -> SetFrequencyRatio
        int   audioClass;     // AudioClass; policy protection group
        int   priority;       // explicit authored rank
        int   loadMode;       // AudioLoadMode; streaming phase consumes this

        // 3D emitter (X3DAudio). A spatial stream's voice is MONO (the worker
        // downmixes); flipping `spatial` recreates the stream. pos is the
        // owning object's live world position — the player derives velocity
        // from successive frames itself.
        bool  spatial;
        float minDist, maxDist, doppler;
        float pos[3];

        Want() : offset(0), length(0), loop(false), volume(1.0f), pitch(1.0f),
                 audioClass(AudioEffect), priority(0), loadMode(AudioLoadAuto),
                 spatial(false), minDist(1.0f), maxDist(50.0f), doppler(1.0f)
        { pos[0] = pos[1] = pos[2] = 0.0f; }
    };

    struct AudioStream; // internal (AudioPlayer.cpp)
    struct EncodedClip; // internal shared encoded-byte cache entry

    class AudioPlayer
    {
    public:
        // A supplied reader is not owned and must outlive the player. Passing
        // NULL uses FileClipReader and preserves normal host behavior.
        explicit AudioPlayer(ClipReader* reader = NULL);
        ~AudioPlayer();

        // Reconcile the wanted set: open new keys, close vanished ones, apply
        // live volume/pitch/loop changes and the 3D matrix/doppler for spatial
        // streams. Call once per frame; an empty set silences everything (the
        // editor's edit-mode contract). listener may be NULL when nothing is
        // spatial (or the camera is unknown); dt feeds velocity derivation.
        void Update(const Want* wants, int count, float dt,
                    const ListenerState* listener);

        // True while the key's stream exists and hasn't finished (a one-shot
        // that drained reports false; a loop always reports true).
        bool IsPlaying(const std::string& key) const;
        bool HasStream(const std::string& key) const;

        void Shutdown();

        enum
        {
            kMaxStreams = 24,
            kResidentThresholdBytes = 256 * 1024,
            kEncodedCacheBudgetBytes = 16 * 1024 * 1024,
            kStreamChunkBytes = 256 * 1024
        };

    private:
        AudioStream* Find(const std::string& key) const; // caller holds m_lock

        void EnsureWorker();
        static DWORD WINAPI WorkerProc(LPVOID param);
        void WorkerLoop();

        std::vector<AudioStream*> m_streams; // under m_lock
        std::vector<EncodedClip*> m_cache;   // under m_lock
        unsigned int m_cacheBytes;
        unsigned int m_cacheClock;
        HANDLE m_thread;
        HANDLE m_wake;
        volatile LONG m_stop;
        mutable CRITICAL_SECTION m_lock;
        FileClipReader m_defaultReader;
        ClipReader* m_reader;

        // Listener velocity derivation (doppler): last frame's position plus
        // an exponentially smoothed velocity (raw deltas jitter audibly).
        float m_listenerPrev[3];
        float m_listenerVelS[3];
        bool  m_listenerPrevValid;

        AudioPlayer(const AudioPlayer&);
        AudioPlayer& operator=(const AudioPlayer&);
    };
}

#endif // AUDIO_AUDIOPLAYER_H
