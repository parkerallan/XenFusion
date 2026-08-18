#include "panels/PerformancePanel.h"
#include "loc/Loc.h"

#include "state/EngineState.h"

#include "imgui.h"

void PerformancePanel::Render(EngineState& state)
{
    if (!state.show_performance_panel)
        return;

    ImGuiIO& io = ImGui::GetIO();
    frame_times_[offset_] = io.DeltaTime * 1000.0f;
    offset_ = (offset_ + 1) % kHistory;

    if (!ImGui::Begin(loc::TWin("panel.performance.title", "Performance"), &state.show_performance_panel))
    {
        ImGui::End();
        return;
    }

    int object_count = 0;
    for (const SceneFile& scene : state.scenes)
        object_count += (int)scene.objects.size();

    ImGui::Text(loc::T("performance.fps"), io.Framerate);
    ImGui::Text(loc::T("performance.frame_ms"), 1000.0f / io.Framerate);
    ImGui::Text(loc::T("performance.counts"), (int)state.scenes.size(), object_count);

    ImGui::PlotLines("##frametimes", frame_times_, kHistory, offset_,
                     loc::T("performance.plot_overlay"), 0.0f, 33.3f, ImVec2(-1.0f, 60.0f));

    ImGui::End();
}
