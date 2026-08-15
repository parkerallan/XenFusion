#include "audio/AudioPlayer.h"
#include "video/AudioOut.h"

#include <math.h>  // expf (parameter smoothing)
#include <stdio.h> // pl_mpeg's declarations use FILE but only its implementation includes stdio
#include "plmpeg/pl_mpeg.h" // public API only — the implementation lives in VideoPlayer.cpp

// One worker thread services every stream. Reads, MP2 decode, PCM conversion,
// and XAudio submission run outside the player lock; `busy` pins the stream so
// the host can reconcile parameters without waiting for a decode burst and
// cannot delete the stream while worker-owned state is in use.

namespace aud
{
    struct StreamSource
    {
        ClipReader* reader;
        std::string path;
        unsigned int base;
        unsigned int size;
        unsigned int pos;
        bool failed;
        std::vector<unsigned char> chunk;

        StreamSource()
            : reader(NULL), base(0), size(0), pos(0), failed(false) {}
    };

    namespace
    {
        void StreamLoad(plm_buffer_t* buffer, void* user)
        {
            StreamSource* source = (StreamSource*)user;
            if (!source || source->failed || source->pos >= source->size)
            {
                plm_buffer_signal_end(buffer);
                return;
            }

            const unsigned int remaining = source->size - source->pos;
            const unsigned int requested = remaining < AudioPlayer::kStreamChunkBytes
                                               ? remaining : AudioPlayer::kStreamChunkBytes;
            source->chunk.clear();
            const bool ok = source->reader->Read(
                source->path, source->base + source->pos, requested,
                source->chunk);
            if (!ok || source->chunk.size() != requested)
            {
                source->failed = true;
                source->chunk.clear();
                plm_buffer_signal_end(buffer);
                return;
            }

            plm_buffer_write(buffer, &source->chunk[0], source->chunk.size());
            source->pos += (unsigned int)source->chunk.size();
            if (source->pos >= source->size)
                plm_buffer_signal_end(buffer);
        }

        void StreamSeek(plm_buffer_t*, size_t offset, void* user)
        {
            StreamSource* source = (StreamSource*)user;
            source->pos = offset < source->size ? (unsigned int)offset : source->size;
            source->failed = false;
        }

        size_t StreamTell(plm_buffer_t*, void* user)
        {
            return (size_t)((StreamSource*)user)->pos;
        }
    }

    struct EncodedClip
    {
        std::string path;
        unsigned int offset;
        unsigned int length;
        unsigned int references;
        unsigned int lastUse;
        std::vector<unsigned char> bytes;

        EncodedClip() : offset(0), length(0), references(0), lastUse(0) {}
    };

    struct AudioStream
    {
        std::string  key;
        std::string  path;
        unsigned int offset;
        unsigned int length;    // 0 = whole file
        bool  loop;
        float volume;
        float pitch;
        int   loadMode;

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
        std::vector<unsigned char> bytes; // long/nonresident clip bytes
        EncodedClip* cached;              // shared short-clip bytes
        StreamSource* source;             // callback-backed long-track reader
        plm_audio_t* dec;
        int          sampleRate;
        vid::AudioOut voice;

        AudioStream()
                        : offset(0), length(0), loop(false), volume(1.0f), pitch(1.0f),
                            loadMode(AudioLoadAuto),
              spatial(false), prevPosValid(false),
              ratioS(1.0f), smoothInit(false),
              opened(false), busy(false), remove(false),
              closing(false), closeGain(1.0f), failed(false),
              ended(false), finished(false),
              cached(NULL), source(NULL), dec(NULL), sampleRate(0)
        {
            prevPos[0] = prevPos[1] = prevPos[2] = 0.0f;
            gainS[0] = gainS[1] = 1.0f;
            velS[0] = velS[1] = velS[2] = 0.0f;
            appliedVol = appliedRatio = -1.0f;
            appliedGain[0] = appliedGain[1] = -1.0f;
        }

                ~AudioStream()
                {
                        if (cached && cached->references > 0)
                                --cached->references;
                    delete source;
                }
    };

    AudioPlayer::AudioPlayer(ClipReader* reader)
                : m_cacheBytes(0), m_cacheClock(0), m_thread(NULL), m_wake(NULL),
                    m_stop(0),
          m_reader(reader ? reader : &m_defaultReader), m_listenerPrevValid(false)
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
        for (size_t i = 0; i < m_cache.size(); ++i)
            delete m_cache[i];
        m_cache.clear();
        m_cacheBytes = 0;
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

        std::vector<VoiceCandidate> candidates;
        candidates.resize(count > 0 ? (size_t)count : 0);
        for (int index = 0; index < count; ++index)
        {
            VoiceCandidate& candidate = candidates[index];
            candidate.key = wants[index].key;
            candidate.audioClass = wants[index].audioClass;
            candidate.priority = wants[index].priority;
            candidate.spatial = wants[index].spatial;
            candidate.loop = wants[index].loop;
            candidate.maxDistance = wants[index].maxDist;
            if (listener && wants[index].spatial)
            {
                const float dx = wants[index].pos[0] - listener->pos[0];
                const float dy = wants[index].pos[1] - listener->pos[1];
                const float dz = wants[index].pos[2] - listener->pos[2];
                candidate.distance = sqrtf(dx * dx + dy * dy + dz * dz);
            }
        }
        const VoiceSelection selection = SelectVoices(
            candidates.empty() ? NULL : &candidates[0], count, kMaxStreams);
        std::vector<Want> admitted;
        admitted.reserve(selection.admitted.size());
        for (size_t index = 0; index < selection.admitted.size(); ++index)
            admitted.push_back(wants[selection.admitted[index]]);
        const Want* activeWants = admitted.empty() ? NULL : &admitted[0];
        const int activeCount = (int)admitted.size();

        EnterCriticalSection(&m_lock);

        // Close streams whose key vanished (a not-wanted stream IS a stop) or
        // whose spatial-ness flipped (the voice's channel count is fixed at
        // open); apply live params + the 3D matrix/doppler to the rest.
        for (size_t i = 0; i < m_streams.size(); )
        {
            AudioStream* s = m_streams[i];
            const Want* w = NULL;
            for (int k = 0; k < activeCount; ++k)
                if (activeWants[k].key == s->key) { w = &activeWants[k]; break; }
            if (s->closing)
            { ++i; continue; } // fading out on the worker; leave it alone
            if (!w || w->spatial != s->spatial)
            {
                if (s->opened)
                {
                    s->closing = true; // audible — fade, don't cut (declick)
                }
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
        int liveStreams = 0;
        for (size_t index = 0; index < m_streams.size(); ++index)
            if (!m_streams[index]->closing && !m_streams[index]->remove)
                ++liveStreams;
        for (int k = 0; k < activeCount; ++k)
        {
            if (activeWants[k].path.empty() || Find(activeWants[k].key))
                continue;
            if (liveStreams >= (int)kMaxStreams)
                break;
            AudioStream* s = new AudioStream();
            s->key     = activeWants[k].key;
            s->path    = activeWants[k].path;
            s->offset  = activeWants[k].offset;
            s->length  = activeWants[k].length;
            s->loop    = activeWants[k].loop;
            s->volume  = activeWants[k].volume;
            s->pitch   = activeWants[k].pitch;
            s->loadMode = activeWants[k].loadMode;
            s->spatial = activeWants[k].spatial;
            s->sp.minDist = activeWants[k].minDist;
            s->sp.maxDist = activeWants[k].maxDist;
            s->sp.doppler = activeWants[k].doppler;
            for (int c = 0; c < 3; ++c) s->sp.pos[c] = activeWants[k].pos[c];
            m_streams.push_back(s);
            ++liveStreams;
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

    float AudioPlayer::PlaybackSeconds(const std::string& key) const
    {
        EnterCriticalSection(&m_lock);
        AudioStream* s = Find(key);
        float seconds = -1.0f;
        // Before the worker opens the voice there is no clock yet; report 0 so
        // a just-started clip holds its first frame rather than reading absent.
        if (s && !s->failed && !s->remove)
            seconds = (s->voice.IsOpen() && s->sampleRate > 0)
                ? (float)((double)s->voice.SamplesPlayed() / (double)s->sampleRate)
                : 0.0f;
        LeaveCriticalSection(&m_lock);
        return seconds;
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
                    if (s->loadMode == AudioLoadStream && !s->source)
                    {
                        unsigned int sourceSize = s->length;
                        bool sizeOk = true;
                        if (sourceSize == 0)
                        {
                            s->busy = true;
                            const std::string path = s->path;
                            LeaveCriticalSection(&m_lock);
                            sizeOk = m_reader->Size(path, sourceSize);
                            EnterCriticalSection(&m_lock);
                            s->busy = false;
                        }
                        if (!sizeOk)
                        {
                            s->failed = true;
                            ++i;
                            continue;
                        }
                        s->source = new StreamSource();
                        s->source->reader = m_reader;
                        s->source->path = s->path;
                        s->source->base = s->offset;
                        s->source->size = sourceSize;
                        plm_buffer_t* streamBuffer = plm_buffer_create_with_callbacks(
                            StreamLoad, StreamSeek, StreamTell, sourceSize, s->source);
                        s->dec = plm_audio_create_with_buffer(streamBuffer, TRUE);
                    }

                    for (size_t cacheIndex = 0;
                         !s->dec && s->loadMode != AudioLoadStream && cacheIndex < m_cache.size();
                        ++cacheIndex)
                    {
                        EncodedClip* clip = m_cache[cacheIndex];
                        if (clip->path == s->path && clip->offset == s->offset &&
                            clip->length == s->length)
                        {
                            s->cached = clip;
                            ++clip->references;
                            clip->lastUse = ++m_cacheClock;
                            break;
                        }
                    }

                    if (!s->dec && !s->cached)
                    {
                        // Heavy: read the encoded clip OUTSIDE the lock. `busy`
                        // keeps the host from deleting the stream meanwhile.
                        s->busy = true;
                        std::string  path   = s->path;
                        unsigned int offset = s->offset;
                        unsigned int length = s->length;
                        LeaveCriticalSection(&m_lock);
                        std::vector<unsigned char> bytes;
                        const bool ok = m_reader->Read(path, offset, length, bytes);
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
                        const bool cacheable =
                            s->loadMode == AudioLoadResident
                                ? bytes.size() <= kEncodedCacheBudgetBytes
                                : s->loadMode == AudioLoadAuto &&
                                  bytes.size() <= kResidentThresholdBytes;
                        if (cacheable)
                        {
                            while (m_cacheBytes + bytes.size() > kEncodedCacheBudgetBytes)
                            {
                                size_t oldest = m_cache.size();
                                for (size_t cacheIndex = 0; cacheIndex < m_cache.size(); ++cacheIndex)
                                    if (m_cache[cacheIndex]->references == 0 &&
                                        (oldest == m_cache.size() ||
                                         m_cache[cacheIndex]->lastUse < m_cache[oldest]->lastUse))
                                        oldest = cacheIndex;
                                if (oldest == m_cache.size())
                                    break;
                                m_cacheBytes -= (unsigned int)m_cache[oldest]->bytes.size();
                                delete m_cache[oldest];
                                m_cache.erase(m_cache.begin() + oldest);
                            }
                            if (m_cacheBytes + bytes.size() <= kEncodedCacheBudgetBytes)
                            {
                                EncodedClip* clip = new EncodedClip();
                                clip->path = path;
                                clip->offset = offset;
                                clip->length = length;
                                clip->references = 1;
                                clip->lastUse = ++m_cacheClock;
                                clip->bytes.swap(bytes);
                                m_cacheBytes += (unsigned int)clip->bytes.size();
                                m_cache.push_back(clip);
                                s->cached = clip;
                            }
                        }
                        if (!s->cached)
                            s->bytes.swap(bytes);
                    }

                    if (!s->dec)
                    {
                        std::vector<unsigned char>& encoded =
                            s->cached ? s->cached->bytes : s->bytes;
                        plm_buffer_t* buf = plm_buffer_create_with_memory(
                            &encoded[0], encoded.size(), FALSE);
                        s->dec = plm_audio_create_with_buffer(buf, TRUE); // dec owns buf
                    }
                    bool hasHeader = false;
                    if (s->dec && s->source)
                    {
                        s->busy = true;
                        LeaveCriticalSection(&m_lock);
                        hasHeader = plm_audio_has_header(s->dec) != 0;
                        EnterCriticalSection(&m_lock);
                        s->busy = false;
                    }
                    else if (s->dec)
                        hasHeader = plm_audio_has_header(s->dec) != 0;
                    if (!s->dec || !hasHeader)
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
                        sprintf_s(msg, sizeof(msg), "audio: voice open failed (rate=%d)\n",
                                  s->sampleRate);
                        OutputDebugStringA(msg);
                        ++i;
                        continue;
                    }
                    s->opened = true;
                    s->voice.SetVolume(s->volume < 0.0f ? 0.0f : s->volume);
                    {
                        char msg[320];
                        sprintf_s(msg, sizeof(msg), "audio: stream open %dHz %s %s\n",
                                  s->sampleRate, s->source ? "streamed" : "resident",
                                  s->key.c_str());
                        OutputDebugStringA(msg);
                    }
                }

                // Keep the output queue full (volume/pitch/matrix are applied
                // by the host thread in Update).
                while (!s->ended && s->voice.QueuedBuffers() < vid::AudioOut::kMaxQueue)
                {
                    const bool shouldLoop = s->loop;
                    const bool spatial = s->spatial;
                    s->busy = true;
                    LeaveCriticalSection(&m_lock);
                    plm_samples_t* sm = plm_audio_decode(s->dec);
                    bool corruptLoop = false;
                    if (!sm && shouldLoop)
                    {
                        plm_audio_rewind(s->dec);
                        sm = plm_audio_decode(s->dec);
                        if (!sm)
                            corruptLoop = true;
                    }
                    short pcm[vid::AudioOut::kMaxFrames * vid::AudioOut::kChannels];
                    int n = 0;
                    if (sm)
                    {
                        n = (int)sm->count;
                        if (n > vid::AudioOut::kMaxFrames) n = vid::AudioOut::kMaxFrames;
                    }
                    if (sm && spatial)
                    {
                        for (int sample = 0; sample < n; ++sample)
                        {
                            float v = 0.5f * (sm->interleaved[sample * 2] +
                                              sm->interleaved[sample * 2 + 1]);
                            if (v >  1.0f) v =  1.0f;
                            if (v < -1.0f) v = -1.0f;
                            pcm[sample] = (short)(v * 32767.0f);
                        }
                    }
                    else if (sm)
                    {
                        const int total = n * vid::AudioOut::kChannels;
                        for (int sample = 0; sample < total; ++sample)
                        {
                            float v = sm->interleaved[sample];
                            if (v >  1.0f) v =  1.0f;
                            if (v < -1.0f) v = -1.0f;
                            pcm[sample] = (short)(v * 32767.0f);
                        }
                    }
                    if (sm)
                        s->voice.Submit(pcm, n);
                    const bool sourceFailed = s->source && s->source->failed;
                    EnterCriticalSection(&m_lock);
                    s->busy = false;
                    if (sourceFailed || corruptLoop)
                    {
                        s->failed = true;
                        break;
                    }
                    if (!sm)
                    {
                        s->ended = true;
                        break;
                    }
                    if (s->remove || s->closing)
                        break;
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
