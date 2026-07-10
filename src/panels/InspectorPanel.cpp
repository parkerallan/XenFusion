#include "panels/InspectorPanel.h"

#include "state/EngineState.h"

#include "imgui.h"

#include <cstring>

void InspectorPanel::Render(EngineState& state)
{
    if (!state.show_inspector_panel)
        return;

    if (!ImGui::Begin("Inspector", &state.show_inspector_panel))
    {
        ImGui::End();
        return;
    }

    SceneObject* obj = state.SelectedObject();
    if (!obj)
    {
        ImGui::TextDisabled("Select an object in the Files panel.");
        ImGui::End();
        return;
    }

    bool changed = false;

    // Name.
    char name_buf[128];
    std::strncpy(name_buf, obj->name.c_str(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##name", name_buf, sizeof(name_buf)))
    {
        obj->name = name_buf;
        changed = true;
    }

    changed |= ImGui::Checkbox("Visible", &obj->visible);

    ImGui::SeparatorText("Transform");
    changed |= ImGui::DragFloat3("Position", obj->position, 0.05f);
    changed |= ImGui::DragFloat3("Rotation", obj->rotation, 0.5f);
    changed |= ImGui::DragFloat3("Scale",    obj->scale,    0.05f);

    // Mark the scene dirty so Ctrl+S / Save persists the edit.
    if (changed)
        if (SceneFile* scene = state.SelectedScene())
            scene->dirty = true;

    ImGui::End();
}
