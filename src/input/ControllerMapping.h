#pragma once

#include "input/InputState.h"

#include <string>
#include <vector>

// Editor-only controller mapping: for TESTING scripts in the Play preview without
// a gamepad, each Xbox control (left column of the Mapping panel) is bound to a PC
// keyboard key or mouse input (right column). Scripts still poll Xbox controls
// (input.button("A") / input.axis("LX")); the SceneRenderer builds the InputState
// from the controller AND, per this mapping, from the keyboard/mouse. The console
// runtime ignores all of this — it reads a real controller.
//
// Nothing is bound by default: the developer explicitly binds each control.

namespace input
{
    // A PC input bound to an Xbox control.
    struct PcBinding
    {
        enum Kind { None = 0, Key, MouseButton, MouseMove };
        int kind; // Kind
        int code; // Key: ImGuiKey · MouseButton: 0=L 1=R 2=M · MouseMove: 0=+X 1=-X 2=+Y 3=-Y
        PcBinding() : kind(None), code(0) {}
    };

    // One mappable Xbox control (a panel row).
    //   name : the EXACT string scripts use — input.button(name) for a button,
    //          input.axis(name) for a stick/trigger.
    //   dir  : "" for buttons/triggers; "-" / "+" for the two directions of a stick
    //          axis (a key is on/off, so each stick axis needs one key per side).
    //   button >= 0 -> sets InputState.buttons[button]
    //   axis   >= 0 -> adds `sign` to InputState.axes[axis]
    struct MapControl
    {
        const char* name;
        const char* dir;
        int button; // InputState::Btn or -1
        int axis;   // InputState::Ax  or -1
        int sign;   // +1 / -1 for axis directions

        bool isButton() const { return button >= 0; }
    };

    // The fixed control list (rows), in display + serialization order.
    const std::vector<MapControl>& MapControls();

    // Stable key for a control ("A", "LX-", "LX+", "LT") — display + serialization.
    std::string ControlKey(const MapControl& c);

    // The exact script call a control answers ("input.button(\"A\")").
    std::string ScriptCall(const MapControl& c);

    // Per-control PC bindings (parallel to MapControls()). Empty by default.
    struct ControllerMapping
    {
        std::vector<PcBinding> bindings;
        ControllerMapping() { Clear(); }
        void Clear(); // all bindings None
    };

    // Human-readable label for a binding ("W", "Mouse L", "Mouse +X", "-").
    std::string BindingLabel(const PcBinding& b);

    // Text (.ini-style) serialization keyed by ControlKey.
    std::string Serialize(const ControllerMapping& m);
    void        Deserialize(const std::string& text, ControllerMapping& m);

    // Overlay keyboard/mouse (per the mapping) onto an already-controller-polled
    // InputState. Uses the live ImGui key/mouse state, so call during the ImGui
    // frame. Keyboard/mouse input overrides the controller on any axis it drives.
    void ApplyMapping(const ControllerMapping& m, InputState& io);

    // If a PC key / mouse button was just pressed (for rebind capture), fill `out`
    // and return true. Esc returns kind=None (a request to clear). Uses ImGui.
    bool CaptureBinding(PcBinding& out);
}
