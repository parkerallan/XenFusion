#include "input/ControllerMapping.h"

#include "imgui.h"

#include <cstdlib>
#include <sstream>

namespace input
{
    const std::vector<MapControl>& MapControls()
    {
        typedef InputState S;
        static const std::vector<MapControl> ctrls = {
            // Buttons — input.button(name). name == the button string.
            { "A", "", S::A, -1, 0 }, { "B", "", S::B, -1, 0 },
            { "X", "", S::X, -1, 0 }, { "Y", "", S::Y, -1, 0 },
            { "LB", "", S::LB, -1, 0 }, { "RB", "", S::RB, -1, 0 },
            { "Start", "", S::Start, -1, 0 }, { "Back", "", S::Back, -1, 0 },
            { "LS", "", S::LS, -1, 0 }, { "RS", "", S::RS, -1, 0 },
            { "DPadUp", "", S::DPadUp, -1, 0 }, { "DPadDown", "", S::DPadDown, -1, 0 },
            { "DPadLeft", "", S::DPadLeft, -1, 0 }, { "DPadRight", "", S::DPadRight, -1, 0 },
            // Stick axes — input.axis(name). Two rows per axis: "-" and "+" side.
            { "LX", "-", -1, S::LX, -1 }, { "LX", "+", -1, S::LX, +1 },
            { "LY", "-", -1, S::LY, -1 }, { "LY", "+", -1, S::LY, +1 },
            { "RX", "-", -1, S::RX, -1 }, { "RX", "+", -1, S::RX, +1 },
            { "RY", "-", -1, S::RY, -1 }, { "RY", "+", -1, S::RY, +1 },
            // Triggers — input.axis(name), 0..1 (single direction).
            { "LT", "", -1, S::LT, +1 }, { "RT", "", -1, S::RT, +1 },
        };
        return ctrls;
    }

    std::string ControlKey(const MapControl& c)
    {
        return std::string(c.name) + c.dir; // "A", "LX-", "LX+", "LT"
    }

    std::string ScriptCall(const MapControl& c)
    {
        return (c.isButton() ? "input.button(\"" : "input.axis(\"") + std::string(c.name) + "\")";
    }

    namespace
    {
        int IndexOf(const std::string& key)
        {
            const std::vector<MapControl>& c = MapControls();
            for (size_t i = 0; i < c.size(); ++i)
                if (ControlKey(c[i]) == key) return (int)i;
            return -1;
        }
        float BindingValue(const PcBinding& b)
        {
            switch (b.kind)
            {
            case PcBinding::Key:         return ImGui::IsKeyDown((ImGuiKey)b.code) ? 1.0f : 0.0f;
            case PcBinding::MouseButton: return ImGui::IsMouseDown(b.code) ? 1.0f : 0.0f;
            case PcBinding::MouseMove:
            {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                float v = 0.0f;
                switch (b.code) { case 0: v = d.x; break; case 1: v = -d.x; break;
                                  case 2: v = d.y; break; case 3: v = -d.y; break; }
                v *= 0.06f; // sensitivity
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                return v;
            }
            default: return 0.0f;
            }
        }
    }

    void ControllerMapping::Clear()
    {
        bindings.assign(MapControls().size(), PcBinding());
    }

    std::string BindingLabel(const PcBinding& b)
    {
        switch (b.kind)
        {
        case PcBinding::Key:         return ImGui::GetKeyName((ImGuiKey)b.code);
        case PcBinding::MouseButton: return b.code == 0 ? "Mouse L" : b.code == 1 ? "Mouse R" : "Mouse M";
        case PcBinding::MouseMove:
            return b.code == 0 ? "Mouse +X" : b.code == 1 ? "Mouse -X"
                 : b.code == 2 ? "Mouse +Y" : "Mouse -Y";
        default: return "-";
        }
    }

    std::string Serialize(const ControllerMapping& m)
    {
        const std::vector<MapControl>& c = MapControls();
        std::ostringstream out;
        out << "# Editor test input mappings (ControlKey = kind:code). Kind: 1=key 2=mousebtn 3=mousemove\n";
        for (size_t i = 0; i < c.size() && i < m.bindings.size(); ++i)
            if (m.bindings[i].kind != PcBinding::None)
                out << ControlKey(c[i]) << '=' << m.bindings[i].kind << ':' << m.bindings[i].code << '\n';
        return out.str();
    }

    void Deserialize(const std::string& text, ControllerMapping& m)
    {
        m.Clear();
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#') continue;
            const std::string::size_type eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string::size_type cn = line.find(':', eq);
            if (cn == std::string::npos) continue;
            const int idx = IndexOf(line.substr(0, eq));
            if (idx < 0) continue;
            m.bindings[(size_t)idx].kind = (PcBinding::Kind)std::atoi(line.substr(eq + 1, cn - eq - 1).c_str());
            m.bindings[(size_t)idx].code = std::atoi(line.substr(cn + 1).c_str());
        }
    }

    void ApplyMapping(const ControllerMapping& m, InputState& io)
    {
        const std::vector<MapControl>& c = MapControls();
        float axisAccum[InputState::AxisCount] = {0};
        bool  axisTouched[InputState::AxisCount] = {false};
        for (size_t i = 0; i < c.size() && i < m.bindings.size(); ++i)
        {
            const float v = BindingValue(m.bindings[i]);
            if (v <= 0.0f) continue;
            if (c[i].button >= 0)
                io.buttons[c[i].button] = true;
            else if (c[i].axis >= 0)
            {
                axisAccum[c[i].axis] += c[i].sign * v;
                axisTouched[c[i].axis] = true;
            }
        }
        for (int a = 0; a < InputState::AxisCount; ++a)
            if (axisTouched[a])
            {
                float v = axisAccum[a];
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                io.axes[a] = v; // keyboard/mouse overrides the controller on this axis
            }
    }

    bool CaptureBinding(PcBinding& out)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { out = PcBinding(); return true; } // clear
        for (int mb = 0; mb < 3; ++mb)
            if (ImGui::IsMouseClicked(mb)) { out.kind = PcBinding::MouseButton; out.code = mb; return true; }
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        {
            const ImGuiKey key = (ImGuiKey)k;
            if (key == ImGuiKey_Escape) continue;
            if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue; // mouse handled above
            if (ImGui::IsKeyPressed(key, false)) { out.kind = PcBinding::Key; out.code = k; return true; }
        }
        return false;
    }
}
