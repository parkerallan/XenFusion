#include "panels/SettingsPanel.h"

#include "state/EngineState.h"

#include "imgui.h"

void SettingsPanel::Render(EngineState& state)
{
    if (!state.show_settings_panel)
        return;

    if (!ImGui::Begin("Settings", &state.show_settings_panel))
    {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Rendering");
    ImGui::TextUnformatted("Backend: Direct3D 9");
    ImGui::ColorEdit3("Viewport clear color", state.clear_color);

    ImGui::SeparatorText("Editor");
    ImGui::Checkbox("Auto-scroll log", &state.auto_scroll_log);
    ImGui::Checkbox("Show ImGui demo window", &state.show_imgui_demo);

    ImGui::SeparatorText("Log filters");
    ImGui::Checkbox("Info",    &state.log_show_info);
    ImGui::SameLine(); ImGui::Checkbox("Warning", &state.log_show_warning);
    ImGui::Checkbox("Error",   &state.log_show_error);
    ImGui::SameLine(); ImGui::Checkbox("Build",   &state.log_show_build);

    ImGui::End();
}
