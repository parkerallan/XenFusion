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
            // Transparency (blend vs. cutout) is derived from the diffuse's alpha
            // channel at load time — no toggle, same as the rest of the material.
        }
        else if (attr.type == "Shader")
        {
            // Read-only path display; set by dragging a .hlsl from the Assets panel.
            char buf[260];
            const std::string shown = attr.shader_path.empty() ? "(drag a .hlsl from Assets)" : attr.shader_path;
            std::strncpy(buf, shown.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::InputText("##shader_path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                {
                    std::string dropped((const char*)payload->Data, (std::size_t)payload->DataSize);
                    std::string ext = std::filesystem::path(dropped).extension().string();
                    for (char& c : ext) c = (char)std::tolower((unsigned char)c);

                    if (ext == ".hlsl" || ext == ".fx")
                    {
                        attr.shader_path = dropped;
                        save();
                    }
                    else
                    {
                        state.AddLog("Not a shader file: " + dropped, LogLevel::Error);
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        else if (attr.type == "Camera")
        {
            // Edits mark the scene dirty every tick; the file is written once,
            // when the drag ends (the gizmo's commit-on-release pattern).
            bool edited = false, commit = false;
            edited |= ImGui::DragFloat("FOV",  &attr.cam_fov, 0.5f, 1.0f, 179.0f, "%.1f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat("Near", &attr.cam_near, 0.05f, 0.01f, 1000.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat("Far",  &attr.cam_far, 1.0f, 1.0f, 10000.0f, "%.0f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::Checkbox("Active", &attr.cam_active))
            {
                // One active camera per scene: activating this one clears the rest.
                if (attr.cam_active && scene)
                    for (SceneObject& other : scene->objects)
                        for (ObjectAttribute& oa : other.attributes)
                            if (&oa != &attr && oa.type == "Camera")
                                oa.cam_active = false;
                edited = commit = true;
            }
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Directional Light" || attr.type == "Point Light")
        {
            const bool is_point = (attr.type == "Point Light");
            ImGui::TextDisabled(is_point ? "Position uses the object position."
                                         : "Direction uses the object rotation.");
            bool edited = false, commit = false;
            edited |= ImGui::ColorEdit3("Color", attr.light_color);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat("Intensity", &attr.light_intensity, 0.05f, 0.0f, 100.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (is_point)
            {
                edited |= ImGui::DragFloat("Range", &attr.light_range, 0.1f, 0.1f, 1000.0f, "%.1f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Rigid Body")
        {
            bool edited = false, commit = false;
            const char* kinds = "Static\0Dynamic\0Kinematic\0";
            if (ImGui::Combo("Body", &attr.phys_kind, kinds)) { edited = commit = true; }
            const char* shapes = "Box\0Sphere\0Capsule\0Cylinder\0Mesh (convex)\0Mesh (exact)\0";
            if (ImGui::Combo("Shape", &attr.phys_shape, shapes)) { edited = commit = true; }
            if (attr.phys_shape == 0) // Box
            {
                edited |= ImGui::DragFloat3("Half-extents", attr.phys_size, 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else if (attr.phys_shape == 1) // Sphere
            {
                edited |= ImGui::DragFloat("Radius", &attr.phys_size[0], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else if (attr.phys_shape == 2 || attr.phys_shape == 3) // Capsule / Cylinder
            {
                edited |= ImGui::DragFloat("Radius", &attr.phys_size[0], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat("Half-height", &attr.phys_size[1], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else // Mesh (convex = 4, exact = 5): geometry comes from the 3D Model
            {
                ImGui::TextDisabled("Uses this object's 3D Model mesh.");
                if (attr.phys_shape == 5 && attr.phys_kind == 1) // exact + dynamic
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                       "Exact mesh is static-only; this body won't move.");
            }
            if (attr.phys_kind == 1) // Dynamic
            {
                edited |= ImGui::DragFloat("Mass", &attr.phys_mass, 0.05f, 0.001f, 10000.0f, "%.3f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat("Linear damping",  &attr.phys_lin_damping, 0.005f, 0.0f, 1.0f, "%.3f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat("Angular damping", &attr.phys_ang_damping, 0.005f, 0.0f, 1.0f, "%.3f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::Checkbox("Gravity", &attr.phys_gravity)) { edited = commit = true; }
                if (attr.phys_gravity)
                {
                    edited |= ImGui::DragFloat("Gravity scale", &attr.phys_gravity_scale, 0.02f, -10.0f, 10.0f, "%.2f");
                    commit |= ImGui::IsItemDeactivatedAfterEdit();
                }
            }
            // Contact material: applies to every body (a bouncy floor needs
            // restitution too; Bullet combines the two bodies' values on contact).
            edited |= ImGui::DragFloat("Bounciness", &attr.phys_restitution, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat("Friction", &attr.phys_friction, 0.005f, 0.0f, 2.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Trigger Volume")
        {
            ImGui::TextDisabled("Reports overlaps; no physical response.");
            bool edited = false, commit = false;
            const char* shapes = "Box\0Sphere\0Capsule\0Cylinder\0";
            if (ImGui::Combo("Shape", &attr.trig_shape, shapes)) { edited = commit = true; }
            if (attr.trig_shape == 0) // Box
            {
                edited |= ImGui::DragFloat3("Half-extents", attr.trig_size, 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else if (attr.trig_shape == 1) // Sphere
            {
                edited |= ImGui::DragFloat("Radius", &attr.trig_size[0], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else // Capsule / Cylinder (Y-up): radius + half-height
            {
                edited |= ImGui::DragFloat("Radius", &attr.trig_size[0], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat("Half-height", &attr.trig_size[1], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (edited && scene) scene->dirty = true;
            if (commit) save();
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
        if (ImGui::MenuItem("Shader"))
        {
            ObjectAttribute attr;
            attr.type = "Shader";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem("Camera"))
        {
            ObjectAttribute attr;
            attr.type = "Camera";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem("Directional Light"))
        {
            ObjectAttribute attr;
            attr.type = "Directional Light";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem("Point Light"))
        {
            ObjectAttribute attr;
            attr.type = "Point Light";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem("Rigid Body"))
        {
            ObjectAttribute attr;
            attr.type = "Rigid Body";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem("Trigger Volume"))
        {
            ObjectAttribute attr;
            attr.type = "Trigger Volume";
            obj->attributes.push_back(attr);
            save();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}
