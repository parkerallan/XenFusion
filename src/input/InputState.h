#pragma once

#include <string.h>

// A normalized controller snapshot shared by both targets. Each target fills it
// from its own source (XInput on the 360; XInput + keyboard in the editor), and
// the ScriptHost answers input.button("A") / input.axis("LX") from it. Header-only,
// strict C++03 (compiled by the XDK too).

namespace input
{
    struct InputState
    {
        enum Btn { A = 0, B, X, Y, LB, RB, Start, Back, LS, RS,
                   DPadUp, DPadDown, DPadLeft, DPadRight, ButtonCount };
        enum Ax  { LX = 0, LY, RX, RY, LT, RT, AxisCount };

        bool  buttons[ButtonCount];
        float axes[AxisCount]; // sticks [-1,1], triggers [0,1]

        InputState() { Clear(); }
        void Clear()
        {
            for (int i = 0; i < ButtonCount; ++i) buttons[i] = false;
            for (int i = 0; i < AxisCount;   ++i) axes[i] = 0.0f;
        }

        static int ButtonIndex(const char* n)
        {
            if (!n) return -1;
            if (!strcmp(n, "A")) return A;   if (!strcmp(n, "B")) return B;
            if (!strcmp(n, "X")) return X;   if (!strcmp(n, "Y")) return Y;
            if (!strcmp(n, "LB")) return LB; if (!strcmp(n, "RB")) return RB;
            if (!strcmp(n, "Start")) return Start; if (!strcmp(n, "Back")) return Back;
            if (!strcmp(n, "LS")) return LS; if (!strcmp(n, "RS")) return RS;
            if (!strcmp(n, "DPadUp"))    return DPadUp;
            if (!strcmp(n, "DPadDown"))  return DPadDown;
            if (!strcmp(n, "DPadLeft"))  return DPadLeft;
            if (!strcmp(n, "DPadRight")) return DPadRight;
            return -1;
        }
        static int AxisIndex(const char* n)
        {
            if (!n) return -1;
            if (!strcmp(n, "LX")) return LX; if (!strcmp(n, "LY")) return LY;
            if (!strcmp(n, "RX")) return RX; if (!strcmp(n, "RY")) return RY;
            if (!strcmp(n, "LT")) return LT; if (!strcmp(n, "RT")) return RT;
            return -1;
        }

        bool  Button(const char* n) const { int i = ButtonIndex(n); return i >= 0 && buttons[i]; }
        float Axis(const char* n)   const { int i = AxisIndex(n);   return i >= 0 ? axes[i] : 0.0f; }
    };
}
