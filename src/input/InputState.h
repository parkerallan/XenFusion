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
        bool  prev[ButtonCount];  // last frame's buttons, for the press/release edges
        bool  primed;             // false until the first poll has seeded `prev`
        float axes[AxisCount];    // sticks [-1,1], triggers [0,1]

        InputState() { Reset(); }

        // Per-poll wipe of the CURRENT frame only. `prev` deliberately survives —
        // it is the previous frame, and clearing it would make every edge query
        // report nothing.
        void Clear()
        {
            for (int i = 0; i < ButtonCount; ++i) buttons[i] = false;
            for (int i = 0; i < AxisCount;   ++i) axes[i] = 0.0f;
        }

        // Full wipe including the edge history. For starting a fresh session, so
        // a button left held when the last one ended can't fire an edge here.
        void Reset()
        {
            Clear();
            for (int i = 0; i < ButtonCount; ++i) prev[i] = false;
            primed = false;
        }

        // Seed `prev` from the current frame so nothing reads as an edge. Called
        // once after the first poll of a session: a button ALREADY held when a
        // session starts has not been pressed during it, and must not look like it.
        void Prime()
        {
            for (int i = 0; i < ButtonCount; ++i) prev[i] = buttons[i];
            primed = true;
        }

        // Roll the current frame into the previous one. Call once per frame,
        // BEFORE refilling `buttons` (PollXInput does this for you).
        void BeginFrame()
        {
            for (int i = 0; i < ButtonCount; ++i) prev[i] = buttons[i];
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

        // Edges: true only on the frame the button went down / came up. Button()
        // stays true for as long as it is held, which is why anything that should
        // happen ONCE per press has to ask these instead.
        bool Pressed(const char* n) const
        { int i = ButtonIndex(n); return i >= 0 && buttons[i] && !prev[i]; }
        bool Released(const char* n) const
        { int i = ButtonIndex(n); return i >= 0 && !buttons[i] && prev[i]; }
    };
}
