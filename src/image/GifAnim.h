#pragma once

// Shared GIF playback clock, used by BOTH the renderer's SceneRenderer and the
// 360 runtime's SceneRuntime (and by the GUI core, which drives per-widget
// playback), so an animated Image attribute runs at the same rate and shows the
// same frame at the same time on PC and console. Ported from the Vulkan
// engine's Scene2DRenderer::GetOrLoadAnimatedImage.
//
// This header owns ONLY the clock. The two targets deliberately store the
// frames differently — the renderer keeps one D3D9 texture per frame, the
// console keeps one Xenos array texture whose slices are the frames — because
// PC D3D9 has no array textures. What must agree between them is which frame is
// on screen at time t, and that is exactly what Advance() decides.
//
// Header-only and strict C++03 (the runtime builds under the XDK's VS2010
// toolset): no default member initializers, no enum class, no nullptr. Same
// rules as camera/CameraResolve.h.

#include <string>
#include <vector>

namespace gifanim
{
    // The Xenos texture fetch constant addresses a stacked (array) texture with
    // a 6-bit Depth field holding ArraySize-1 (GPUTEXTURESIZE_STACK in the
    // XDK's d3d9gpu.h), so 64 slices is a hardware ceiling, not a policy. The
    // cooker refuses a longer GIF rather than silently dropping frames.
    const int kMaxFrames = 64;

    // Play mode values match the Image attribute's image_play_mode field and
    // the inspector combo ("Off\0Play Once\0Loop\0"), which in turn mirror the
    // Video attribute's video_play_mode.
    enum PlayMode { PlayOff = 0, PlayOnce = 1, PlayLoop = 2 };

    // A GIF that stb has decoded: canvas size, frame count, and the per-frame
    // delays it carried. stb composites GIF disposal itself, so every frame is
    // a full canvas of frameW x frameH.
    struct Info
    {
        int frameW, frameH, frameCount;
        std::vector<unsigned int> delaysMs;

        Info() : frameW(0), frameH(0), frameCount(0) {}
    };

    // Where one Image attribute (or one GUI widget) is in its GIF. Keyed per
    // attribute by the callers, not per file, so two objects showing the same
    // GIF animate independently.
    struct Playback
    {
        int   frame;
        float elapsed;   // seconds accumulated toward the current frame's delay
        int   lastMode;
        bool  finished;  // Play Once has latched on the last frame

        Playback() : frame(0), elapsed(0.0f), lastMode(-1), finished(false) {}
    };

    inline bool IsGifPath(const std::string& path)
    {
        // Needs a stem: a bare ".gif" is a dotfile whose extension is empty,
        // which is also how std::filesystem (and so the Vulkan engine's
        // HasAnyExtension) reads it.
        const size_t n = path.size();
        if (n < 5) return false;
        const char before = path[n - 5];
        if (before == '/' || before == '\\') return false;
        const char* p = path.c_str() + (n - 4);
        return p[0] == '.'
            && (p[1] == 'g' || p[1] == 'G')
            && (p[2] == 'i' || p[2] == 'I')
            && (p[3] == 'f' || p[3] == 'F');
    }

    // A GIF delay of 0 means "as fast as possible", which every decoder and
    // browser clamps; stb's own convention is 100 ms, so we match it rather
    // than spinning through the whole animation in one frame.
    inline float FrameDelaySeconds(const Info& info, int frame)
    {
        if (frame < 0 || frame >= (int)info.delaysMs.size())
            return 0.1f;
        const unsigned int ms = info.delaysMs[(size_t)frame];
        return ms > 0u ? (float)ms * 0.001f : 0.1f;
    }

    // Normalized W coordinate of a slice's centre, for tex3D() against a Xenos
    // array texture: (2f+1) / 2n. Matches the XDK's own ArrayTexture sample
    // (AtgSimpleShaders.cpp). Only the console uses this.
    inline float SliceCoord(int frame, int frameCount)
    {
        if (frameCount <= 0) return 0.0f;
        return (float)(2 * frame + 1) / (float)(2 * frameCount);
    }

    // Step playback by dt seconds. dt <= 0 never advances, which is what makes
    // a GIF sit frozen on frame 0 while the scene is being edited (the renderer
    // passes 0 when not in Play) without any special case here.
    inline void Advance(Playback& state, const Info& info, int playMode, float dt)
    {
        // A play-mode change restarts from the top, including un-latching a
        // finished Play Once — otherwise re-selecting Play Once would do
        // nothing visible.
        if (state.lastMode != playMode)
        {
            state.lastMode = playMode;
            state.elapsed  = 0.0f;
            state.frame    = 0;
            state.finished = false;
        }

        if (info.frameCount <= 1 || playMode == PlayOff || state.finished || dt <= 0.0f)
            return;

        if (state.frame < 0 || state.frame >= info.frameCount)
            state.frame = 0;

        // Clamp the step so a hitch (a long load, a breakpoint) skips at most a
        // quarter second of animation instead of racing through the whole clip.
        state.elapsed += (dt < 0.25f) ? dt : 0.25f;

        // Bounded catch-up: at most one full lap per call, so a pathological
        // dt can never spin here.
        for (int steps = 0; steps < info.frameCount; ++steps)
        {
            const float delay = FrameDelaySeconds(info, state.frame);
            if (state.elapsed < delay)
                break;
            state.elapsed -= delay;
            if (playMode == PlayOnce && state.frame >= info.frameCount - 1)
            {
                state.frame    = info.frameCount - 1;
                state.finished = true;
                state.elapsed  = 0.0f;
                break;
            }
            state.frame = (state.frame + 1) % info.frameCount;
        }
    }
}
