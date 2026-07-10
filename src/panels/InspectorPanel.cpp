#include "panels/InspectorPanel.h"

#include "project/ProjectIO.h"
#include "state/EngineState.h"
#include "ui/Icons.h"

#include "imgui.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>

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
    SceneFile*   scene = state.SelectedScene();
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

    if (changed && scene)
        scene->dirty = true;

    // --- Attributes ---
    ImGui::SeparatorText("Attributes");

    auto save = [&]() { if (scene) { scene->dirty = true; project::SaveScene(*scene); } };

    for (int a = 0; a < (int)obj->attributes.size(); ++a)
    {
        ObjectAttribute& attr = obj->attributes[a];
        ImGui::PushID(a);

        if (ImGui::SmallButton("x"))
        {
            obj->attributes.erase(obj->attributes.begin() + a);
            save();
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(ICON_FA_CUBE);
        ImGui::SameLine();
        ImGui::TextUnformatted(attr.type.c_str());

        if (attr.type == "3D Model")
        {
            // Read-only path display; set by dragging a file from the Assets panel.
            char buf[260];
            const std::string shown = attr.model_path.empty() ? "(drag a model from Assets)" : attr.model_path;
            std::strncpy(buf, shown.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::InputText("##model_path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                {
                    std::string dropped((const char*)payload->Data, (std::size_t)payload->DataSize);
                    std::string ext = std::filesystem::path(dropped).extension().string();
                    for (char& c : ext) c = (char)std::tolower((unsigned char)c);

                    if (ext == ".mesh")
                    {
                        attr.model_path = dropped; // already a baked mesh
                        save();
                    }
                    else
                    {
                        static const char* kSourceExts[] =
                            {".obj", ".fbx", ".gltf", ".glb", ".dae", ".3ds", ".ply", ".stl"};
                        bool is_model = false;
                        for (const char* e : kSourceExts) if (ext == e) { is_model = true; break; }

                        if (is_model)
                        {
                            // Store the baked .mesh path; the MeshCache bakes it
                            // from this source sibling on first use.
                            std::filesystem::path m(dropped);
                            m.replace_extension(".mesh");
                            attr.model_path = m.generic_string();
                            save();
                        }
                        else
                        {
                            state.AddLog("Not a model file: " + dropped, LogLevel::Error);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        ImGui::PopID();
    }

    if (ImGui::Button(ICON_FA_PLUS " Add Attribute"))
        ImGui::OpenPopup("add_attribute");
    if (ImGui::BeginPopup("add_attribute"))
    {
        if (ImGui::MenuItem("3D Model"))
        {
            ObjectAttribute attr;
            attr.type = "3D Model";
            obj->attributes.push_back(attr);
            save();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}
