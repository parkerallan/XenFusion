#include "video/VideoPlayer.h"
#include "video/AudioOut.h"

#include <stdio.h> // pl_mpeg's declarations use FILE but only its implementation includes stdio

#define PL_MPEG_IMPLEMENTATION
#include "plmpeg/pl_mpeg.h"

// Per-stream decode worker + a 4-slot frame ring. The worker owns the plm
// handle and only ever touches Free slots; the host thread (Update/GetFrame)
// owns the Shown slot and the clock. The one critical section guards slot
// states and the plane-size fields; everything else is single-owner.

namespace vid
{
    struct VideoStream
    {
        enum { kSlots = 4 };
        enum SlotState { SlotFree = 0, SlotReady = 1, SlotShown = 2 };

        struct Slot
        {
            int    state;
            double pts; // monotonic across loops (loopBase + frame time)
            std::vector<unsigned char> y, cb, cr;
        };

        std::string key;
        std::string path;
        plm_t*      plm;
        HANDLE      thread;
        HANDLE      wake;
        // Container-range mode (in-pak video): our own read handle plus the
        // byte window; rangePos is the logical read position within it. The
        // worker thread is the only reader once decoding starts.
        HANDLE       rangeFile;
        unsigned int rangeBase;
        unsigned int rangeSize;
        unsigned int rangePos;
        CRITICAL_SECTION lock;
        volatile LONG    mode;  // PlayMode, host writes / worker reads
        volatile LONG    stop;
        volatile LONG    ended; // PlayOnce ran out (worker parks until mode flips)

        Slot     slots[kSlots];
        int      yW, yH, cW, cH;   // padded plane sizes (worker writes under lock, once)
        int      width, height;    // display crop
        int      display;          // slot index currently shown, -1
        unsigned frameId;
        double   clock;
        double   frameRate;        // for the last frame's display duration
        bool     clockInit;        // first frame snaps the clock to its pts
        bool     finished;         // Play-Once fully shown — GetFrame hides it
        bool     failed;           // open failed — stays dormant until the path changes

        // Audio (MP2 -> XAudio2). The worker owns the decode + voice creation;
        // the host reads the cursor once audioReady flips. audioBase (the first
        // audio frame's pts) is guarded by `lock`; once samples are playing the
        // host's clock becomes audioBase + SamplesPlayed / rate.
        AudioOut      audio;
        volatile LONG audioReady;   // voice created (host may read the cursor)
        bool          audible;      // from Want — a flip recreates the stream
        bool          hasAudio;     // audible AND the file has an audio stream
        bool          audioFailed;  // engine/device absent — dt clock instead
        int           sampleRate;
        double        audioBase;    // under lock
        bool          audioBaseSet; // worker-owned write, host reads under lock
        float         volume;
        bool          muted;

        // Worker-only loop bookkeeping.
        double loopBase;
        double duration;
        double lastTime;

        VideoStream()
            : plm(NULL), thread(NULL), wake(NULL),
              rangeFile(INVALID_HANDLE_VALUE), rangeBase(0), rangeSize(0), rangePos(0),
              mode(PlayLoop), stop(0), ended(0),
              yW(0), yH(0), cW(0), cH(0), width(0), height(0),
              display(-1), frameId(0), clock(0.0), frameRate(30.0),
              clockInit(false), finished(false), failed(false),
              audioReady(0), audible(true), hasAudio(false), audioFailed(false),
              sampleRate(0), audioBase(0.0), audioBaseSet(false),
              volume(1.0f), muted(false),
              loopBase(0.0), duration(0.0), lastTime(0.0)
        {
            for (int i = 0; i < kSlots; ++i) { slots[i].state = SlotFree; slots[i].pts = 0.0; }
        }
    };

    namespace
    {
        // Keep the voice fed: create it lazily on the worker (so a slow device
        // open never blocks the render thread), then decode MP2 frames until
        // the queue holds ~150ms. Runs even while the video side is parked at
        // EOF so the tail of the track drains.
        void PumpAudio(VideoStream* s)
        {
            if (!s->audio.IsOpen())
            {
                if (!s->audio.Open(s->sampleRate))
                {
                    s->audioFailed = true; // no engine/device — dt clock instead
                    plm_set_audio_enabled(s->plm, FALSE); // stop buffering packets
                    return;
                }
                s->audio.SetVolume(s->muted ? 0.0f : s->volume);
                InterlockedExchange(&s->audioReady, 1);
            }
            while (s->audio.QueuedBuffers() < AudioOut::kMaxQueue)
            {
                plm_samples_t* sm = plm_decode_audio(s->plm);
                if (!sm)
                    break; // dry — the video side owns EOF / loop rewind
                short pcm[AudioOut::kMaxFrames * AudioOut::kChannels];
                int n = (int)sm->count;
                if (n > AudioOut::kMaxFrames) n = AudioOut::kMaxFrames;
                const int total = n * AudioOut::kChannels;
                for (int i = 0; i < total; ++i)
                {
                    float v = sm->interleaved[i];
                    if (v >  1.0f) v =  1.0f;
                    if (v < -1.0f) v = -1.0f;
                    pcm[i] = (short)(v * 32767.0f);
                }
                if (!s->audioBaseSet)
                {
                    EnterCriticalSection(&s->lock);
                    s->audioBase    = s->loopBase + sm->time;
                    s->audioBaseSet = true;
                    LeaveCriticalSection(&s->lock);
                }
                s->audio.Submit(pcm, n);
            }
        }

        DWORD WINAPI WorkerProc(LPVOID param)
        {
            VideoStream* s = (VideoStream*)param;
            for (;;)
            {
                if (s->stop)
                    break;

                if (s->hasAudio && !s->audioFailed)
                    PumpAudio(s);

                // A finished Play-Once stream parks here; switching it to Loop
                // rewinds and resumes.
                if (s->ended)
                {
                    if (s->mode == PlayLoop)
                    {
                        s->loopBase += (s->duration > 0.0) ? s->duration : (s->lastTime + 1.0 / 30.0);
                        plm_rewind(s->plm);
                        InterlockedExchange(&s->ended, 0);
                        continue;
                    }
                    WaitForSingleObject(s->wake, 100);
                    continue;
                }

                // Claim a free ring slot; full ring = decoded far enough ahead.
                int slot = -1;
                EnterCriticalSection(&s->lock);
                for (int i = 0; i < VideoStream::kSlots; ++i)
                    if (s->slots[i].state == VideoStream::SlotFree) { slot = i; break; }
                LeaveCriticalSection(&s->lock);
                if (slot < 0)
                {
                    WaitForSingleObject(s->wake, 100);
                    continue;
                }

                plm_frame_t* fr = plm_decode_video(s->plm);
                if (!fr)
                {
                    if (s->mode == PlayLoop)
                    {
                        s->loopBase += (s->duration > 0.0) ? s->duration : (s->lastTime + 1.0 / 30.0);
                        plm_rewind(s->plm);
                    }
                    else
                        InterlockedExchange(&s->ended, 1);
                    continue;
                }
                s->lastTime = fr->time;

                // Copy the padded planes out; pl_mpeg reuses its frame buffers.
                VideoStream::Slot& sl = s->slots[slot];
                const size_t ySize = (size_t)fr->y.width  * fr->y.height;
                const size_t cSize = (size_t)fr->cb.width * fr->cb.height;
                if (sl.y.size() != ySize)  sl.y.resize(ySize);
                if (sl.cb.size() != cSize) sl.cb.resize(cSize);
                if (sl.cr.size() != cSize) sl.cr.resize(cSize);
                memcpy(&sl.y[0],  fr->y.data,  ySize);
                memcpy(&sl.cb[0], fr->cb.data, cSize);
                memcpy(&sl.cr[0], fr->cr.data, cSize);

                EnterCriticalSection(&s->lock);
                s->yW = fr->y.width;  s->yH = fr->y.height;
                s->cW = fr->cb.width; s->cH = fr->cb.height;
                sl.pts   = s->loopBase + fr->time;
                sl.state = VideoStream::SlotReady;
                LeaveCriticalSection(&s->lock);
            }
            return 0;
        }

        // ---- container-range data source (the console's in-pak video) ----
        // Mirrors pl_mpeg's own file callbacks (this TU holds the implementation,
        // so the buffer internals are visible), but reads through a Win32 handle
        // limited to the entry's byte window. plm sees a normal seekable "file"
        // of rangeSize bytes, so rewind/loop work unchanged.
        void RangeLoad(plm_buffer_t* self, void* user)
        {
            VideoStream* s = (VideoStream*)user;
            if (self->discard_read_bytes)
                plm_buffer_discard_read_bytes(self);
            const size_t avail = self->capacity - self->length;
            const unsigned int remain =
                (s->rangePos < s->rangeSize) ? (s->rangeSize - s->rangePos) : 0;
            const DWORD want = (DWORD)((avail < (size_t)remain) ? avail : (size_t)remain);
            DWORD got = 0;
            if (want > 0)
            {
                // lo is the unsigned low half when the high pointer is supplied,
                // so base + pos may use the full u32 range (pak TOC is u32).
                LONG hi = 0;
                SetFilePointer(s->rangeFile, (LONG)(s->rangeBase + s->rangePos), &hi, FILE_BEGIN);
                ReadFile(s->rangeFile, self->bytes + self->length, want, &got, NULL);
                s->rangePos  += got;
                self->length += got;
            }
            if (got == 0)
                self->has_ended = TRUE;
        }
        void RangeSeek(plm_buffer_t* self, size_t offset, void* user)
        {
            (void)self;
            ((VideoStream*)user)->rangePos = (unsigned int)offset;
        }
        size_t RangeTell(plm_buffer_t* self, void* user)
        {
            (void)self;
            return (size_t)((VideoStream*)user)->rangePos;
        }

        VideoStream* OpenStream(const Want& w)
        {
            VideoStream* s = new VideoStream();
            s->key  = w.key;
            s->path = w.path;
            s->mode = w.playMode;
            InitializeCriticalSection(&s->lock);
            s->wake = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset

            if (w.length > 0)
            {
                s->rangeBase = w.offset;
                s->rangeSize = w.length;
                s->rangeFile = CreateFileA(w.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                           NULL, OPEN_EXISTING, 0, NULL);
                if (s->rangeFile != INVALID_HANDLE_VALUE)
                {
                    plm_buffer_t* buf = plm_buffer_create_with_callbacks(
                        RangeLoad, RangeSeek, RangeTell, (size_t)w.length, s);
                    s->plm = plm_create_with_buffer(buf, TRUE); // plm owns + frees buf
                }
            }
            else
                s->plm = plm_create_with_filename(w.path.c_str());

            if (!s->plm || !plm_probe(s->plm, 1024 * 1024))
            {
                if (s->plm) { plm_destroy(s->plm); s->plm = NULL; }
                s->failed = true; // dormant record; retried only on a path change
                return s;
            }
            s->audible  = w.audible;
            s->volume   = w.volume;
            s->muted    = w.muted;
            s->hasAudio = w.audible && plm_get_num_audio_streams(s->plm) > 0;
            plm_set_audio_enabled(s->plm, s->hasAudio ? TRUE : FALSE);
            if (s->hasAudio)
                s->sampleRate = plm_get_samplerate(s->plm);
            s->width     = plm_get_width(s->plm);
            s->height    = plm_get_height(s->plm);
            s->duration  = plm_get_duration(s->plm);
            s->frameRate = plm_get_framerate(s->plm);

            s->thread = CreateThread(NULL, 0, WorkerProc, s, 0, NULL);
#ifdef _XBOX
            if (s->thread)
                XSetThreadProcessor(s->thread, 2); // core 1: render owns 0, streaming owns 4
#endif
            return s;
        }

        void CloseStream(VideoStream* s)
        {
            if (s->thread)
            {
                InterlockedExchange(&s->stop, 1);
                SetEvent(s->wake);
                WaitForSingleObject(s->thread, INFINITE);
                CloseHandle(s->thread);
            }
            if (s->wake)
                CloseHandle(s->wake);
            if (s->plm)
                plm_destroy(s->plm); // worker joined — safe on this thread now
            if (s->rangeFile != INVALID_HANDLE_VALUE)
                CloseHandle(s->rangeFile); // after plm_destroy (buffer may not load again)
            DeleteCriticalSection(&s->lock);
            delete s;
        }
    }

    VideoPlayer::VideoPlayer() {}
    VideoPlayer::~VideoPlayer() { Shutdown(); }

    void VideoPlayer::Shutdown()
    {
        for (size_t i = 0; i < m_streams.size(); ++i)
            CloseStream(m_streams[i]);
        m_streams.clear();
    }

    VideoStream* VideoPlayer::Find(const std::string& key) const
    {
        for (size_t i = 0; i < m_streams.size(); ++i)
            if (m_streams[i]->key == key)
                return m_streams[i];
        return NULL;
    }

    void VideoPlayer::Update(const Want* wants, int count, float dt)
    {
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f; // hitch clamp (mirrors the Vulkan manager)

        // Close streams whose key vanished, went Off, changed source, or
        // flipped audibility (Play/Stop restarts the video from the top —
        // the Vulkan editor's behavior, and it keeps the audio path simple).
        for (size_t i = 0; i < m_streams.size(); )
        {
            VideoStream* s = m_streams[i];
            const Want* w = NULL;
            for (int k = 0; k < count; ++k)
                if (wants[k].key == s->key) { w = &wants[k]; break; }
            if (!w || w->playMode == PlayOff || w->path != s->path ||
                w->audible != s->audible)
            {
                CloseStream(s);
                m_streams.erase(m_streams.begin() + i);
                continue;
            }
            s->mode = w->playMode; // PlayOnce <-> Loop switches apply live
            if (w->volume != s->volume || w->muted != s->muted)
            {
                s->volume = w->volume;
                s->muted  = w->muted;
                if (s->audioReady)
                    s->audio.SetVolume(s->muted ? 0.0f : s->volume);
            }
            ++i;
        }

        // Open newly wanted streams (capped).
        for (int k = 0; k < count; ++k)
        {
            if (wants[k].playMode == PlayOff || wants[k].path.empty())
                continue;
            if (Find(wants[k].key))
                continue;
            if ((int)m_streams.size() >= (int)kMaxStreams)
                break;
            m_streams.push_back(OpenStream(wants[k]));
        }

        // Advance clocks and pick the frame to show.
        for (size_t i = 0; i < m_streams.size(); ++i)
        {
            VideoStream* s = m_streams[i];
            if (s->failed)
                continue;

            EnterCriticalSection(&s->lock);
            if (s->clockInit)
            {
                // Once samples are audibly playing, the audio cursor is the
                // master clock (the Vulkan manager's rule); before that — and
                // for silent streams — the clock accumulates host dt.
                bool audioClock = false;
                if (s->audioReady && s->audioBaseSet && s->sampleRate > 0)
                {
                    // Once the decoder hit EOF and the voice drained, the
                    // cursor stops — hand back to dt so the last video frames
                    // (audio is usually a hair shorter) still play out.
                    const bool drained = s->ended && s->audio.QueuedBuffers() == 0;
                    const unsigned __int64 played = s->audio.SamplesPlayed();
                    if (!drained && played > 0)
                    {
                        s->clock = s->audioBase + (double)played / (double)s->sampleRate;
                        audioClock = true;
                    }
                }
                if (!audioClock)
                    s->clock += dt;
            }
            int    best = -1;
            double bestPts = -1.0;
            if (s->clockInit)
            {
                // Newest ready frame that is due; older due frames are dropped.
                for (int j = 0; j < VideoStream::kSlots; ++j)
                    if (s->slots[j].state == VideoStream::SlotReady &&
                        s->slots[j].pts <= s->clock + 1e-4 &&
                        s->slots[j].pts > bestPts)
                    { best = j; bestPts = s->slots[j].pts; }
            }
            else
            {
                // First frame: take the oldest ready and snap the clock to it.
                for (int j = 0; j < VideoStream::kSlots; ++j)
                    if (s->slots[j].state == VideoStream::SlotReady &&
                        (best < 0 || s->slots[j].pts < bestPts))
                    { best = j; bestPts = s->slots[j].pts; }
            }
            if (best >= 0)
            {
                for (int j = 0; j < VideoStream::kSlots; ++j)
                    if (j != best && s->slots[j].state == VideoStream::SlotReady &&
                        s->slots[j].pts < bestPts)
                        s->slots[j].state = VideoStream::SlotFree; // late — drop
                if (s->display >= 0)
                    s->slots[s->display].state = VideoStream::SlotFree;
                s->slots[best].state = VideoStream::SlotShown;
                s->display = best;
                ++s->frameId;
                if (!s->clockInit)
                {
                    s->clock = bestPts;
                    s->clockInit = true;
                }
            }

            // A Play-Once ends ON SCREEN only after its final frame has shown
            // for a full frame period: decoder at EOF, nothing left in the
            // ring, and the clock past the shown frame's slot. Then GetFrame
            // reports nothing and the overlay disappears.
            if (!s->finished && s->ended && s->mode == PlayOnce &&
                s->clockInit && s->display >= 0)
            {
                bool anyReady = false;
                for (int j = 0; j < VideoStream::kSlots; ++j)
                    if (s->slots[j].state == VideoStream::SlotReady)
                        anyReady = true;
                const double period = (s->frameRate > 1.0) ? 1.0 / s->frameRate : 1.0 / 30.0;
                if (!anyReady && s->clock >= s->slots[s->display].pts + period)
                    s->finished = true;
            }
            LeaveCriticalSection(&s->lock);
            SetEvent(s->wake); // freed slots / mode change — wake the decoder
        }
    }

    bool VideoPlayer::IsPlaying(const std::string& key) const
    {
        // "Playing" tracks visibility: a Play-Once counts as playing until its
        // last frame has fully shown (finished), at which point it also hides.
        VideoStream* s = Find(key);
        return s && !s->failed && !(s->mode == PlayOnce && s->finished);
    }

    bool VideoPlayer::HasStream(const std::string& key) const
    {
        return Find(key) != NULL;
    }

    bool VideoPlayer::GetFrame(const std::string& key, Frame& out) const
    {
        VideoStream* s = Find(key);
        if (!s || s->failed || s->display < 0 || s->finished)
            return false; // finished Play-Once: the overlay hides itself
        const VideoStream::Slot& sl = s->slots[s->display];
        out.y       = &sl.y[0];
        out.cb      = &sl.cb[0];
        out.cr      = &sl.cr[0];
        out.yW      = s->yW;
        out.yH      = s->yH;
        out.cW      = s->cW;
        out.cH      = s->cH;
        out.width   = s->width;
        out.height  = s->height;
        out.frameId = s->frameId;
        return true;
    }
}
