#ifndef VIDEO_AUDIOOUT_H
#define VIDEO_AUDIOOUT_H

// Minimal XAudio2 output for the video system's MP2 track: one s16 stereo
// source voice per stream over a lazily-created process-wide engine. Strict
// C++03, shared by the VS2022 editor (Windows SDK XAudio2 2.9) and the VS2010
// Xbox 360 runtime (XDK XAudio2) — the two APIs differ only in a couple of
// default-argument signatures, handled inside AudioOut.cpp.
//
// Threading: Open/Submit are called by the stream's decode worker; SetVolume /
// QueuedBuffers / SamplesPlayed by the host thread (XAudio2 is thread-safe).
// Close only after the worker has been joined.

#ifdef _XBOX
#include <xtl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

struct IXAudio2SourceVoice;

namespace vid
{
    class AudioOut
    {
    public:
        enum
        {
            kMaxQueue  = 6,    // throttle: worker stops submitting at this depth
            kSlabs     = 8,    // owned buffers; > kMaxQueue so reuse is safe
            kMaxFrames = 1152, // MP2 frame: samples per channel
            kChannels  = 2
        };

        AudioOut();
        ~AudioOut();

        // Create the source voice (starts immediately). False if the engine or
        // voice can't be created — the caller runs silent.
        bool Open(int sampleRate);
        void Close();
        bool IsOpen() const { return m_voice != 0; }

        // Copy interleaved stereo s16 into an owned slab and queue it.
        // frames = samples per channel (<= kMaxFrames).
        bool Submit(const short* samples, int frames);

        int  QueuedBuffers() const;

        // Total samples the voice has consumed since Open — the master clock.
        unsigned __int64 SamplesPlayed() const;

        void SetVolume(float volume); // 0..1 (mute = 0)

    private:
        IXAudio2SourceVoice* m_voice;
        short m_slabs[kSlabs][kMaxFrames * kChannels];
        int   m_next;

        AudioOut(const AudioOut&);
        AudioOut& operator=(const AudioOut&);
    };
}

#endif // VIDEO_AUDIOOUT_H
