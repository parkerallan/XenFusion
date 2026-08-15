// The face layer's contract: what wins when an expression, procedural blink and
// gaze all want the same shape. Same code on both targets, so getting it wrong
// here is wrong everywhere.

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
