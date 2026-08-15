#pragma once

// Records a facial performance: incoming tracking weights plus a mic take,
// written out as a .faceclip and its .mp2.
//
// Weights arrive whenever a packet does — UDP over WiFi jitters and drops — so
// samples are timestamped as they land and resampled onto a uniform grid at
// stop, which is what the cooked format wants anyway.
//
// Audio is captured by ffmpeg from a dshow device, the same dependency the
// video import already relies on. ffmpeg takes an unpredictable moment to open
// the device, so t=0 for the weights is taken once capture is confirmed running
// and the residual is carried on the clip as audio_offset_ms.

#include "anim/FaceClipAsset.h"
#include "anim/FaceShapes.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace facerec
{
    // Mic devices ffmpeg can see. Empty when ffmpeg is missing from PATH.
    std::vector<std::string> AudioDevices(std::string& error);

    // The parse, split out so it can be checked against real ffmpeg output.
    // Builds differ: some print "DirectShow audio devices" section headers,
    // newer ones tag each line "(audio)" instead.
    std::vector<std::string> ParseAudioDevices(const std::string& ffmpeg_output);

    class Recorder
    {
    public:
        // Both out-of-line: AudioJob is only complete in the .cpp, and an
        // inline constructor would have to instantiate its deleter here.
        Recorder();
        ~Recorder();

        // `audio_device` empty records no audio. `clip_path` and `audio_path`
        // are absolute; the clip stores `audio_relative` for the project.
        bool Start(const std::filesystem::path& clip_path,
                   const std::filesystem::path& audio_path,
                   const std::string& audio_relative,
                   const std::string& audio_device,
                   float fps, std::string& error);

        // Call every frame with the latest weights while recording; samples are
        // only kept once the clock has started.
        void Sample(double now, const float* weights, unsigned int count);

        // Writes the clip (and finishes the audio). False leaves nothing behind.
        bool Stop(double now, std::string& error);
        void Cancel();

        bool Recording() const { return m_recording; }
        // Seconds captured so far, or 0 while still waiting on the mic.
        float Elapsed(double now) const;
        unsigned int FrameCount() const { return (unsigned int)m_samples.size(); }
        bool WaitingForAudio() const { return m_recording && m_audioPending; }

    private:
        struct Captured
        {
            double time;
            unsigned char weights[face::kShapeCount];
        };
        struct AudioJob;

        void StopAudio();

        std::vector<Captured> m_samples;
        std::unique_ptr<AudioJob> m_audio;
        std::filesystem::path m_clipPath;
        std::filesystem::path m_audioPath;
        std::string m_audioRelative;
        float  m_fps = 30.0f;
        double m_startTime = 0.0;
        bool   m_recording = false;
        bool   m_audioPending = false;
    };
}
