#include "panels/MapperPanel.h"

#include "input/ControllerMapping.h"
#include "state/EngineState.h"

#include "imgui.h"

#include <fstream>
#include <sstream>

void MapperPanel::Load(EngineState& state)
{
    input::ControllerMapping& m = state.controller_mapping;
    m.Clear(); // nothing bound unless a saved file overlays it
    std::ifstream in(state.project_root / "input_mappings.ini", std::ios::binary);
    if (in)
    {
        std::ostringstream ss; ss << in.rdbuf();
        input::Deserialize(ss.str(), m);
    }
}

void MapperPanel::Save(EngineState& state) const
{
    std::ofstream out(state.project_root / "input_mappings.ini", std::ios::binary | std::ios::trunc);
    out << input::Serialize(state.controller_mapping);
    state.AddLog("Saved input mappings");
}

void MapperPanel::Render(EngineState& state)
{
    // Load the project's saved mappings when the open project changes — done
    // BEFORE the visibility check so mapped PC input works whether or not the
    // panel is open. (Render is called every frame regardless of visibility.)
    if (state.HasProject() && state.project_root != loaded_project_)
    {
        loaded_project_ = state.project_root;
        Load(state);
    }

    if (!state.show_mapper_panel)
        return;

    if (!ImGui::Begin("Mapping", &state.show_mapper_panel))
    {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "Bind PC keys/mouse to Xbox controls so you can drive scripts in the Play "
        "preview without a gamepad");

    if (ImGui::Button("Clear all")) { state.controller_mapping.Clear(); listening_ = -1; }
    ImGui::SameLine();
    if (ImGui::Button("Save"))      { if (state.HasProject()) Save(state); }
    if (!state.HasProject()) { ImGui::SameLine(); ImGui::TextDisabled("(open a project to save)"); }

    ImGui::Separator();

    const std::vector<input::MapControl>& ctrls = input::MapControls();
    input::ControllerMapping& m = state.controller_mapping;
    if ((int)m.bindings.size() < (int)ctrls.size())
        m.bindings.resize(ctrls.size());

    if (ImGui::BeginTable("##mapping", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("Xbox control", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Script call",  ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("PC key / mouse");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)ctrls.size(); ++i)
        {
            const input::MapControl& c = ctrls[i];
            ImGui::TableNextRow();

            // Col 1: control name (+ direction for stick axes).
            ImGui::TableSetColumnIndex(0);
            if (c.dir[0]) ImGui::Text("%s %s", c.name, c.dir);   // "LX -", "LX +"
            else          ImGui::TextUnformatted(c.name);        // "A", "LT"

            // Col 2: the exact script call this row answers.
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", input::ScriptCall(c).c_str());

            // Col 3: the PC binding (click to rebind).
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(i);
            const bool listening = (listening_ == i);
            const std::string lbl = listening ? "press a key..." : input::BindingLabel(m.bindings[i]);
            if (listening) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.38f, 0.10f, 1.0f));
            if (ImGui::Button(lbl.c_str(), ImVec2(-1.0f, 0.0f)))
            {
                listening_ = listening ? -1 : i;
                armed_ = false; // skip the click that started listening
            }
            if (listening) ImGui::PopStyleColor();

            if (ImGui::BeginPopupContextItem("##ctx"))
            {
                if (ImGui::MenuItem("Mouse Move +X")) { m.bindings[i].kind = input::PcBinding::MouseMove; m.bindings[i].code = 0; }
                if (ImGui::MenuItem("Mouse Move -X")) { m.bindings[i].kind = input::PcBinding::MouseMove; m.bindings[i].code = 1; }
                if (ImGui::MenuItem("Mouse Move +Y")) { m.bindings[i].kind = input::PcBinding::MouseMove; m.bindings[i].code = 2; }
                if (ImGui::MenuItem("Mouse Move -Y")) { m.bindings[i].kind = input::PcBinding::MouseMove; m.bindings[i].code = 3; }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear")) { m.bindings[i] = input::PcBinding(); }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Capture a press for the listening row (skipping the frame it started so the
    // initiating mouse click isn't captured).
    if (listening_ >= 0 && listening_ < (int)m.bindings.size())
    {
        if (!armed_)
            armed_ = true;
        else
        {
            input::PcBinding b;
            if (input::CaptureBinding(b))
            {
                m.bindings[listening_] = b;
                listening_ = -1;
                armed_ = false;
            }
        }
    }

    ImGui::End();
}
