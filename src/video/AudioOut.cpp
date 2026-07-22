#include "video/AudioOut.h"

#include <xaudio2.h>
#ifndef _XBOX
#include <objbase.h> // CoInitializeEx — see EnsureEngine
#endif

#include <string.h>

namespace vid
{
    namespace
    {
        // Process-wide engine, created on first use and kept for the process
        // lifetime (both hosts keep video/audio alive until exit anyway).
        IXAudio2*               g_engine = 0;
        IXAudio2MasteringVoice* g_master = 0;
        bool                    g_tried  = false;

        bool EnsureEngine()
        {
            if (g_engine)
                return true;
            if (g_tried)
                return false;
            g_tried = true;
#ifndef _XBOX
            // CreateMasteringVoice enumerates endpoints via MMDevice, which
            // needs COM on the calling thread (we're called from a stream's
            // decode worker). S_FALSE / RPC_E_CHANGED_MODE are fine — COM is
            // simply already up in some mode.
            CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif
            if (FAILED(XAudio2Create(&g_engine, 0, XAUDIO2_DEFAULT_PROCESSOR)))
            {
                g_engine = 0;
                return false;
            }
            if (FAILED(g_engine->CreateMasteringVoice(&g_master)))
            {
                g_engine->Release();
                g_engine = 0;
                g_master = 0;
                return false;
            }
            return true;
        }
    }

    AudioOut::AudioOut() : m_voice(0), m_next(0) {}
    AudioOut::~AudioOut() { Close(); }

    bool AudioOut::Open(int sampleRate)
    {
        if (m_voice)
            return true;
        if (sampleRate <= 0 || !EnsureEngine())
            return false;

        WAVEFORMATEX wfx;
        memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = kChannels;
        wfx.nSamplesPerSec  = (DWORD)sampleRate;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = (WORD)(kChannels * 2);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

        if (FAILED(g_engine->CreateSourceVoice(&m_voice, &wfx)))
        {
            m_voice = 0;
            return false;
        }
        m_voice->Start(0);
        return true;
    }

    void AudioOut::Close()
    {
        if (m_voice)
        {
            m_voice->DestroyVoice(); // stops + releases queued buffers
            m_voice = 0;
        }
    }

    bool AudioOut::Submit(const short* samples, int frames)
    {
        if (!m_voice || frames <= 0)
            return false;
        if (frames > kMaxFrames)
            frames = kMaxFrames;

        short* slab = m_slabs[m_next];
        m_next = (m_next + 1) % kSlabs;
        memcpy(slab, samples, (size_t)frames * kChannels * sizeof(short));

        XAUDIO2_BUFFER buf;
        memset(&buf, 0, sizeof(buf));
        buf.AudioBytes = (UINT32)(frames * kChannels * sizeof(short));
        buf.pAudioData = (const BYTE*)slab;
        return SUCCEEDED(m_voice->SubmitSourceBuffer(&buf));
    }

    int AudioOut::QueuedBuffers() const
    {
        if (!m_voice)
            return 0;
        XAUDIO2_VOICE_STATE st;
        m_voice->GetState(&st);
        return (int)st.BuffersQueued;
    }

    unsigned __int64 AudioOut::SamplesPlayed() const
    {
        if (!m_voice)
            return 0;
        XAUDIO2_VOICE_STATE st;
        m_voice->GetState(&st);
        return st.SamplesPlayed;
    }

    void AudioOut::SetVolume(float volume)
    {
        if (m_voice)
            m_voice->SetVolume(volume);
    }
}
