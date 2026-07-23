#include "audio/AudioPlayer.h"
#include "video/AudioOut.h"

#include <math.h>  // expf (parameter smoothing)
#include <stdio.h> // pl_mpeg's declarations use FILE but only its implementation includes stdio
#include "plmpeg/pl_mpeg.h" // public API only — the implementation lives in VideoPlayer.cpp

// One worker thread services every stream under the player lock: the decode
// itself is sub-millisecond per 26ms MP2 frame, so even the full stream cap
// stays cheap. The only long operation — reading a clip's encoded bytes at
// voice start — runs OUTSIDE the lock (the stream is flagged busy so the host
// can't delete it mid-read). All other stream state is only ever touched with
// the lock held, by either thread.

namespace aud
{
    struct AudioStream
    {
        std::string  key;
        std::string  path;
        unsigned int offset;
        unsigned int length;    // 0 = whole file
        bool  loop;
        float volume;
        float pitch;

        // 3D emitter state (fixed spatial-ness at open; a flip recreates the
        // stream). Position/params refresh from the want each frame; velocity
        // derives from successive positions on the host thread.
        bool  spatial;
        SpatialParams sp;
        float prevPos[3];
        bool  prevPosValid;

        // Parameter smoothing: SetOutputMatrix / SetFrequencyRatio apply
        // instantly, so per-frame steps click (worst near hard pan, where the
        // curve is steep) and finite-difference velocities jitter the doppler.
        // Everything audible converges exponentially (~80ms) instead.
        float gainS[2];   // smoothed matrix
        float ratioS;     // smoothed pitch * doppler
        float velS[3];    // smoothed emitter velocity
        bool  smoothInit; // first frame snaps to target

        // Last values actually pushed to the voice — identical values are not
        // re-sent (XAudio2 param calls aren't free, especially under Xenia).
        float appliedVol;
        float appliedRatio;
        float appliedGain[2];

        // Lifecycle: worker opens (reads bytes + creates decoder/voice), then
        // pumps; ended = decoder dry on a non-loop; finished = ended + voice
        // drained (IsPlaying goes false). A stopped AUDIBLE stream doesn't die
        // instantly — cutting a voice mid-waveform clicks — it goes `closing`
        // and the worker fades it to silence (~75ms) before destroying it.
        bool  opened;
        bool  busy;      // worker is reading bytes outside the lock — don't delete
        bool  remove;    // host wants it gone NOW (never audible); worker reaps
        bool  closing;   // fade to silence, then the worker reaps
        float closeGain; // 1 -> 0 across worker passes
        bool  failed;    // unreadable/undecodable — dormant record
        bool  ended;
        bool  finished;

        std::vector<unsigned char> bytes; // encoded clip, read once at start
        plm_audio_t* dec;
        int          sampleRate;
        vid::AudioOut voice;

        AudioStream()
            : offset(0), length(0), loop(false), volume(1.0f), pitch(1.0f),
              spatial(false), prevPosValid(false),
              ratioS(1.0f), smoothInit(false),
              opened(false), busy(false), remove(false),
              closing(false), closeGain(1.0f), failed(false),
              ended(false), finished(false),
              dec(NULL), sampleRate(0)
        {
            prevPos[0] = prevPos[1] = prevPos[2] = 0.0f;
            gainS[0] = gainS[1] = 1.0f;
            velS[0] = velS[1] = velS[2] = 0.0f;
            appliedVol = appliedRatio = -1.0f;
            appliedGain[0] = appliedGain[1] = -1.0f;
        }
    };

    namespace
    {
        // Read a whole file or a byte window of a container into `out`.
        bool ReadClipBytes(const std::string& path, unsigned int offset,
                           unsigned int length, std::vector<unsigned char>& out)
        {
            HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (f == INVALID_HANDLE_VALUE)
                return false;
            unsigned int size = length;
            if (size == 0)
            {
                const DWORD fileSize = GetFileSize(f, NULL);
                if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
                { CloseHandle(f); return false; }
                size = fileSize;
            }
            LONG hi = 0;
            SetFilePointer(f, (LONG)offset, &hi, FILE_BEGIN);
            out.resize(size);
            DWORD got = 0;
            const BOOL ok = ReadFile(f, &out[0], size, &got, NULL);
            CloseHandle(f);
            if (!ok || got != size)
            { out.clear(); return false; }
            return true;
        }
    }

    AudioPlayer::AudioPlayer()
        : m_thread(NULL), m_wake(NULL), m_stop(0), m_listenerPrevValid(false)
    {
        m_listenerPrev[0] = m_listenerPrev[1] = m_listenerPrev[2] = 0.0f;
        m_listenerVelS[0] = m_listenerVelS[1] = m_listenerVelS[2] = 0.0f;
        InitializeCriticalSection(&m_lock);
    }

    AudioPlayer::~AudioPlayer() { Shutdown(); DeleteCriticalSection(&m_lock); }

    void AudioPlayer::Shutdown()
    {
        if (m_thread)
        {
            InterlockedExchange(&m_stop, 1);
            SetEvent(m_wake);
            WaitForSingleObject(m_thread, INFINITE);
            CloseHandle(m_thread);
            m_thread = NULL;
        }
        if (m_wake) { CloseHandle(m_wake); m_wake = NULL; }
        InterlockedExchange(&m_stop, 0);
        for (size_t i = 0; i < m_streams.size(); ++i)
        {
            AudioStream* s = m_streams[i];
            if (s->dec)
                plm_audio_destroy(s->dec); // owns + frees its buffer
            delete s;
        }
        m_streams.clear();
    }

    void AudioPlayer::EnsureWorker()
    {
        if (m_thread)
            return;
        m_wake   = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
        m_thread = CreateThread(NULL, 0, WorkerProc, this, 0, NULL);
#ifdef _XBOX
        if (m_thread)
            XSetThreadProcessor(m_thread, 3); // core 1 (video decode owns 2)
#endif
    }

    AudioStream* AudioPlayer::Find(const std::string& key) const
    {
        for (size_t i = 0; i < m_streams.size(); ++i)
            if (m_streams[i]->key == key)
                return m_streams[i];
        return NULL;
    }

    void AudioPlayer::Update(const Want* wants, int count, float dt,
                             const ListenerState* listener)
    {
        // Exponential smoothing factor for everything audible (~80ms).
        const float alpha = (dt > 1e-4f) ? (1.0f - expf(-dt / 0.08f)) : 0.0f;

        // Listener velocity from successive frames (doppler), smoothed —
        // raw finite differences jitter audibly through the doppler shift.
        ListenerState lst;
        if (listener)
        {
            lst = *listener;
            float rawVel[3] = { 0.0f, 0.0f, 0.0f };
            if (m_listenerPrevValid && dt > 1e-4f)
                for (int k = 0; k < 3; ++k)
                    rawVel[k] = (lst.pos[k] - m_listenerPrev[k]) / dt;
            for (int k = 0; k < 3; ++k)
            {
                m_listenerPrev[k] = lst.pos[k];
                m_listenerVelS[k] += (rawVel[k] - m_listenerVelS[k]) * alpha;
                lst.vel[k] = m_listenerVelS[k];
            }
            m_listenerPrevValid = true;
        }
        else
        {
            m_listenerPrevValid = false;
            m_listenerVelS[0] = m_listenerVelS[1] = m_listenerVelS[2] = 0.0f;
        }

        EnterCriticalSection(&m_lock);

        // Close streams whose key vanished (a not-wanted stream IS a stop) or
        // whose spatial-ness flipped (the voice's channel count is fixed at
        // open); apply live params + the 3D matrix/doppler to the rest.
        for (size_t i = 0; i < m_streams.size(); )
        {
            AudioStream* s = m_streams[i];
            const Want* w = NULL;
            for (int k = 0; k < count; ++k)
                if (wants[k].key == s->key) { w = &wants[k]; break; }
            if (s->closing)
            { ++i; continue; } // fading out on the worker; leave it alone
            if (!w || w->spatial != s->spatial)
            {
                if (s->opened)
                    s->closing = true; // audible — fade, don't cut (declick)
                else if (s->busy)
                    s->remove = true;  // worker is mid-read; it reaps afterwards
                else
                {
                    if (s->dec)
                        plm_audio_destroy(s->dec);
                    delete s; // never audible — safe to drop instantly
                    m_streams.erase(m_streams.begin() + i);
                    continue;
                }
                ++i;
                continue;
            }
            s->loop   = w->loop;
            s->volume = w->volume;
            s->pitch  = w->pitch;

            // Emitter params + velocity from successive want positions
            // (smoothed — see the smoothing note on the stream fields).
            s->sp.minDist = w->minDist;
            s->sp.maxDist = w->maxDist;
            s->sp.doppler = w->doppler;
            float rawVel[3] = { 0.0f, 0.0f, 0.0f };
            if (s->prevPosValid && dt > 1e-4f)
                for (int k = 0; k < 3; ++k)
                    rawVel[k] = (w->pos[k] - s->prevPos[k]) / dt;
            for (int k = 0; k < 3; ++k)
            {
                s->velS[k] += (rawVel[k] - s->velS[k]) * alpha;
                s->sp.vel[k] = s->velS[k];
                s->sp.pos[k] = w->pos[k];
                s->prevPos[k] = w->pos[k];
            }
            s->prevPosValid = true;

            // Voice params from the host thread (XAudio2 calls are
            // thread-safe; the voice's lifetime is guarded by m_lock).
            // Identical values are NOT re-sent — see appliedVol/Ratio/Gain.
            if (s->opened)
            {
                const float vol = s->volume < 0.0f ? 0.0f : s->volume;
                if (vol != s->appliedVol)
                {
                    s->voice.SetVolume(vol);
                    s->appliedVol = vol;
                }
                float ratio = s->pitch;
                if (s->spatial)
                {
                    float gains[2];
                    float doppler = 1.0f;
                    Spatial3D::Calculate(lst, s->sp, gains, &doppler);
                    ratio = s->pitch * doppler;
                    if (!s->smoothInit)
                    {
                        s->gainS[0] = gains[0];
                        s->gainS[1] = gains[1];
                        s->ratioS   = ratio;
                        s->smoothInit = true;
                    }
                    else
                    {
                        s->gainS[0] += (gains[0] - s->gainS[0]) * alpha;
                        s->gainS[1] += (gains[1] - s->gainS[1]) * alpha;
                        s->ratioS   += (ratio    - s->ratioS)   * alpha;
                    }
                    if (fabsf(s->gainS[0] - s->appliedGain[0]) > 0.001f ||
                        fabsf(s->gainS[1] - s->appliedGain[1]) > 0.001f)
                    {
                        s->voice.SetOutputMatrix(1, s->gainS);
                        s->appliedGain[0] = s->gainS[0];
                        s->appliedGain[1] = s->gainS[1];
                    }
                    ratio = s->ratioS;
                }
                if (fabsf(ratio - s->appliedRatio) > 0.0005f)
                {
                    s->voice.SetFrequencyRatio(ratio);
                    s->appliedRatio = ratio;
                }
            }
            ++i;
        }

        // Open newly wanted streams (capped); the worker does the heavy open.
        bool added = false;
        for (int k = 0; k < count; ++k)
        {
            if (wants[k].path.empty() || Find(wants[k].key))
                continue;
            if ((int)m_streams.size() >= (int)kMaxStreams)
                break;
            AudioStream* s = new AudioStream();
            s->key     = wants[k].key;
            s->path    = wants[k].path;
            s->offset  = wants[k].offset;
            s->length  = wants[k].length;
            s->loop    = wants[k].loop;
            s->volume  = wants[k].volume;
            s->pitch   = wants[k].pitch;
            s->spatial = wants[k].spatial;
            s->sp.minDist = wants[k].minDist;
            s->sp.maxDist = wants[k].maxDist;
            s->sp.doppler = wants[k].doppler;
            for (int c = 0; c < 3; ++c) s->sp.pos[c] = wants[k].pos[c];
            m_streams.push_back(s);
            added = true;
        }

        LeaveCriticalSection(&m_lock);

        if (added)
        {
            EnsureWorker();
            if (m_wake)
                SetEvent(m_wake);
        }
    }

    bool AudioPlayer::IsPlaying(const std::string& key) const
    {
        EnterCriticalSection(&m_lock);
        AudioStream* s = Find(key);
        const bool playing = s && !s->failed && !s->finished &&
                             !s->remove && !s->closing;
        LeaveCriticalSection(&m_lock);
        return playing;
    }

    bool AudioPlayer::HasStream(const std::string& key) const
    {
        EnterCriticalSection(&m_lock);
        const bool has = Find(key) != NULL;
        LeaveCriticalSection(&m_lock);
        return has;
    }

    DWORD WINAPI AudioPlayer::WorkerProc(LPVOID param)
    {
        ((AudioPlayer*)param)->WorkerLoop();
        return 0;
    }

    void AudioPlayer::WorkerLoop()
    {
        for (;;)
        {
            if (m_stop)
                break;

            EnterCriticalSection(&m_lock);
            for (size_t i = 0; i < m_streams.size(); )
            {
                AudioStream* s = m_streams[i];
                // Declick fade: step a closing stream's gain down each pass
                // (~15ms apart), destroy once inaudible.
                if (s->closing && !s->busy)
                {
                    s->closeGain *= 0.55f;
                    if (s->opened)
                        s->voice.SetVolume((s->volume < 0.0f ? 0.0f : s->volume) * s->closeGain);
                    if (s->closeGain < 0.03f)
                    {
                        if (s->dec)
                            plm_audio_destroy(s->dec);
                        delete s; // AudioOut dtor stops + releases the voice
                        m_streams.erase(m_streams.begin() + i);
                        continue;
                    }
                    ++i;
                    continue;
                }
                if (s->remove && !s->busy)
                {
                    if (s->dec)
                        plm_audio_destroy(s->dec);
                    delete s;
                    m_streams.erase(m_streams.begin() + i);
                    continue;
                }
                if (s->failed || s->remove || s->closing)
                { ++i; continue; }

                if (!s->opened)
                {
                    // Heavy: read the encoded clip OUTSIDE the lock. `busy`
                    // keeps the host from deleting the stream meanwhile.
                    s->busy = true;
                    std::string  path   = s->path;
                    unsigned int offset = s->offset;
                    unsigned int length = s->length;
                    LeaveCriticalSection(&m_lock);
                    std::vector<unsigned char> bytes;
                    const bool ok = ReadClipBytes(path, offset, length, bytes);
                    EnterCriticalSection(&m_lock);
                    s->busy = false;
                    if (s->remove)
                    { ++i; continue; } // reaped on the next pass
                    if (!ok)
                    {
                        s->failed = true;
                        OutputDebugStringA("audio: clip read failed\n");
                        ++i;
                        continue;
                    }
                    s->bytes.swap(bytes);

                    plm_buffer_t* buf = plm_buffer_create_with_memory(
                        &s->bytes[0], s->bytes.size(), FALSE);
                    s->dec = plm_audio_create_with_buffer(buf, TRUE); // dec owns buf
                    if (!s->dec || !plm_audio_has_header(s->dec))
                    {
                        if (s->dec) { plm_audio_destroy(s->dec); s->dec = NULL; }
                        s->failed = true;
                        ++i;
                        continue;
                    }
                    s->sampleRate = plm_audio_get_samplerate(s->dec);
                    // Spatial emitters are mono (the X3DAudio matrix is 1->2);
                    // 2D streams keep the clip's stereo.
                    const int channels = s->spatial ? 1 : vid::AudioOut::kChannels;
                    if (!s->voice.Open(s->sampleRate, channels, 8.0f))
                    {
                        s->failed = true; // no device — dormant
                        char msg[128];
                        sprintf_s(msg, sizeof(msg), "audio: voice open failed (rate=%d bytes=%u)\n",
                                  s->sampleRate, (unsigned int)s->bytes.size());
                        OutputDebugStringA(msg);
                        ++i;
                        continue;
                    }
                    s->opened = true;
                    s->voice.SetVolume(s->volume < 0.0f ? 0.0f : s->volume);
                    {
                        char msg[320];
                        sprintf_s(msg, sizeof(msg), "audio: stream open %dHz %ub %s\n",
                                  s->sampleRate, (unsigned int)s->bytes.size(), s->key.c_str());
                        OutputDebugStringA(msg);
                    }
                }

                // Keep the queue ~150ms ahead (volume/pitch/matrix are applied
                // by the host thread in Update).
                while (!s->ended &&
                       s->voice.QueuedBuffers() < vid::AudioOut::kMaxQueue)
                {
                    plm_samples_t* sm = plm_audio_decode(s->dec);
                    if (!sm && s->loop)
                    {
                        plm_audio_rewind(s->dec);
                        sm = plm_audio_decode(s->dec);
                        if (!sm)
                        { s->failed = true; break; } // corrupt loop — don't spin
                    }
                    if (!sm)
                    {
                        s->ended = true;
                        break;
                    }
                    short pcm[vid::AudioOut::kMaxFrames * vid::AudioOut::kChannels];
                    int n = (int)sm->count;
                    if (n > vid::AudioOut::kMaxFrames) n = vid::AudioOut::kMaxFrames;
                    if (s->spatial)
                    {
                        // Mono downmix for the 3D emitter.
                        for (int t = 0; t < n; ++t)
                        {
                            float v = 0.5f * (sm->interleaved[t * 2] +
                                              sm->interleaved[t * 2 + 1]);
                            if (v >  1.0f) v =  1.0f;
                            if (v < -1.0f) v = -1.0f;
                            pcm[t] = (short)(v * 32767.0f);
                        }
                    }
                    else
                    {
                        const int total = n * vid::AudioOut::kChannels;
                        for (int t = 0; t < total; ++t)
                        {
                            float v = sm->interleaved[t];
                            if (v >  1.0f) v =  1.0f;
                            if (v < -1.0f) v = -1.0f;
                            pcm[t] = (short)(v * 32767.0f);
                        }
                    }
                    s->voice.Submit(pcm, n);
                }
                if (s->ended && !s->finished && s->voice.QueuedBuffers() == 0)
                    s->finished = true; // one-shot fully drained

                ++i;
            }
            LeaveCriticalSection(&m_lock);

            WaitForSingleObject(m_wake, 15); // ~2 pumps per queued MP2 frame
        }
    }
}
