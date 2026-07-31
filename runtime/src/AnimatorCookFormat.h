#pragma once

namespace animcook
{
    const unsigned int kMagic = 0x414E4331; // 'ANC1'
    const unsigned int kVersionV1 = 1;
    const unsigned int kVersion = 2;
    const unsigned int kHeaderBytesV1 = 40;
    const unsigned int kHeaderBytes = 56;
    const unsigned int kStateBytes = 20;
    const unsigned int kTransitionBytes = 36;
    const unsigned int kClipBytes = 16;
    const unsigned int kModifierBytes = 92;

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