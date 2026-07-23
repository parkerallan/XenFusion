#include "video/AudioOut.h"

#include <xaudio2.h>
#ifndef _XBOX
#include <objbase.h> // CoInitializeEx — see EnsureEngine
#endif

#include <stdio.h> // sprintf_s (failure logging)
#include <string.h>

namespace vid
{
    namespace
    {
        // Process-wide engine, created on first use and kept for the process
        // lifetime (both hosts keep video/audio alive until exit anyway).
        // Creation is racy without a lock: the video decode workers and the
        // audio worker can all arrive at once — a bare "tried" flag made the
        // losers go permanently dormant (found the hard way on the console,
        // where a video and an audio stream both start at boot).
        IXAudio2*               g_engine = 0;
        IXAudio2MasteringVoice* g_master = 0;
        bool                    g_tried  = false;
        CRITICAL_SECTION        g_engineLock;
        struct EngineLockInit
        {
            EngineLockInit() { InitializeCriticalSection(&g_engineLock); }
        } g_engineLockInit; // static ctor: runs single-threaded before main

        void LogHr(const char* what, HRESULT hr)
        {
            char msg[128];
            sprintf_s(msg, sizeof(msg), "audioout: %s failed 0x%08X\n", what, (unsigned int)hr);
            OutputDebugStringA(msg);
        }

        bool EnsureEngine()
        {
            EnterCriticalSection(&g_engineLock);
            if (g_engine)
            {
                LeaveCriticalSection(&g_engineLock);
                return true;
            }
            if (g_tried)
            {
                // A previous attempt genuinely failed (no device) — dormant.
                LeaveCriticalSection(&g_engineLock);
                return false;
            }
            g_tried = true;
#ifndef _XBOX
            // CreateMasteringVoice enumerates endpoints via MMDevice, which
            // needs COM on the calling thread (we're called from a stream's
            // decode worker). S_FALSE / RPC_E_CHANGED_MODE are fine — COM is
            // simply already up in some mode.
            CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif
            bool ok = false;
            HRESULT hr = XAudio2Create(&g_engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
            if (FAILED(hr))
            {
                LogHr("XAudio2Create", hr);
                g_engine = 0;
            }
            else
            {
                hr = g_engine->CreateMasteringVoice(&g_master);
                if (FAILED(hr))
                {
                    LogHr("CreateMasteringVoice", hr);
                    g_engine->Release();
                    g_engine = 0;
                    g_master = 0;
                }
                else
                    ok = true;
            }
            LeaveCriticalSection(&g_engineLock);
            return ok;
        }
    }

    AudioOut::AudioOut() : m_voice(0), m_channels(kChannels), m_next(0) {}
    AudioOut::~AudioOut() { Close(); }

    bool AudioOut::Open(int sampleRate, int channels, float maxFreqRatio)
    {
        if (m_voice)
            return true;
        if (sampleRate <= 0 || channels < 1 || channels > kChannels || !EnsureEngine())
            return false;

        WAVEFORMATEX wfx;
        memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = (WORD)channels;
        wfx.nSamplesPerSec  = (DWORD)sampleRate;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = (WORD)(channels * 2);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

        if (maxFreqRatio < 1.0f)
            maxFreqRatio = 1.0f;
        const HRESULT hr = g_engine->CreateSourceVoice(&m_voice, &wfx, 0, maxFreqRatio);
        if (FAILED(hr))
        {
            LogHr("CreateSourceVoice", hr);
            m_voice = 0;
            return false;
        }
        m_channels = channels;
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
        memcpy(slab, samples, (size_t)frames * m_channels * sizeof(short));

        XAUDIO2_BUFFER buf;
        memset(&buf, 0, sizeof(buf));
        buf.AudioBytes = (UINT32)(frames * m_channels * sizeof(short));
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

    void AudioOut::SetFrequencyRatio(float ratio)
    {
        if (m_voice)
            m_voice->SetFrequencyRatio(ratio);
    }

    void AudioOut::SetOutputMatrix(int srcChannels, const float* gains)
    {
        if (m_voice && gains)
            m_voice->SetOutputMatrix(NULL, (UINT32)srcChannels, kChannels, gains);
    }
}
