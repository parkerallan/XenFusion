// A recorded performance round-trips: frames -> JSON -> cooked 'FCL1' -> the
// shared sampler both renderers use. The point is that the viewport and the
// console cannot disagree about a take, because both sample the same bytes with
// the same code.

#include "anim/FaceClip.h"
#include "anim/FaceClipAsset.h"
#include "anim/FaceShapes.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    int Fail(const std::string& message)
    {
        std::cerr << "FaceClipCodecTest: " << message << '\n';
        return 1;
    }

    bool Near(float a, float b, float tolerance = 1.0f / 200.0f)
    {
        return std::fabs(a - b) <= tolerance;
    }
}

int main()
{
    const int jaw   = face::ShapeIndex("jawOpen");
    const int blink = face::ShapeIndex("eyeBlinkLeft");
    const int brow  = face::ShapeIndex("browInnerUp");

    // Two seconds at 30 Hz: a jaw ramp, a blink spike, and a column that never
    // moves (which Compact should drop).
    FaceClipAsset asset;
    asset.name = "take01";
    asset.source_audio_path = "assets/audio/take01.mp2";
    asset.audio_offset_ms = -40;
    asset.fps = 30.0f;
    asset.shapes = {(uint8_t)jaw, (uint8_t)blink, (uint8_t)brow};
    const int frames = 61;
    for (int f = 0; f < frames; ++f)
    {
        const uint8_t ramp = (uint8_t)std::lround(255.0 * f / (frames - 1));
        const uint8_t spike = (f == 30) ? 255 : 0;
        asset.frames.push_back({ramp, spike, 0});
    }

    faceclip::Compact(asset);
    if (asset.shapes.size() != 2)
        return Fail("Compact kept a column that never moves");
    if (asset.frames[0].size() != 2)
        return Fail("Compact did not trim the frames to match");
    if (!Near(asset.Duration(), 2.0f))
        return Fail("duration should be 2s, got " + std::to_string(asset.Duration()));

    const fs::path dir = fs::temp_directory_path() / "xenfusion_face_clip_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / "take01.faceclip";

    std::string error;
    if (!faceclip::Save(path, asset, error)) return Fail("save failed: " + error);
    FaceClipAsset loaded;
    if (!faceclip::Load(path, loaded, error)) return Fail("load failed: " + error);
    if (loaded.frames.size() != asset.frames.size()) return Fail("frames lost in the JSON round trip");
    if (loaded.shapes != asset.shapes) return Fail("shape columns lost in the JSON round trip");
    if (loaded.source_audio_path != asset.source_audio_path)
        return Fail("source audio lost — playback could not start its own audio");
    if (loaded.audio_offset_ms != -40) return Fail("audio offset lost");

    std::vector<unsigned char> payload;
    if (!faceclip::CookBE(loaded, payload, error)) return Fail("cook failed: " + error);

    face::ClipView clip;
    if (!face::ParseClip(payload.data(), (unsigned)payload.size(), clip))
        return Fail("the cooked payload did not parse");
    if (clip.frameCount != 61 || clip.targetCount != 2)
        return Fail("cooked dimensions are wrong");
    if (clip.audioOffsetMs != -40) return Fail("cooked audio offset is wrong");
    if (std::string(clip.audioPath, clip.audioPathBytes) != asset.source_audio_path)
        return Fail("the cooked clip lost its audio path");
    if (!Near(clip.Duration(), 2.0f)) return Fail("cooked duration is wrong");

    // A clip writes only the shapes it drives, which is what lets it layer over
    // an expression pose.
    float weights[face::kShapeCount] = {};
    unsigned char touched[face::kShapeCount] = {};
    weights[brow] = 0.75f;
    face::SampleClip(clip, 1.0f, false, weights, face::kShapeCount, touched);
    if (!Near(weights[brow], 0.75f)) return Fail("the clip overwrote a shape it does not drive");
    if (touched[brow] != 0) return Fail("the clip claimed a shape it does not drive");
    if (touched[jaw] == 0) return Fail("the clip did not report the shape it drives");
    if (!Near(weights[jaw], 0.5f))
        return Fail("jaw at the halfway point is " + std::to_string(weights[jaw]));

    // Ends, clamping and looping.
    face::SampleClip(clip, 0.0f, false, weights, face::kShapeCount, nullptr);
    if (!Near(weights[jaw], 0.0f)) return Fail("the clip does not start at its first frame");
    face::SampleClip(clip, 99.0f, false, weights, face::kShapeCount, nullptr);
    if (!Near(weights[jaw], 1.0f)) return Fail("past its end a clip must hold the last frame");
    face::SampleClip(clip, 2.5f, true, weights, face::kShapeCount, nullptr);
    if (!Near(weights[jaw], 0.25f))
        return Fail("a looping clip did not wrap: got " + std::to_string(weights[jaw]));

    // The blink is one frame wide; sampling on it must actually catch it.
    face::SampleClip(clip, 1.0f, false, weights, face::kShapeCount, nullptr);
    if (!Near(weights[blink], 1.0f))
        return Fail("the one-frame blink was lost at its own timestamp");

    fs::remove_all(dir, ec);
    std::cout << "FaceClipCodecTest: ok (" << clip.frameCount << " frames, "
              << clip.targetCount << " shapes, " << payload.size() << " bytes)\n";
    return 0;
}
