#ifndef ANIM_FACESHAPES_H
#define ANIM_FACESHAPES_H

// The ARKit-52 blendshape vocabulary, shared by the engine and the 360 runtime
// (header-only C++03, the CameraResolve.h pattern). Order is canonical ARKit
// order and MUST NOT change: it is the wire format for cooked poses and morph
// targets, which carry an index rather than a name.

namespace face
{
    const int kShapeCount = 52;
    const int kShapeNone = -1;   // a morph target matching no ARKit shape

    inline const char* const* ShapeNames()
    {
        static const char* const kNames[kShapeCount] =
        {
            "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight",
            "cheekPuff", "cheekSquintLeft", "cheekSquintRight",
            "eyeBlinkLeft", "eyeBlinkRight", "eyeLookDownLeft", "eyeLookDownRight",
            "eyeLookInLeft", "eyeLookInRight", "eyeLookOutLeft", "eyeLookOutRight",
            "eyeLookUpLeft", "eyeLookUpRight", "eyeSquintLeft", "eyeSquintRight",
            "eyeWideLeft", "eyeWideRight",
            "jawForward", "jawLeft", "jawOpen", "jawRight",
            "mouthClose", "mouthDimpleLeft", "mouthDimpleRight", "mouthFrownLeft", "mouthFrownRight",
            "mouthFunnel", "mouthLeft", "mouthLowerDownLeft", "mouthLowerDownRight",
            "mouthPressLeft", "mouthPressRight", "mouthPucker", "mouthRight",
            "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper",
            "mouthSmileLeft", "mouthSmileRight", "mouthStretchLeft", "mouthStretchRight",
            "mouthUpperUpLeft", "mouthUpperUpRight",
            "noseSneerLeft", "noseSneerRight",
            "tongueOut"
        };
        return kNames;
    }

    inline const char* ShapeName(int index)
    {
        return (index >= 0 && index < kShapeCount) ? ShapeNames()[index] : "";
    }

    // Lowercase, dropping everything that is not a letter or digit: exporters
    // disagree about case and separators for the same shape ("jawOpen",
    // "Jaw.Open", "jaw_open") and all of them mean the ARKit target.
    inline void FoldShapeName(const char* name, char* out, int outBytes)
    {
        int written = 0;
        if (outBytes <= 0) return;
        for (const char* c = name; name && *c && written + 1 < outBytes; ++c)
        {
            char ch = *c;
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
            if (keep) out[written++] = ch;
        }
        out[written] = '\0';
    }

    // ARKit index for an authored target name, or kShapeNone. A folded name that
    // ENDS with a shape's folded name counts, so qualified spellings
    // ("CC_Base_Body.jawOpen") match; no ARKit name is a suffix of another.
    inline int ShapeIndex(const char* name)
    {
        char folded[128];
        FoldShapeName(name, folded, (int)sizeof(folded));
        if (folded[0] == '\0') return kShapeNone;

        int foldedLen = 0;
        while (folded[foldedLen] != '\0') ++foldedLen;

        for (int i = 0; i < kShapeCount; ++i)
        {
            char candidate[64];
            FoldShapeName(ShapeNames()[i], candidate, (int)sizeof(candidate));
            int candidateLen = 0;
            while (candidate[candidateLen] != '\0') ++candidateLen;
            if (candidateLen > foldedLen) continue;

            const char* tail = folded + (foldedLen - candidateLen);
            int c = 0;
            while (c < candidateLen && tail[c] == candidate[c]) ++c;
            if (c == candidateLen) return i;
        }
        return kShapeNone;
    }
}

#endif // ANIM_FACESHAPES_H
