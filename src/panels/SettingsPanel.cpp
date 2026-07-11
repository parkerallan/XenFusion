#include "panels/SettingsPanel.h"

#include "state/EngineState.h"

#include "imgui.h"

#include <cstdio>
#include <string>

void SettingsPanel::Render(EngineState& state)
{
    if (!state.show_settings_panel)
        return;

    if (!ImGui::Begin("Settings", &state.show_settings_panel))
    {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Toolchain");

    // A read-only path field (can't be typed into) + a Browse button that asks
    // EngineApplication to open the Windows folder picker for that entry.
    auto path_row = [&](const char* label, const std::string& value, int pick_id)
    {
        ImGui::PushID(pick_id);
        ImGui::TextUnformatted(label);

        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", value.empty() ? "(not set)" : value.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::SetNextItemWidth(-90.0f); // leave room for the button
        ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
            state.toolchain_pick = pick_id;
        ImGui::PopID();
    };

    path_row("Xbox 360 SDK (XDK)", state.toolchain_xdk,      1);
    path_row("Xenia emulator",     state.toolchain_emulator, 2);
    path_row("Visual Studio 2010", state.toolchain_vs2010,   3);

    ImGui::SeparatorText("Log filters");
    ImGui::Checkbox("Info",    &state.log_show_info);
    ImGui::SameLine(); ImGui::Checkbox("Warning", &state.log_show_warning);
    ImGui::Checkbox("Error",   &state.log_show_error);
    ImGui::SameLine(); ImGui::Checkbox("Build",   &state.log_show_build);

    ImGui::End();
}
