#include "panels/SettingsPanel.h"

#include "app/Settings.h"
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

    path_row("Xbox 360 SDK (XDK) 2.0.21256.3", state.toolchain_xdk,      1);
    path_row("Xenia emulator",     state.toolchain_emulator, 2);

    ImGui::SeparatorText("Build Configuration");

    // Base folder for all console build artifacts (.xex, deploy\, .iso, obj).
    // Keeps builds out of the engine's source tree. Empty = the runtime\ folder.
    path_row("Build output folder", state.build_output_dir, 3);
    if (state.build_output_dir.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted("  (not set - defaults to the engine's runtime\\ folder)");
        ImGui::PopStyleColor();
    }

    // Release / Debug for the .xex build. Persisted immediately on change.
    ImGui::TextUnformatted("Configuration");
    ImGui::SetNextItemWidth(160.0f);
    const bool isDebug = (state.build_config == "Debug");
    if (ImGui::BeginCombo("##buildconfig", isDebug ? "Debug" : "Release"))
    {
        if (ImGui::Selectable("Release", !isDebug)) { state.build_config = "Release"; settings::Save(state); }
        if (ImGui::Selectable("Debug",    isDebug)) { state.build_config = "Debug";   settings::Save(state); }
        ImGui::EndCombo();
    }

    // Disc image filename (editable). Empty = "<project>.iso". Saved when the
    // field loses focus after an edit.
    ImGui::TextUnformatted("ISO name");
    char isobuf[256];
    std::snprintf(isobuf, sizeof(isobuf), "%s", state.build_iso_name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##isoname", "<project>.iso", isobuf, sizeof(isobuf)))
        state.build_iso_name = isobuf;
    if (ImGui::IsItemDeactivatedAfterEdit())
        settings::Save(state);

    ImGui::SeparatorText("Log filters");
    ImGui::Checkbox("Info",    &state.log_show_info);
    ImGui::SameLine(); ImGui::Checkbox("Warning", &state.log_show_warning);
    ImGui::Checkbox("Error",   &state.log_show_error);
    ImGui::SameLine(); ImGui::Checkbox("Build",   &state.log_show_build);
    ImGui::SameLine(); ImGui::Checkbox("Log",     &state.log_show_script);

    ImGui::End();
}
