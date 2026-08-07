#pragma once

#include "input/InputState.h"

// Fills an InputState from XInput. The XInput API (XInputGetState, XINPUT_STATE,
// XINPUT_GAMEPAD_*) is identical on the 360 and PC — only the header differs — so
// the mapping lives here once. The INCLUDER must make XInput available first:
//   editor:  #include <Xinput.h>
//   runtime: #include <xtl.h>   (the XDK's XInput)
// Deadzones are hard-coded (the named constants aren't on every SDK).

namespace input
{
    inline float NormStick(short v, short deadzone)
    {
        const float f = (float)v;
        const float range = 32767.0f - (float)deadzone;
        if (f >  deadzone) return (f - deadzone) / range;
        if (f < -deadzone) return (f + deadzone) / range;
        return 0.0f;
    }

    // Poll controller 0 into s (clears first). No-op fields stay zero if absent.
    // Rolls the previous frame first, so InputState::Pressed/Released work for
    // every caller without them having to remember to do it.
    inline void PollXInput(InputState& s)
    {
        s.BeginFrame();
        s.Clear();
        XINPUT_STATE st;
        memset(&st, 0, sizeof(st));
        if (XInputGetState(0, &st) != ERROR_SUCCESS)
            return;
        const XINPUT_GAMEPAD& g = st.Gamepad;
        const unsigned short b = g.wButtons;
        s.buttons[InputState::A]     = (b & XINPUT_GAMEPAD_A) != 0;
        s.buttons[InputState::B]     = (b & XINPUT_GAMEPAD_B) != 0;
        s.buttons[InputState::X]     = (b & XINPUT_GAMEPAD_X) != 0;
        s.buttons[InputState::Y]     = (b & XINPUT_GAMEPAD_Y) != 0;
        s.buttons[InputState::LB]    = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        s.buttons[InputState::RB]    = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        s.buttons[InputState::Start] = (b & XINPUT_GAMEPAD_START) != 0;
        s.buttons[InputState::Back]  = (b & XINPUT_GAMEPAD_BACK) != 0;
        s.buttons[InputState::LS]    = (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
        s.buttons[InputState::RS]    = (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
        s.buttons[InputState::DPadUp]    = (b & XINPUT_GAMEPAD_DPAD_UP) != 0;
        s.buttons[InputState::DPadDown]  = (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
        s.buttons[InputState::DPadLeft]  = (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
        s.buttons[InputState::DPadRight] = (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
        s.axes[InputState::LX] = NormStick(g.sThumbLX, 7849);
        s.axes[InputState::LY] = NormStick(g.sThumbLY, 7849);
        s.axes[InputState::RX] = NormStick(g.sThumbRX, 8689);
        s.axes[InputState::RY] = NormStick(g.sThumbRY, 8689);
        s.axes[InputState::LT] = g.bLeftTrigger  / 255.0f;
        s.axes[InputState::RT] = g.bRightTrigger / 255.0f;
    }
}
