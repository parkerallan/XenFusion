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
            // ~620ms of queued audio. Deep on purpose: under Xenia, guest
            // worker threads schedule in bursts, and a pitched-up voice drains
            // faster than nominal — a shallow queue (the original 6) starved
            // intermittently and clicked. Params (volume/pitch/matrix) are
            // voice-level, not buffered, so responsiveness is unaffected.
            kMaxQueue  = 24,   // throttle: worker stops submitting at this depth
            kSlabs     = 32,   // owned buffers; > kMaxQueue so reuse is safe
            kMaxFrames = 1152, // MP2 frame: samples per channel
            kChannels  = 2
        };

        AudioOut();
        ~AudioOut();

        // Create the source voice (starts immediately). False if the engine or
        // voice can't be created — the caller runs silent. channels: 2 for
        // video/2D audio, 1 for spatialized emitters. maxFreqRatio bounds
        // SetFrequencyRatio (pitch * doppler); the default matches XAudio2's.
        bool Open(int sampleRate, int channels = kChannels, float maxFreqRatio = 2.0f);
        void Close();
        bool IsOpen() const { return m_voice != 0; }

        // Copy interleaved s16 (m_channels wide) into an owned slab and queue
        // it. frames = samples per channel (<= kMaxFrames).
        bool Submit(const short* samples, int frames);

        int  QueuedBuffers() const;

        // Total samples the voice has consumed since Open — the master clock.
        unsigned __int64 SamplesPlayed() const;

        void SetVolume(float volume); // linear gain (mute = 0)

        // Playback-rate multiplier (pitch * doppler), clamped by Open's
        // maxFreqRatio. No-op before the voice exists.
        void SetFrequencyRatio(float ratio);

        // Per-channel gains into the stereo mastering voice: srcChannels *
        // 2 coefficients (the X3DAudio matrix). No-op before the voice exists.
        void SetOutputMatrix(int srcChannels, const float* gains);

    private:
        IXAudio2SourceVoice* m_voice;
        int   m_channels; // set by Open
        short m_slabs[kSlabs][kMaxFrames * kChannels];
        int   m_next;

        AudioOut(const AudioOut&);
        AudioOut& operator=(const AudioOut&);
    };
}

#endif // VIDEO_AUDIOOUT_H
