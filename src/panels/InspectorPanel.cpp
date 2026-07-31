#include "panels/InspectorPanel.h"

#include "project/ProjectIO.h"
#include "state/EngineState.h"
#include "ui/Icons.h"
#include "video/VideoImport.h"

#include "imgui.h"
#include "stb/stb_image.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // Run an ffmpeg command hidden and wait for its exit. Video imports call
    // this from a background job; audio import still uses it synchronously.
    bool RunFfmpeg(const std::string& cmd, bool& could_start)
    {
        could_start = false;
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back('\0');
        if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                    nullptr, nullptr, &si, &pi))
            return false;
        could_start = true;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return code == 0;
    }

    // Transcode any audio ffmpeg can read into the raw MP2 elementary stream
    // the engine plays (pl_mpeg's standalone audio decoder).
    bool TranscodeToMp2(const std::filesystem::path& src,
                        const std::filesystem::path& dst,
                        EngineState& state)
    {
        // 48kHz: the 360 mixes natively at 48k — a 44.1k source would eat a
        // sample-rate conversion on every voice (and Xenia's emulation of
        // that SRC audibly crackles on mono spatial voices).
        const std::string cmd = "ffmpeg -y -i \"" + src.string() +
            "\" -vn -f mp2 -c:a mp2 -b:a 192k -ar 48000 -ac 2 \"" + dst.string() + "\"";
        bool could_start = false;
        if (!RunFfmpeg(cmd, could_start) || !std::filesystem::exists(dst))
        {
            state.AddLog(could_start
                ? "ffmpeg failed transcoding " + src.filename().string()
                : "ffmpeg not found on PATH - cannot import", LogLevel::Error);
            return false;
        }
        return true;
    }
}

struct InspectorPanel::VideoImportJob
{
    std::thread worker;
    std::atomic<bool> done = false;
    bool success = false;
    bool could_start = false;
    int scene_index = -1;
    int object_index = -1;
    int attribute_index = -1;
    std::string source_name;
    std::string relative_output;
};

InspectorPanel::InspectorPanel() = default;

InspectorPanel::~InspectorPanel()
{
    if (video_import_ && video_import_->worker.joinable())
        video_import_->worker.join();
}

void InspectorPanel::PollVideoImport(EngineState& state)
{
    if (!video_import_ || !video_import_->done.load())
        return;

    if (video_import_->worker.joinable())
        video_import_->worker.join();

    VideoImportJob& job = *video_import_;
    if (!job.success)
    {
        state.AddLog(job.could_start
            ? "ffmpeg failed transcoding " + job.source_name
            : "ffmpeg not found on PATH - cannot import", LogLevel::Error);
        video_import_.reset();
        return;
    }

    if (job.scene_index >= 0 && job.scene_index < (int)state.scenes.size())
    {
        SceneFile& scene = state.scenes[job.scene_index];
        if (job.object_index >= 0 && job.object_index < (int)scene.objects.size())
        {
            SceneObject& object = scene.objects[job.object_index];
            if (job.attribute_index >= 0 &&
                job.attribute_index < (int)object.attributes.size() &&
                object.attributes[job.attribute_index].type == "Video")
            {
                object.attributes[job.attribute_index].video_path = job.relative_output;
                scene.dirty = true;
                project::SaveScene(scene);
                state.AddLog("Video imported: " + job.relative_output);
                video_import_.reset();
                return;
            }
        }
    }

    state.AddLog("Video transcoded but its target attribute no longer exists: " +
                 job.relative_output, LogLevel::Warning);
    video_import_.reset();
}

void InspectorPanel::Render(EngineState& state)
{
    PollVideoImport(state);

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

            if (video_import_)
                ImGui::TextDisabled("Importing %s...", video_import_->source_name.c_str());

            if (!video_import_ && ImGui::BeginDragDropTarget())
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
        else if (attr.type == "Script")
        {
            // Read-only path display; set by dragging a .lua from the Assets panel.
            char buf[260];
            const std::string shown = attr.script_path.empty() ? "(drag a .lua from Assets)" : attr.script_path;
            std::strncpy(buf, shown.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::InputText("##script_path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                {
                    std::string dropped((const char*)payload->Data, (std::size_t)payload->DataSize);
                    std::string ext = std::filesystem::path(dropped).extension().string();
                    for (char& c : ext) c = (char)std::tolower((unsigned char)c);

                    if (ext == ".lua")
                    {
                        attr.script_path = dropped;
                        save();
                    }
                    else
                    {
                        state.AddLog("Not a Lua script: " + dropped, LogLevel::Error);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::TextDisabled("Runs on_start / on_update(dt) / on_trigger during Play.");
        }
        else if (attr.type == "Animator")
        {
            char path_buf[260];
            const std::string shown = attr.animator_controller_path.empty()
                ? "(drag a .anim controller from Assets)" : attr.animator_controller_path;
            std::strncpy(path_buf, shown.c_str(), sizeof(path_buf) - 1);
            path_buf[sizeof(path_buf) - 1] = '\0';

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::InputText("##animator_controller_path", path_buf, sizeof(path_buf),
                             ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                {
                    std::string dropped((const char*)payload->Data, (std::size_t)payload->DataSize);
                    std::string ext = std::filesystem::path(dropped).extension().string();
                    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                    if (ext == ".anim")
                    {
                        attr.animator_controller_path = dropped;
                        save();
                    }
                    else
                    {
                        state.AddLog("Not an animator controller: " + dropped, LogLevel::Error);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            char state_buf[128];
            std::strncpy(state_buf, attr.animator_initial_state.c_str(), sizeof(state_buf) - 1);
            state_buf[sizeof(state_buf) - 1] = '\0';
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputTextWithHint("##animator_initial_state", "Initial state (controller default)",
                                         state_buf, sizeof(state_buf)))
            {
                attr.animator_initial_state = state_buf;
                save();
            }

            bool edited = false;
            if (ImGui::DragFloat("Playback Speed", &attr.animator_playback_speed,
                                 0.01f, 0.01f, 10.0f, "%.2fx"))
            {
                if (attr.animator_playback_speed < 0.01f)
                    attr.animator_playback_speed = 0.01f;
                edited = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                save();
            if (ImGui::Checkbox("Auto Play", &attr.animator_auto_play))
                save();
            if (edited && scene)
                scene->dirty = true;
            ImGui::TextDisabled("Bind pose in edit mode; controller starts when Play begins.");
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

            const char* cam_types = "Fixed\0Follow\0Track\0";
            if (ImGui::Combo("Type", &attr.cam_type, cam_types))
            {
                // Switching to Track seeds a minimal 2-point path at the object.
                if (attr.cam_type == 2 && attr.cam_track_points.size() < 6)
                {
                    attr.cam_track_points.clear();
                    for (int k = 0; k < 3; ++k) attr.cam_track_points.push_back(obj->position[k]);
                    attr.cam_track_points.push_back(obj->position[0]);
                    attr.cam_track_points.push_back(obj->position[1]);
                    attr.cam_track_points.push_back(obj->position[2] + 10.0f); // ahead (+Z)
                }
                edited = commit = true;
            }

            if (attr.cam_type == 1) // Follow
            {
                // Target picker: any other object in this scene by name.
                const char* current = attr.cam_follow_target.empty()
                                          ? "(none)" : attr.cam_follow_target.c_str();
                if (ImGui::BeginCombo("Target", current))
                {
                    if (ImGui::Selectable("(none)", attr.cam_follow_target.empty()))
                    {
                        attr.cam_follow_target.clear();
                        edited = commit = true;
                    }
                    if (scene)
                        for (const SceneObject& other : scene->objects)
                        {
                            if (other.name == obj->name)
                                continue; // a camera cannot follow itself
                            if (ImGui::Selectable(other.name.c_str(),
                                                  other.name == attr.cam_follow_target))
                            {
                                attr.cam_follow_target = other.name;
                                edited = commit = true;
                            }
                        }
                    ImGui::EndCombo();
                }
                if (ImGui::Checkbox("Lock Position", &attr.cam_follow_lock))
                    edited = commit = true;
                ImGui::TextDisabled(attr.cam_follow_lock
                    ? "Stays at its transform + offset, aims at the target."
                    : "Rides at target + offset; keeps its own rotation.");
                edited |= ImGui::DragFloat3("Offset", attr.cam_follow_offset, 0.05f);
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::BeginDisabled(attr.cam_follow_lock);
                edited |= ImGui::DragFloat2("Orbit", attr.cam_follow_orbit, 0.5f, -360.0f, 360.0f, "%.2f deg");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::EndDisabled();
                edited |= ImGui::DragFloat3("Rotation Offset", attr.cam_follow_rot_offset, 0.5f, -360.0f, 360.0f, "%.2f deg");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat("Smoothing", &attr.cam_follow_smoothing, 0.005f, 0.0f, 2.0f, "%.3f s");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                if (attr.cam_follow_smoothing < 0.0f) attr.cam_follow_smoothing = 0.0f;
            }
            else if (attr.cam_type == 2) // Track
            {
                const int point_count = (int)(attr.cam_track_points.size() / 3);
                ImGui::Text("Path Points (%d)", point_count);
                int remove_index = -1;
                for (int p = 0; p < point_count; ++p)
                {
                    ImGui::PushID(p);
                    if (ImGui::RadioButton("##select", state.selected_track_point_index == p))
                        state.selected_track_point_index = p;
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-60.0f);
                    edited |= ImGui::DragFloat3("##point", &attr.cam_track_points[p * 3], 0.05f);
                    commit |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(point_count <= 2); // spline needs 2+ points
                    if (ImGui::SmallButton("X"))
                        remove_index = p;
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                if (remove_index >= 0)
                {
                    attr.cam_track_points.erase(attr.cam_track_points.begin() + remove_index * 3,
                                                attr.cam_track_points.begin() + remove_index * 3 + 3);
                    if (state.selected_track_point_index >= point_count - 1)
                        state.selected_track_point_index = point_count - 2;
                    edited = commit = true;
                }
                if (ImGui::Button("Add Point"))
                {
                    // Append 2 units past the last point along +Z.
                    const std::size_t n = attr.cam_track_points.size();
                    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
                    if (n >= 3) { lx = attr.cam_track_points[n-3]; ly = attr.cam_track_points[n-2]; lz = attr.cam_track_points[n-1]; }
                    attr.cam_track_points.push_back(lx);
                    attr.cam_track_points.push_back(ly);
                    attr.cam_track_points.push_back(lz + 2.0f);
                    state.selected_track_point_index = point_count;
                    edited = commit = true;
                }
                edited |= ImGui::DragFloat("Speed", &attr.cam_track_speed, 0.05f, 0.0f, 1000.0f, "%.2f u/s");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat("Acceleration", &attr.cam_track_accel, 0.05f, 0.0f, 1000.0f, "%.2f u/s^2");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat3("Rotation Offset##track", attr.cam_track_rot_offset, 0.5f, -360.0f, 360.0f, "%.2f deg");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                if (attr.cam_track_speed < 0.0f) attr.cam_track_speed = 0.0f;
                if (attr.cam_track_accel < 0.0f) attr.cam_track_accel = 0.0f;
            }

            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Directional Light" || attr.type == "Point Light" ||
                 attr.type == "Spot Light" || attr.type == "Environment Light")
        {
            const bool is_point = (attr.type == "Point Light");
            const bool is_spot  = (attr.type == "Spot Light");
            const bool is_env   = (attr.type == "Environment Light");
            ImGui::TextDisabled(is_env   ? "Flat ambient boost for the whole scene."
                                : is_spot ? "Position and direction use the object transform."
                                : is_point ? "Position uses the object position."
                                           : "Direction uses the object rotation.");
            bool edited = false, commit = false;
            edited |= ImGui::ColorEdit3("Color", attr.light_color);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat("Intensity", &attr.light_intensity, 0.05f, 0.0f, 100.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (is_point || is_spot)
            {
                edited |= ImGui::DragFloat("Range", &attr.light_range, 0.1f, 0.1f, 1000.0f, "%.1f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (is_spot)
            {
                edited |= ImGui::DragFloat("Inner Cone", &attr.light_inner_deg, 0.25f, 0.1f, 89.0f, "%.1f deg");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat("Outer Cone", &attr.light_outer_deg, 0.25f, 0.1f, 89.0f, "%.1f deg");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                // Volumetric beam: the cone made visible in the air (flashlight
                // in fog) — an additive cone mesh, see beam.hlsl.
                if (ImGui::Checkbox("Project Light Shaft", &attr.light_volumetric)) { edited = commit = true; }
                if (attr.light_volumetric)
                {
                    edited |= ImGui::DragFloat("Beam Intensity", &attr.light_volumetric_intensity, 0.01f, 0.0f, 10.0f, "%.2f");
                    commit |= ImGui::IsItemDeactivatedAfterEdit();
                }
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
                ImGui::TextUnformatted("Lock rotation");
                ImGui::SameLine();
                if (ImGui::Checkbox("X##LockRotation", &attr.phys_lock_rotation[0])) { edited = commit = true; }
                ImGui::SameLine();
                if (ImGui::Checkbox("Y##LockRotation", &attr.phys_lock_rotation[1])) { edited = commit = true; }
                ImGui::SameLine();
                if (ImGui::Checkbox("Z##LockRotation", &attr.phys_lock_rotation[2])) { edited = commit = true; }
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
        else if (attr.type == "Image")
        {
            char buf[260];
            const std::string shown = attr.image_path.empty() ? "(drag a PNG or JPG from Assets)" : attr.image_path;
            std::strncpy(buf, shown.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::InputText("##image_path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                {
                    std::string dropped((const char*)payload->Data, (std::size_t)payload->DataSize);
                    std::string ext = std::filesystem::path(dropped).extension().string();
                    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg")
                    {
                        attr.image_path = dropped;
                        int width = 0, height = 0, channels = 0;
                        const std::filesystem::path source = state.project_root / dropped;
                        if (stbi_info(source.string().c_str(), &width, &height, &channels) &&
                            width > 0 && height > 0)
                        {
                            attr.image_w = (float)width;
                            attr.image_h = (float)height;
                        }
                        save();
                    }
                    else
                    {
                        state.AddLog("Image attributes support PNG and JPG files", LogLevel::Error);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            bool edited = false, commit = false;
            if (ImGui::Checkbox("Stretch To Screen", &attr.image_stretch)) { edited = commit = true; }
            ImGui::BeginDisabled(attr.image_stretch);
            float pos[2] = { attr.image_x, attr.image_y };
            if (ImGui::DragFloat2("Position", pos, 1.0f, -4000.0f, 4000.0f, "%.0f"))
            {
                attr.image_x = pos[0]; attr.image_y = pos[1];
                edited = true;
            }
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (attr.image_lock_aspect)
            {
                const float aspect = attr.image_h > 0.0f ? attr.image_w / attr.image_h : 1.0f;
                if (ImGui::DragFloat("Width", &attr.image_w, 1.0f, 1.0f, 4000.0f, "%.0f"))
                {
                    attr.image_h = attr.image_w / aspect;
                    edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else
            {
                float size[2] = { attr.image_w, attr.image_h };
                if (ImGui::DragFloat2("Size", size, 1.0f, 1.0f, 4000.0f, "%.0f"))
                {
                    attr.image_w = size[0]; attr.image_h = size[1];
                    edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (ImGui::Checkbox("Lock Aspect Ratio", &attr.image_lock_aspect)) { edited = commit = true; }
            ImGui::EndDisabled();
            ImGui::TextDisabled("Position/size in 1280x720 reference space.");
            edited |= ImGui::ColorEdit3("Tint", attr.image_tint);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat("Alpha", &attr.image_alpha, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragInt("Priority", &attr.image_priority, 0.1f, 0, 100);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextDisabled("Higher priority draws behind lower.");
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Text")
        {
            char path_buf[260];
            const std::string shown = attr.text_font_path.empty()
                ? "(drag a TTF or OTF font from Assets)" : attr.text_font_path;
            std::strncpy(path_buf, shown.c_str(), sizeof(path_buf) - 1);
            path_buf[sizeof(path_buf) - 1] = '\0';

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::InputText("##text_font_path", path_buf, sizeof(path_buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                {
                    std::string dropped((const char*)payload->Data, (std::size_t)payload->DataSize);
                    std::string ext = std::filesystem::path(dropped).extension().string();
                    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                    if (ext == ".ttf" || ext == ".otf")
                    {
                        attr.text_font_path = dropped;
                        save();
                    }
                    else
                    {
                        state.AddLog("Text attributes support TTF and OTF fonts", LogLevel::Error);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            char text_buf[4096];
            std::strncpy(text_buf, attr.text_value.c_str(), sizeof(text_buf) - 1);
            text_buf[sizeof(text_buf) - 1] = '\0';
            if (ImGui::InputTextMultiline("Text", text_buf, sizeof(text_buf), ImVec2(-1.0f, 100.0f)))
            {
                attr.text_value = text_buf;
                if (scene) scene->dirty = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) save();

            bool edited = false, commit = false;
            float position[2] = { attr.text_x, attr.text_y };
            if (ImGui::DragFloat2("Position", position, 1.0f, -4000.0f, 4000.0f, "%.0f"))
            {
                attr.text_x = position[0]; attr.text_y = position[1]; edited = true;
            }
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (attr.text_lock_aspect)
            {
                const float aspect = attr.text_h > 0.0f ? attr.text_w / attr.text_h : 1.0f;
                if (ImGui::DragFloat("Width", &attr.text_w, 1.0f, 1.0f, 4000.0f, "%.0f"))
                {
                    attr.text_h = attr.text_w / aspect; edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else
            {
                float size[2] = { attr.text_w, attr.text_h };
                if (ImGui::DragFloat2("Size", size, 1.0f, 1.0f, 4000.0f, "%.0f"))
                {
                    attr.text_w = size[0]; attr.text_h = size[1]; edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (ImGui::Checkbox("Lock Aspect Ratio", &attr.text_lock_aspect)) { edited = commit = true; }
            edited |= ImGui::DragFloat("Font Size", &attr.text_font_size, 0.5f, 1.0f, 512.0f, "%.1f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::ColorEdit3("Color", attr.text_color);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat("Alpha", &attr.text_alpha, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragInt("Priority", &attr.text_priority, 0.1f, 0, 100);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextDisabled("Position/size in 1280x720 reference space.");
            ImGui::TextDisabled("Higher priority draws behind lower.");
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Video")
        {
            // Read-only path display; set by dragging a video from the Assets
            // panel. Non-.mpg sources are transcoded via ffmpeg on drop.
            bool profile_changed = false;
            profile_changed |= ImGui::RadioButton("360p", &attr.video_import_profile,
                                                  video_import::Profile360p);
            ImGui::SameLine();
            profile_changed |= ImGui::RadioButton("480p", &attr.video_import_profile,
                                                  video_import::Profile480p);
            ImGui::SameLine();
            profile_changed |= ImGui::RadioButton("720p", &attr.video_import_profile,
                                                  video_import::Profile720p);
            if (profile_changed)
                save();

            char buf[260];
            const std::string shown = attr.video_path.empty() ? "(drag a video from Assets)" : attr.video_path;
            std::strncpy(buf, shown.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::InputText("##video_path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                {
                    std::string dropped((const char*)payload->Data, (std::size_t)payload->DataSize);
                    std::string ext = std::filesystem::path(dropped).extension().string();
                    for (char& c : ext) c = (char)std::tolower((unsigned char)c);

                    if (ext == ".mpg" || ext == ".mpeg")
                    {
                        attr.video_path = dropped;
                        save();
                    }
                    else
                    {
                        static const char* kSourceExts[] =
                            {".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v", ".wmv"};
                        bool is_video = false;
                        for (const char* e : kSourceExts) if (ext == e) { is_video = true; break; }

                        if (is_video)
                        {
                            // Transcode next to the source; the attribute points
                            // at the .mpg (the only format the engine decodes).
                            std::filesystem::path rel(dropped);
                            rel.replace_extension(".mpg");
                            state.AddLog("Transcoding " + dropped + " -> " + rel.generic_string() + " ...");
                            const std::filesystem::path source = state.project_root / dropped;
                            const std::filesystem::path destination = state.project_root / rel;
                            const std::string command = video_import::BuildFfmpegCommand(
                                source.string(), destination.string(), attr.video_import_profile);

                            video_import_.reset(new VideoImportJob());
                            VideoImportJob* job = video_import_.get();
                            job->scene_index = state.selected_scene;
                            job->object_index = state.selected_object;
                            job->attribute_index = a;
                            job->source_name = source.filename().string();
                            job->relative_output = rel.generic_string();
                            job->worker = std::thread([job, command, destination]()
                            {
                                job->success = RunFfmpeg(command, job->could_start) &&
                                               std::filesystem::exists(destination);
                                job->done.store(true);
                            });
                        }
                        else
                        {
                            state.AddLog("Not a video file: " + dropped, LogLevel::Error);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            bool edited = false, commit = false;
            if (ImGui::Checkbox("Stretch To Screen", &attr.video_stretch)) { edited = commit = true; }
            ImGui::BeginDisabled(attr.video_stretch);
            float pos[2] = { attr.video_x, attr.video_y };
            if (ImGui::DragFloat2("Position", pos, 1.0f, -4000.0f, 4000.0f, "%.0f"))
            {
                attr.video_x = pos[0]; attr.video_y = pos[1];
                edited = true;
            }
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (attr.video_lock_aspect)
            {
                edited |= ImGui::DragFloat("Width", &attr.video_w, 1.0f, 16.0f, 4000.0f, "%.0f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else
            {
                float size[2] = { attr.video_w, attr.video_h };
                if (ImGui::DragFloat2("Size", size, 1.0f, 16.0f, 4000.0f, "%.0f"))
                {
                    attr.video_w = size[0]; attr.video_h = size[1];
                    edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (ImGui::Checkbox("Lock Aspect Ratio", &attr.video_lock_aspect)) { edited = commit = true; }
            ImGui::EndDisabled();
            ImGui::TextDisabled("Position/size in 1280x720 reference space.");
            edited |= ImGui::ColorEdit3("Tint", attr.video_tint);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat("Alpha", &attr.video_alpha, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragInt("Priority", &attr.video_priority, 0.1f, 0, 100);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextDisabled("Higher priority draws behind lower.");
            const char* modes = "Off\0Play Once\0Loop\0";
            if (ImGui::Combo("Play Mode", &attr.video_play_mode, modes)) { edited = commit = true; }
            edited |= ImGui::DragFloat("Volume", &attr.video_volume, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::Checkbox("Muted", &attr.video_muted)) { edited = commit = true; }
            ImGui::TextDisabled("Edit mode shows the first frame; plays in Play mode.");
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Audio")
        {
            // Read-only path display; set by dragging audio from the Assets
            // panel. Non-.mp2 sources are transcoded via ffmpeg on drop.
            char buf[260];
            const std::string shown = attr.audio_path.empty() ? "(drag audio from Assets)" : attr.audio_path;
            std::strncpy(buf, shown.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::InputText("##audio_path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                {
                    std::string dropped((const char*)payload->Data, (std::size_t)payload->DataSize);
                    std::string ext = std::filesystem::path(dropped).extension().string();
                    for (char& c : ext) c = (char)std::tolower((unsigned char)c);

                    if (ext == ".mp2")
                    {
                        attr.audio_path = dropped;
                        save();
                    }
                    else
                    {
                        static const char* kSourceExts[] =
                            {".wav", ".mp3", ".ogg", ".flac", ".m4a", ".aac", ".wma"};
                        bool is_audio = false;
                        for (const char* e : kSourceExts) if (ext == e) { is_audio = true; break; }

                        if (is_audio)
                        {
                            std::filesystem::path rel(dropped);
                            rel.replace_extension(".mp2");
                            state.AddLog("Transcoding " + dropped + " -> " + rel.generic_string() + " ...");
                            if (TranscodeToMp2(state.project_root / dropped,
                                               state.project_root / rel, state))
                            {
                                attr.audio_path = rel.generic_string();
                                save();
                                state.AddLog("Audio imported: " + attr.audio_path);
                            }
                        }
                        else
                        {
                            state.AddLog("Not an audio file: " + dropped, LogLevel::Error);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            bool edited = false, commit = false;
            const char* modes = "Off\0On\0";
            if (ImGui::Combo("Play Mode", &attr.audio_play, modes)) { edited = commit = true; }
            edited |= ImGui::DragFloat("Volume", &attr.audio_volume, 0.05f, 0.0f, 20.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat("Pitch", &attr.audio_pitch, 0.005f, 0.1f, 4.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::Checkbox("Loop", &attr.audio_loop)) { edited = commit = true; }
            const char* audioClasses = "Effect\0Music\0Ambience\0Dialogue\0";
            if (ImGui::Combo("Class", &attr.audio_class, audioClasses)) { edited = commit = true; }
            edited |= ImGui::DragInt("Priority", &attr.audio_priority, 1.0f, -100, 100);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            const char* loadModes = "Auto\0Resident\0Stream\0";
            if (ImGui::Combo("Load Mode", &attr.audio_load_mode, loadModes)) { edited = commit = true; }
            if (ImGui::Checkbox("3D Spatialize", &attr.audio_spatial)) { edited = commit = true; }
            if (attr.audio_spatial)
            {
                edited |= ImGui::DragFloat("Min Distance", &attr.audio_min_dist, 0.05f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat("Max Distance", &attr.audio_max_dist, 0.1f, 0.02f, 1000.0f, "%.1f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat("Doppler Factor", &attr.audio_doppler, 0.02f, 0.0f, 10.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            ImGui::TextDisabled("Plays in Play mode only; position follows the object.");
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
        if (ImGui::MenuItem("Script"))
        {
            ObjectAttribute attr;
            attr.type = "Script";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem("Animator"))
        {
            ObjectAttribute attr;
            attr.type = "Animator";
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
        if (ImGui::MenuItem("Spot Light"))
        {
            ObjectAttribute attr;
            attr.type = "Spot Light";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem("Environment Light"))
        {
            ObjectAttribute attr;
            attr.type = "Environment Light";
            attr.light_intensity = 0.2f; // an ambient boost, not a blowout
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
        if (ImGui::MenuItem("Image"))
        {
            ObjectAttribute attr;
            attr.type = "Image";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem("Text"))
        {
            ObjectAttribute attr;
            attr.type = "Text";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem("Video"))
        {
            ObjectAttribute attr;
            attr.type = "Video";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem("Audio"))
        {
            ObjectAttribute attr;
            attr.type = "Audio";
            obj->attributes.push_back(attr);
            save();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}
