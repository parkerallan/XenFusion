#include "panels/ViewportPanel.h"
#include "loc/Loc.h"

#include "state/EngineState.h"

#include "imgui.h"

void ViewportPanel::Render(EngineState& state)
{
    if (!state.show_viewport_panel)
        return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(loc::TWin("panel.viewport.title", "Viewport"), &state.show_viewport_panel);
    ImGui::PopStyleVar();
    if (open)
        scene_renderer_.RenderUi(state);
    ImGui::End();
}
