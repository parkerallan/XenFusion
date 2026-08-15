#include "anim/FaceRecorder.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace facerec
{
    namespace
    {
        // ffmpeg with a pipe for stdin (so 'q' can end the take cleanly, which
        // is what flushes the container) and stdout/stderr captured.
        struct Process
        {
            PROCESS_INFORMATION info = {};
            HANDLE stdinWrite = nullptr;
            HANDLE stdoutRead = nullptr;

            bool Start(const std::string& command)
            {
                SECURITY_ATTRIBUTES sa = {};
                sa.nLength = sizeof(sa);
                sa.bInheritHandle = TRUE;

                HANDLE inRead = nullptr, outWrite = nullptr;
                if (!CreatePipe(&inRead, &stdinWrite, &sa, 0)) return false;
                SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
                if (!CreatePipe(&stdoutRead, &outWrite, &sa, 0))
                {
                    CloseHandle(inRead); CloseHandle(stdinWrite); stdinWrite = nullptr;
                    return false;
                }
                SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

                STARTUPINFOA si = {};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                si.hStdInput = inRead;
                si.hStdOutput = outWrite;
                si.hStdError = outWrite;

                std::vector<char> buffer(command.begin(), command.end());
                buffer.push_back('\0');
                const BOOL ok = CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, TRUE,
                                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &info);
                CloseHandle(inRead);
                CloseHandle(outWrite);
                if (!ok)
                {
                    CloseHandle(stdinWrite); stdinWrite = nullptr;
                    CloseHandle(stdoutRead); stdoutRead = nullptr;
                    return false;
                }
                return true;
            }

            void Quit()
            {
                if (stdinWrite)
                {
                    DWORD written = 0;
                    WriteFile(stdinWrite, "q\n", 2, &written, nullptr);
                    CloseHandle(stdinWrite);
                    stdinWrite = nullptr;
                }
                if (info.hProcess)
                {
                    // A clean 'q' flushes the file; kill it if it will not go.
                    if (WaitForSingleObject(info.hProcess, 4000) == WAIT_TIMEOUT)
                        TerminateProcess(info.hProcess, 1);
                    CloseHandle(info.hProcess);
                    CloseHandle(info.hThread);
                    info = PROCESS_INFORMATION();
                }
                if (stdoutRead) { CloseHandle(stdoutRead); stdoutRead = nullptr; }
            }
        };

        std::string RunCapture(const std::string& command)
        {
            SECURITY_ATTRIBUTES sa = {};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            HANDLE readEnd = nullptr, writeEnd = nullptr;
            if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return std::string();
            SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOA si = {};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = writeEnd;
            si.hStdError = writeEnd;
            PROCESS_INFORMATION pi = {};
            std::vector<char> buffer(command.begin(), command.end());
            buffer.push_back('\0');
            if (!CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(readEnd); CloseHandle(writeEnd);
                return std::string();
            }
            CloseHandle(writeEnd);

            std::string output;
            char chunk[512];
            DWORD read = 0;
            while (ReadFile(readEnd, chunk, sizeof(chunk), &read, nullptr) && read > 0)
                output.append(chunk, read);
            CloseHandle(readEnd);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return output;
        }
    }

    struct Recorder::AudioJob
    {
        Process process;
        std::atomic<bool> running{false};
    };

    std::vector<std::string> ParseAudioDevices(const std::string& output)
    {
        std::vector<std::string> devices;
        std::size_t pos = 0;
        bool inAudioSection = false;
        while (pos < output.size())
        {
            std::size_t end = output.find('\n', pos);
            std::string line = output.substr(pos, end == std::string::npos ? end : end - pos);
            pos = (end == std::string::npos) ? output.size() : end + 1;
            if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);

            if (line.find("DirectShow audio devices") != std::string::npos) { inAudioSection = true; continue; }
            if (line.find("DirectShow video devices") != std::string::npos) { inAudioSection = false; continue; }
            // Each device is repeated as an "@device_..." path on the next line.
            if (line.find("Alternative name") != std::string::npos) continue;

            const std::size_t open = line.find('"');
            if (open == std::string::npos) continue;
            const std::size_t close = line.rfind('"');
            if (close == std::string::npos || close <= open) continue;
            const std::string name = line.substr(open + 1, close - open - 1);
            if (name.empty() || name.rfind("@device_", 0) == 0) continue;

            // Newer builds tag the line; older ones rely on the section header.
            const bool tagged = line.find("(audio)") != std::string::npos;
            const bool otherTag = line.find("(video)") != std::string::npos ||
                                  line.find("(none)") != std::string::npos;
            if (!tagged && (otherTag || !inAudioSection)) continue;

            if (std::find(devices.begin(), devices.end(), name) == devices.end())
                devices.push_back(name);
        }
        return devices;
    }

    std::vector<std::string> AudioDevices(std::string& error)
    {
        error.clear();
        // ffmpeg lists devices on stderr and exits non-zero by design.
        const std::string output = RunCapture("ffmpeg -hide_banner -list_devices true -f dshow -i dummy");
        if (output.empty())
        {
            error = "ffmpeg not found on PATH";
            return {};
        }
        std::vector<std::string> devices = ParseAudioDevices(output);
        if (devices.empty())
            error = "ffmpeg reported no audio capture devices";
        return devices;
    }

    Recorder::Recorder() = default;
    Recorder::~Recorder() { Cancel(); }

    bool Recorder::Start(const std::filesystem::path& clip_path,
                         const std::filesystem::path& audio_path,
                         const std::string& audio_relative,
                         const std::string& audio_device,
                         float fps, std::string& error)
    {
        error.clear();
        if (m_recording)
        {
            error = "already recording";
            return false;
        }

        m_samples.clear();
        m_clipPath = clip_path;
        m_audioPath = audio_path;
        m_audioRelative = audio_device.empty() ? std::string() : audio_relative;
        m_fps = fps > 0.0f ? fps : 30.0f;
        m_startTime = 0.0;
        m_audioPending = false;
        m_audio.reset();

        if (!audio_device.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(audio_path.parent_path(), ec);
            std::filesystem::remove(audio_path, ec);
            // Straight to the MP2 elementary stream both targets play, so there
            // is no transcode step afterwards. Mono, because a voice at a
            // character is a spatialized emitter; 48 kHz because that is what
            // the 360 mixes at natively.
            // -flush_packets is not optional: without it ffmpeg holds the whole
            // take in its output buffer and the file stays zero bytes until it
            // exits, so the "capture has started" gate below never trips.
            const std::string command =
                "ffmpeg -hide_banner -y -f dshow -i audio=\"" + audio_device +
                "\" -vn -ac 1 -ar 48000 -f mp2 -c:a mp2 -b:a 192k -flush_packets 1 \"" +
                audio_path.string() + "\"";
            m_audio.reset(new AudioJob());
            if (!m_audio->process.Start(command))
            {
                m_audio.reset();
                error = "could not start ffmpeg (is it on PATH?)";
                return false;
            }
            m_audioPending = true;
        }

        m_recording = true;
        return true;
    }

    void Recorder::Sample(double now, const float* weights, unsigned int count)
    {
        if (!m_recording || weights == nullptr || count != (unsigned int)face::kShapeCount)
            return;

        if (m_audioPending)
        {
            // The clock starts when the capture file appears with bytes in it:
            // before that ffmpeg is still opening the device and anything
            // recorded would sit ahead of the audio.
            std::error_code ec;
            const auto size = std::filesystem::file_size(m_audioPath, ec);
            if (ec || size == 0)
                return;
            m_audioPending = false;
            m_startTime = now;
        }
        else if (m_samples.empty())
        {
            m_startTime = now;
        }

        Captured sample;
        sample.time = now - m_startTime;
        for (int shape = 0; shape < face::kShapeCount; ++shape)
            sample.weights[shape] =
                (unsigned char)std::lround(std::clamp(weights[shape], 0.0f, 1.0f) * 255.0f);
        m_samples.push_back(sample);
    }

    float Recorder::Elapsed(double now) const
    {
        if (!m_recording || m_audioPending || m_samples.empty())
            return 0.0f;
        return (float)(now - m_startTime);
    }

    void Recorder::StopAudio()
    {
        if (m_audio)
        {
            m_audio->process.Quit();
            m_audio.reset();
        }
    }

    bool Recorder::Stop(double now, std::string& error)
    {
        error.clear();
        if (!m_recording)
        {
            error = "not recording";
            return false;
        }
        m_recording = false;
        StopAudio();

        if (m_samples.size() < 2)
        {
            error = "nothing was captured - was the phone streaming?";
            m_samples.clear();
            return false;
        }

        // Resample the jittery arrival times onto a uniform grid.
        const double duration = m_samples.back().time;
        const unsigned int frames =
            (unsigned int)std::lround(duration * (double)m_fps) + 1u;
        if (frames < 2)
        {
            error = "take is too short";
            m_samples.clear();
            return false;
        }

        FaceClipAsset asset;
        asset.name = m_clipPath.stem().string();
        asset.source_audio_path = m_audioRelative;
        asset.fps = m_fps;
        for (int shape = 0; shape < face::kShapeCount; ++shape)
            asset.shapes.push_back((uint8_t)shape);

        std::size_t cursor = 0;
        for (unsigned int frame = 0; frame < frames; ++frame)
        {
            const double t = (double)frame / (double)m_fps;
            while (cursor + 2 < m_samples.size() && m_samples[cursor + 1].time < t)
                ++cursor;
            const Captured& a = m_samples[cursor];
            const Captured& b = m_samples[std::min(cursor + 1, m_samples.size() - 1)];
            const double span = b.time - a.time;
            const double blend = span > 1e-6 ? std::clamp((t - a.time) / span, 0.0, 1.0) : 0.0;

            std::vector<uint8_t> row((std::size_t)face::kShapeCount);
            for (int shape = 0; shape < face::kShapeCount; ++shape)
            {
                const double value = a.weights[shape] + (b.weights[shape] - a.weights[shape]) * blend;
                row[(std::size_t)shape] = (unsigned char)std::lround(std::clamp(value, 0.0, 255.0));
            }
            asset.frames.push_back(std::move(row));
        }

        faceclip::Compact(asset);
        m_samples.clear();
        (void)now;

        if (!asset.Valid())
        {
            error = "the take never moved a single shape";
            return false;
        }
        return faceclip::Save(m_clipPath, asset, error);
    }

    void Recorder::Cancel()
    {
        m_recording = false;
        m_audioPending = false;
        m_samples.clear();
        StopAudio();
    }
}
