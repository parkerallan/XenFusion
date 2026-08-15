#pragma once

namespace animcook
{
    const unsigned int kMagic = 0x414E4331; // 'ANC1'
    const unsigned int kVersionV1 = 1;
    const unsigned int kVersionV2 = 2;
    const unsigned int kVersion = 3;        // adds the face block
    const unsigned int kHeaderBytesV1 = 40;
    const unsigned int kHeaderBytesV2 = 56;
    const unsigned int kHeaderBytes = 80;
    const unsigned int kStateBytes = 20;
    const unsigned int kTransitionBytes = 36;
    const unsigned int kClipBytes = 16;
    const unsigned int kModifierBytes = 92;

    // Face block (v3). Expression poses are sparse -- a pose lists only the
    // ARKit shapes it drives -- so the targets live in one flat array and each
    // pose points into it.
    const unsigned int kPoseBytes       = 12; // nameHash | firstTarget | targetCount
    const unsigned int kPoseTargetBytes = 4;  // u8 shape | u8 weight | u16 pad
    // A clip record carries the stem a script names it by and the project path
    // the pak is keyed under, through the shared string table.
    const unsigned int kFaceClipBytes   = 12; // stemHash | pathOffset | pathBytes
    const unsigned int kMaxPoses        = 256;
    const unsigned int kMaxFaceClips    = 512;

    enum ModifierType
    {
        ModifierPhysics = 0,
        ModifierCollision = 1
    };

    const unsigned int kModifierAffectsChildren = 0x1u;

    enum ConditionOp
    {
        CondAlways = 0,
        CondTruthy = 1,
        CondFalsy = 2,
        CondEqual = 3,
        CondNotEqual = 4,
        CondGreaterEqual = 5,
        CondLessEqual = 6,
        CondGreater = 7,
        CondLess = 8
    };

    const unsigned int kStateLoop = 0x1u;
    const unsigned int kTransitionHasExitTime = 0x1u;
    const unsigned int kConditionBoolLiteral = 0x2u;
}