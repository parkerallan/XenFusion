// The face layer's contract: what wins when an expression, procedural blink and
// gaze all want the same shape. Same code on both targets, so getting it wrong
// here is wrong everywhere.

#include "anim/FaceClip.h"
#include "anim/FaceRuntime.h"
#include "anim/FaceShapes.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int Fail(const std::string& message)
    {
        std::cerr << "FaceLayerTest: " << message << '\n';
        return 1;
    }

    bool Near(float a, float b, float tolerance = 0.01f)
    {
        return std::fabs(a - b) <= tolerance;
    }

    void PushU32BE(std::vector<unsigned char>& out, unsigned int v)
    {
        out.push_back((unsigned char)(v >> 24)); out.push_back((unsigned char)(v >> 16));
        out.push_back((unsigned char)(v >> 8));  out.push_back((unsigned char)v);
    }

    // A one-second clip at 10 Hz holding every listed shape wide open.
    std::vector<unsigned char> BuildClip(const std::vector<int>& shapes)
    {
        const unsigned int frames = 11;
        std::vector<unsigned char> p;
        PushU32BE(p, face::kClipMagic);
        PushU32BE(p, face::kClipVersion);
        PushU32BE(p, frames);
        PushU32BE(p, (unsigned int)shapes.size());
        PushU32BE(p, 10000u);   // 10 Hz
        PushU32BE(p, 0u);       // audio offset
        PushU32BE(p, 0u);       // no audio path
        for (int shape : shapes) p.push_back((unsigned char)shape);
        while ((p.size() - face::kClipHeaderBytes) % 4 != 0) p.push_back(0);
        for (unsigned int f = 0; f < frames; ++f)
            for (std::size_t i = 0; i < shapes.size(); ++i) p.push_back(255);
        return p;
    }

}

int main()
{
    const int jaw   = face::ShapeIndex("jawOpen");
    const int smile = face::ShapeIndex("mouthSmileLeft");
    const int brow  = face::ShapeIndex("browInnerUp");
    const int blinkL = face::ShapeIndex("eyeBlinkLeft");
    const int lookOutLeft = face::ShapeIndex("eyeLookOutLeft");
    const int lookInRight = face::ShapeIndex("eyeLookInRight");

    // An untouched face is at rest and reports itself inactive, so a character
    // standing silent costs no deformation at all.
    {
        face::Layer layer;
        layer.SetBlinkEnabled(false);
        layer.Update(1.0f / 60.0f);
        if (layer.Active())
            return Fail("a face nobody has touched reported itself active");
        for (int shape = 0; shape < face::kShapeCount; ++shape)
            if (!Near(layer.Weights()[shape], 0.0f))
                return Fail("a face at rest is not at zero");
    }

    // An expression pose with speed 0 is a cut; with a speed it eases.
    face::Pose happy;
    happy.nameHash = 1;
    happy.targets.push_back({(unsigned char)smile, 255});
    happy.targets.push_back({(unsigned char)brow, 128});
    {
        face::Layer layer;
        layer.SetBlinkEnabled(false);
        layer.SetPose(&happy, 1.0f, 0.0f);
        layer.Update(1.0f / 60.0f);
        if (!Near(layer.Weights()[smile], 1.0f) || !Near(layer.Weights()[brow], 0.502f))
            return Fail("a snapped pose did not apply at full strength");

        face::Layer eased;
        eased.SetBlinkEnabled(false);
        eased.SetPose(&happy, 1.0f, 4.0f);
        eased.Update(1.0f / 60.0f);
        if (eased.Weights()[smile] >= 1.0f || eased.Weights()[smile] <= 0.0f)
            return Fail("an eased pose should be part-way there after one frame");
        for (int frame = 0; frame < 240; ++frame)
            eased.Update(1.0f / 60.0f);
        if (!Near(eased.Weights()[smile], 1.0f, 0.02f))
            return Fail("an eased pose never arrived");
    }

    // Half weight holds the pose half open.
    {
        face::Layer layer;
        layer.SetBlinkEnabled(false);
        layer.SetPose(&happy, 0.5f, 0.0f);
        layer.Update(1.0f / 60.0f);
        if (!Near(layer.Weights()[smile], 0.5f))
            return Fail("a half-weight pose did not hold half open");
    }

    // A clip overrides the shapes it drives and leaves the expression alone --
    // an angry character can still deliver a line.
    const std::vector<unsigned char> mouthClip = BuildClip({jaw});
    face::ClipView view;
    if (!face::ParseClip(mouthClip.data(), (unsigned)mouthClip.size(), view))
        return Fail("the hand-built clip did not parse");
    {
        face::Layer layer;
        layer.SetBlinkEnabled(false);
        layer.SetPose(&happy, 1.0f, 0.0f);
        layer.PlayClip(view, false);
        layer.Update(1.0f / 60.0f, 0.5f);
        if (!Near(layer.Weights()[jaw], 1.0f))
            return Fail("the clip did not drive its own shape");
        if (!Near(layer.Weights()[smile], 1.0f))
            return Fail("the clip erased the expression it should layer over");
    }

    // The audio clock drives the clip, and a non-looping one stops itself.
    {
        face::Layer layer;
        layer.SetBlinkEnabled(false);
        layer.PlayClip(view, false);
        layer.Update(1.0f / 60.0f, 0.2f);
        if (!layer.ClipPlaying())
            return Fail("the clip stopped early while its audio was still going");
        layer.Update(1.0f / 60.0f, 5.0f);
        if (layer.ClipPlaying())
            return Fail("a non-looping clip did not stop at its end");
    }

    // Blink must never stack on a lid the clip already drives, or a captured
    // performance blinks twice.
    const std::vector<unsigned char> blinkClip = BuildClip({blinkL});
    face::ClipView blinkView;
    if (!face::ParseClip(blinkClip.data(), (unsigned)blinkClip.size(), blinkView))
        return Fail("the blink clip did not parse");
    {
        face::Layer layer;
        layer.SetSeed(12345);
        layer.PlayClip(blinkView, true);
        for (int frame = 0; frame < 60 * 15; ++frame)
        {
            layer.Update(1.0f / 60.0f, -1.0f);
            if (layer.Weights()[blinkL] > 1.0f + 0.001f)
                return Fail("procedural blink stacked on a clip-driven lid");
        }
    }

    // Procedural blink drives the lids.
    {
        face::Layer layer;
        layer.SetSeed(12345);
        layer.SetPose(&happy, 1.0f, 0.0f); // marks the face active
        bool blinked = false;
        for (int frame = 0; frame < 60 * 15 && !blinked; ++frame)
        {
            layer.Update(1.0f / 60.0f);
            if (layer.Weights()[blinkL] > 0.5f) blinked = true;
        }
        if (!blinked)
            return Fail("no blink in 15 seconds -- the character reads as dead");
    }

    // Gaze drives the eyeLook* shapes: looking left takes the left eye OUT and
    // the right eye IN, the way ARKit names them relative to the nose.
    {
        face::Layer layer;
        layer.SetBlinkEnabled(false);
        layer.SetGaze(1.0f, 0.0f);
        for (int frame = 0; frame < 60; ++frame)
            layer.Update(1.0f / 60.0f);
        if (!Near(layer.Weights()[lookOutLeft], 1.0f, 0.05f))
            return Fail("looking left did not take the left eye outward");
        if (!Near(layer.Weights()[lookInRight], 1.0f, 0.05f))
            return Fail("looking left did not take the right eye inward");

        // Clearing eases back to centre rather than snapping.
        layer.ClearGaze();
        layer.Update(1.0f / 60.0f);
        if (layer.Weights()[lookOutLeft] <= 0.0f)
            return Fail("clearing the gaze snapped the eyes instead of easing them");
        for (int frame = 0; frame < 120; ++frame)
            layer.Update(1.0f / 60.0f);
        if (!Near(layer.Weights()[lookOutLeft], 0.0f, 0.02f))
            return Fail("the eyes never came back to centre");
    }

    std::cout << "FaceLayerTest: ok\n";
    return 0;
}
