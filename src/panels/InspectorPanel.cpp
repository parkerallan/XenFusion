#include "panels/InspectorPanel.h"
#include "loc/Loc.h"

#include "image/GifAnim.h"
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
    // Attribute type names are scene-JSON identifiers — the comparisons below
    // and the saved scenes depend on the exact English spelling, so the shown
    // name is looked up from the identifier rather than translated in place.
    const char* AttributeTypeLabel(const std::string& type)
    {
        std::string key = "inspector.attribute.";
        for (std::size_t i = 0; i < type.size(); ++i)
        {
            const unsigned char c = (unsigned char)type[i];
            key += std::isalnum(c) ? (char)std::tolower(c) : '_';
        }

        // loc::T returns the key pointer itself when there is no entry, and
        // key is a local — fall back to the identifier, which the caller owns.
        const char* label = loc::T(key.c_str());
        return (label == key.c_str()) ? type.c_str() : label;
    }

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

    if (!ImGui::Begin(loc::TWin("panel.inspector.title", "Inspector"), &state.show_inspector_panel))
    {
        ImGui::End();
        return;
    }

    SceneObject* obj = state.SelectedObject();
    SceneFile*   scene = state.SelectedScene();
    if (!obj)
    {
        ImGui::TextDisabled(loc::T("inspector.hint"));
        ImGui::End();
        return;
    }

    bool changed = false;

    // Name.
    char name_buf[128];
    std::strncpy(name_buf, obj->name.c_str(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##name", name_buf, sizeof(name_buf)) && name_buf[0] != '\0' && scene)
    {
        const std::string oldName = obj->name;
        const std::string newName = project::MakeUniqueObjectName(*scene, name_buf, state.selected_object);
        obj->name = newName;
        for (SceneObject& object : scene->objects)
            if (object.parent == oldName) object.parent = newName;
        changed = true;
    }

    changed |= ImGui::Checkbox(loc::TL("inspector.visible"), &obj->visible);

    // Tags: a comma-separated list, since they are short labels a script matches
    // exactly (find_by_tag / obj:has_tag) rather than structured data.
    {
        std::string joined;
        for (std::size_t t = 0; t < obj->tags.size(); ++t)
        {
            if (t) joined += ", ";
            joined += obj->tags[t];
        }
        char tag_buf[256];
        std::strncpy(tag_buf, joined.c_str(), sizeof(tag_buf) - 1);
        tag_buf[sizeof(tag_buf) - 1] = '\0';
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##tags", "Tags (comma separated)", tag_buf, sizeof(tag_buf)))
        {
            obj->tags.clear();
            const std::string text = tag_buf;
            std::size_t start = 0;
            while (start <= text.size())
            {
                const std::size_t comma = text.find(',', start);
                const std::size_t end = (comma == std::string::npos) ? text.size() : comma;
                std::size_t b = start, e = end;
                while (b < e && std::isspace((unsigned char)text[b])) ++b;
                while (e > b && std::isspace((unsigned char)text[e - 1])) --e;
                if (e > b) obj->tags.push_back(text.substr(b, e - b));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            changed = true;
        }
    }

    ImGui::SeparatorText(loc::T("inspector.transform"));
    changed |= ImGui::DragFloat3(loc::TL("inspector.position"), obj->position, 0.05f);
    changed |= ImGui::DragFloat3(loc::TL("inspector.rotation"), obj->rotation, 0.5f);
    changed |= ImGui::DragFloat3(loc::TL("inspector.scale"),    obj->scale,    0.05f);

    if (changed && scene)
        scene->dirty = true;

    // --- Attributes ---
    ImGui::SeparatorText(loc::T("inspector.attributes"));

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
        ImGui::TextUnformatted(AttributeTypeLabel(attr.type));

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

            // ImGui binds the target to the LAST submitted item, so anything
            // drawn between this and the path field steals the drop zone.
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

            if (ImGui::Checkbox(loc::TL("inspector.model.cast_shadow"), &attr.cast_shadow))
            {
                save();
            }

            if (video_import_)
                ImGui::TextDisabled(loc::T("inspector.model.importing"), video_import_->source_name.c_str());
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
            ImGui::TextDisabled(loc::T("inspector.script.hint"));
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
            if (ImGui::DragFloat(loc::TL("inspector.animator.playback_speed"), &attr.animator_playback_speed,
                                 0.01f, 0.01f, 10.0f, "%.2fx"))
            {
                if (attr.animator_playback_speed < 0.01f)
                    attr.animator_playback_speed = 0.01f;
                edited = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                save();
            if (ImGui::Checkbox(loc::TL("inspector.animator.auto_play"), &attr.animator_auto_play))
                save();
            if (edited && scene)
                scene->dirty = true;
            ImGui::TextDisabled(loc::T("inspector.animator.hint"));
        }
        else if (attr.type == "Camera")
        {
            // Edits mark the scene dirty every tick; the file is written once,
            // when the drag ends (the gizmo's commit-on-release pattern).
            bool edited = false, commit = false;
            edited |= ImGui::DragFloat(loc::TL("inspector.camera.fov"),  &attr.cam_fov, 0.5f, 1.0f, 179.0f, "%.1f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat(loc::TL("inspector.camera.near"), &attr.cam_near, 0.05f, 0.01f, 1000.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat(loc::TL("inspector.camera.far"),  &attr.cam_far, 1.0f, 1.0f, 10000.0f, "%.0f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::Checkbox(loc::TL("inspector.camera.active"), &attr.cam_active))
            {
                // One active camera per scene: activating this one clears the rest.
                if (attr.cam_active && scene)
                    for (SceneObject& other : scene->objects)
                        for (ObjectAttribute& oa : other.attributes)
                            if (&oa != &attr && oa.type == "Camera")
                                oa.cam_active = false;
                edited = commit = true;
            }

            const char* cam_types = loc::T("inspector.camera.type_items");
            if (ImGui::Combo(loc::TL("inspector.camera.type"), &attr.cam_type, cam_types))
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
                if (ImGui::BeginCombo(loc::TL("inspector.camera.target"), current))
                {
                    if (ImGui::Selectable(loc::TL("inspector.camera.none"), attr.cam_follow_target.empty()))
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
                if (ImGui::Checkbox(loc::TL("inspector.camera.lock_position"), &attr.cam_follow_lock))
                    edited = commit = true;
                ImGui::TextDisabled(attr.cam_follow_lock
                    ? "Stays at its transform + offset, aims at the target."
                    : "Rides at target + offset; keeps its own rotation.");
                edited |= ImGui::DragFloat3(loc::TL("inspector.camera.offset"), attr.cam_follow_offset, 0.05f);
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::BeginDisabled(attr.cam_follow_lock);
                edited |= ImGui::DragFloat2(loc::TL("inspector.camera.orbit"), attr.cam_follow_orbit, 0.5f, -360.0f, 360.0f, "%.2f deg");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::EndDisabled();
                edited |= ImGui::DragFloat3(loc::TL("inspector.camera.rotation_offset"), attr.cam_follow_rot_offset, 0.5f, -360.0f, 360.0f, "%.2f deg");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat(loc::TL("inspector.camera.smoothing"), &attr.cam_follow_smoothing, 0.005f, 0.0f, 2.0f, "%.3f s");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                if (attr.cam_follow_smoothing < 0.0f) attr.cam_follow_smoothing = 0.0f;
            }
            else if (attr.cam_type == 2) // Track
            {
                const int point_count = (int)(attr.cam_track_points.size() / 3);
                ImGui::Text(loc::T("inspector.camera.path_points"), point_count);
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
                if (ImGui::Button(loc::TL("inspector.camera.add_point")))
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
                edited |= ImGui::DragFloat(loc::TL("inspector.camera.speed"), &attr.cam_track_speed, 0.05f, 0.0f, 1000.0f, "%.2f u/s");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat(loc::TL("inspector.camera.acceleration"), &attr.cam_track_accel, 0.05f, 0.0f, 1000.0f, "%.2f u/s^2");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat3(loc::TL("inspector.camera.rotation_offset_track"), attr.cam_track_rot_offset, 0.5f, -360.0f, 360.0f, "%.2f deg");
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
            edited |= ImGui::ColorEdit3(loc::TL("inspector.light.color"), attr.light_color);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat(loc::TL("inspector.light.intensity"), &attr.light_intensity, 0.05f, 0.0f, 100.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            const char* light_modes = loc::T("inspector.light.mode_items");
            if (ImGui::Combo(loc::TL("inspector.light.mode"), &attr.light_mode, light_modes)) { edited = commit = true; }
            if (is_point || is_spot)
            {
                edited |= ImGui::DragFloat(loc::TL("inspector.light.range"), &attr.light_range, 0.1f, 0.1f, 1000.0f, "%.1f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (is_spot)
            {
                edited |= ImGui::DragFloat(loc::TL("inspector.light.inner_cone"), &attr.light_inner_deg, 0.25f, 0.1f, 89.0f, "%.1f deg");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat(loc::TL("inspector.light.outer_cone"), &attr.light_outer_deg, 0.25f, 0.1f, 89.0f, "%.1f deg");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                // Volumetric beam: the cone made visible in the air (flashlight
                // in fog) — an additive cone mesh, see beam.hlsl.
                if (ImGui::Checkbox(loc::TL("inspector.light.project_light_shaft"), &attr.light_volumetric)) { edited = commit = true; }
                if (attr.light_volumetric)
                {
                    edited |= ImGui::DragFloat(loc::TL("inspector.light.beam_intensity"), &attr.light_volumetric_intensity, 0.01f, 0.0f, 10.0f, "%.2f");
                    commit |= ImGui::IsItemDeactivatedAfterEdit();
                }
            }
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Skybox")
        {
            char buf[260];
            const std::string shown = attr.sky_path.empty()
                ? "(drag an equirectangular PNG or JPG from Assets)" : attr.sky_path;
            std::strncpy(buf, shown.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::InputText("##sky_path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
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
                        attr.sky_path = dropped;
                        save();
                    }
                    else
                    {
                        // The frame buffer is LDR and the console's texture
                        // bundler takes neither format, so an .hdr/.exr sky has
                        // to be tone-mapped to PNG first.
                        state.AddLog("Skybox attributes support PNG and JPG files", LogLevel::Error);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            bool edited = false, commit = false;
            edited |= ImGui::DragFloat(loc::TL("inspector.skybox.rotation"), &attr.sky_rotation, 0.25f, -3600.0f, 3600.0f, "%.1f deg");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextDisabled(loc::T("inspector.skybox.hint"));
            ImGui::TextDisabled(loc::T("inspector.skybox.hint_2"));
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Rigid Body")
        {
            bool edited = false, commit = false;
            const char* kinds = loc::T("inspector.rigidbody.body_items");
            if (ImGui::Combo(loc::TL("inspector.rigidbody.body"), &attr.phys_kind, kinds)) { edited = commit = true; }
            const char* shapes = loc::T("inspector.rigidbody.shape_items");
            if (ImGui::Combo(loc::TL("inspector.rigidbody.shape"), &attr.phys_shape, shapes)) { edited = commit = true; }
            if (attr.phys_shape == 0) // Box
            {
                edited |= ImGui::DragFloat3(loc::TL("inspector.rigidbody.half_extents"), attr.phys_size, 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else if (attr.phys_shape == 1) // Sphere
            {
                edited |= ImGui::DragFloat(loc::TL("inspector.rigidbody.radius"), &attr.phys_size[0], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else if (attr.phys_shape == 2 || attr.phys_shape == 3) // Capsule / Cylinder
            {
                edited |= ImGui::DragFloat(loc::TL("inspector.rigidbody.radius"), &attr.phys_size[0], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat(loc::TL("inspector.rigidbody.half_height"), &attr.phys_size[1], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else // Mesh (convex = 4, exact = 5): geometry comes from the 3D Model
            {
                ImGui::TextDisabled(loc::T("inspector.rigidbody.hint"));
                if (attr.phys_shape == 5 && attr.phys_kind == 1) // exact + dynamic
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                       "Exact mesh is static-only; this body won't move.");
            }
            if (attr.phys_kind == 1) // Dynamic
            {
                edited |= ImGui::DragFloat(loc::TL("inspector.rigidbody.mass"), &attr.phys_mass, 0.05f, 0.001f, 10000.0f, "%.3f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat(loc::TL("inspector.rigidbody.linear_damping"),  &attr.phys_lin_damping, 0.005f, 0.0f, 1.0f, "%.3f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat(loc::TL("inspector.rigidbody.angular_damping"), &attr.phys_ang_damping, 0.005f, 0.0f, 1.0f, "%.3f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::TextUnformatted(loc::T("inspector.rigidbody.lock_rotation"));
                ImGui::SameLine();
                if (ImGui::Checkbox("X##LockRotation", &attr.phys_lock_rotation[0])) { edited = commit = true; }
                ImGui::SameLine();
                if (ImGui::Checkbox("Y##LockRotation", &attr.phys_lock_rotation[1])) { edited = commit = true; }
                ImGui::SameLine();
                if (ImGui::Checkbox("Z##LockRotation", &attr.phys_lock_rotation[2])) { edited = commit = true; }
                if (ImGui::Checkbox(loc::TL("inspector.rigidbody.gravity"), &attr.phys_gravity)) { edited = commit = true; }
                if (attr.phys_gravity)
                {
                    edited |= ImGui::DragFloat(loc::TL("inspector.rigidbody.gravity_scale"), &attr.phys_gravity_scale, 0.02f, -10.0f, 10.0f, "%.2f");
                    commit |= ImGui::IsItemDeactivatedAfterEdit();
                }
            }
            // Contact material: applies to every body (a bouncy floor needs
            // restitution too; Bullet combines the two bodies' values on contact).
            edited |= ImGui::DragFloat(loc::TL("inspector.rigidbody.bounciness"), &attr.phys_restitution, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat(loc::TL("inspector.rigidbody.friction"), &attr.phys_friction, 0.005f, 0.0f, 2.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Image")
        {
            char buf[260];
            const std::string shown = attr.image_path.empty() ? "(drag a PNG, JPG or GIF from Assets)" : attr.image_path;
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
                    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif")
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
                        state.AddLog("Image attributes support PNG, JPG and GIF files", LogLevel::Error);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            bool edited = false, commit = false;
            if (ImGui::Checkbox(loc::TL("inspector.image.stretch_to_screen"), &attr.image_stretch)) { edited = commit = true; }
            ImGui::BeginDisabled(attr.image_stretch);
            float pos[2] = { attr.image_x, attr.image_y };
            if (ImGui::DragFloat2(loc::TL("inspector.image.position"), pos, 1.0f, -4000.0f, 4000.0f, "%.0f"))
            {
                attr.image_x = pos[0]; attr.image_y = pos[1];
                edited = true;
            }
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (attr.image_lock_aspect)
            {
                const float aspect = attr.image_h > 0.0f ? attr.image_w / attr.image_h : 1.0f;
                if (ImGui::DragFloat(loc::TL("inspector.image.width"), &attr.image_w, 1.0f, 1.0f, 4000.0f, "%.0f"))
                {
                    attr.image_h = attr.image_w / aspect;
                    edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else
            {
                float size[2] = { attr.image_w, attr.image_h };
                if (ImGui::DragFloat2(loc::TL("inspector.image.size"), size, 1.0f, 1.0f, 4000.0f, "%.0f"))
                {
                    attr.image_w = size[0]; attr.image_h = size[1];
                    edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (ImGui::Checkbox(loc::TL("inspector.image.lock_aspect_ratio"), &attr.image_lock_aspect)) { edited = commit = true; }
            ImGui::EndDisabled();
            ImGui::TextDisabled(loc::T("inspector.image.hint"));
            edited |= ImGui::ColorEdit3(loc::TL("inspector.image.tint"), attr.image_tint);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat(loc::TL("inspector.image.alpha"), &attr.image_alpha, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragInt(loc::TL("inspector.image.priority"), &attr.image_priority, 0.1f, 0, 100);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextDisabled(loc::T("inspector.image.hint_2"));
            if (gifanim::IsGifPath(attr.image_path))
            {
                const char* modes = loc::T("inspector.image.play_mode_items");
                if (ImGui::Combo(loc::TL("inspector.image.play_mode"), &attr.image_play_mode, modes)) { edited = commit = true; }
                ImGui::TextDisabled(loc::T("inspector.image.hint_3"));
                ImGui::TextDisabled(loc::T("inspector.image.hint_4"));
            }
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Color")
        {
            bool edited = false, commit = false;
            if (ImGui::Checkbox(loc::TL("inspector.color.stretch_to_screen"), &attr.color_stretch)) { edited = commit = true; }
            ImGui::BeginDisabled(attr.color_stretch);
            float pos[2] = { attr.color_x, attr.color_y };
            if (ImGui::DragFloat2(loc::TL("inspector.color.position"), pos, 1.0f, -4000.0f, 4000.0f, "%.0f"))
            {
                attr.color_x = pos[0]; attr.color_y = pos[1];
                edited = true;
            }
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (attr.color_lock_aspect)
            {
                // No source image to take a ratio from, so hold the ratio the
                // block already has and let width drive height.
                const float aspect = attr.color_h > 0.0f ? attr.color_w / attr.color_h : 1.0f;
                if (ImGui::DragFloat(loc::TL("inspector.color.width"), &attr.color_w, 1.0f, 1.0f, 4000.0f, "%.0f"))
                {
                    attr.color_h = attr.color_w / aspect;
                    edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else
            {
                float size[2] = { attr.color_w, attr.color_h };
                if (ImGui::DragFloat2(loc::TL("inspector.color.size"), size, 1.0f, 1.0f, 4000.0f, "%.0f"))
                {
                    attr.color_w = size[0]; attr.color_h = size[1];
                    edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (ImGui::Checkbox(loc::TL("inspector.color.lock_aspect_ratio"), &attr.color_lock_aspect)) { edited = commit = true; }
            ImGui::EndDisabled();
            ImGui::TextDisabled(loc::T("inspector.color.hint"));
            edited |= ImGui::ColorEdit3(loc::TL("inspector.color.color"), attr.color_rgb);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat(loc::TL("inspector.color.alpha"), &attr.color_alpha, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragInt(loc::TL("inspector.color.priority"), &attr.color_priority, 0.1f, 0, 100);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextDisabled(loc::T("inspector.color.hint_2"));
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
            if (ImGui::InputTextMultiline(loc::TL("inspector.text.text"), text_buf, sizeof(text_buf), ImVec2(-1.0f, 100.0f)))
            {
                attr.text_value = text_buf;
                if (scene) scene->dirty = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) save();

            bool edited = false, commit = false;
            float position[2] = { attr.text_x, attr.text_y };
            if (ImGui::DragFloat2(loc::TL("inspector.text.position"), position, 1.0f, -4000.0f, 4000.0f, "%.0f"))
            {
                attr.text_x = position[0]; attr.text_y = position[1]; edited = true;
            }
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (attr.text_lock_aspect)
            {
                const float aspect = attr.text_h > 0.0f ? attr.text_w / attr.text_h : 1.0f;
                if (ImGui::DragFloat(loc::TL("inspector.text.width"), &attr.text_w, 1.0f, 1.0f, 4000.0f, "%.0f"))
                {
                    attr.text_h = attr.text_w / aspect; edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else
            {
                float size[2] = { attr.text_w, attr.text_h };
                if (ImGui::DragFloat2(loc::TL("inspector.text.size"), size, 1.0f, 1.0f, 4000.0f, "%.0f"))
                {
                    attr.text_w = size[0]; attr.text_h = size[1]; edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (ImGui::Checkbox(loc::TL("inspector.text.lock_aspect_ratio"), &attr.text_lock_aspect)) { edited = commit = true; }
            edited |= ImGui::DragFloat(loc::TL("inspector.text.font_size"), &attr.text_font_size, 0.5f, 1.0f, 512.0f, "%.1f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::ColorEdit3(loc::TL("inspector.text.color"), attr.text_color);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat(loc::TL("inspector.text.alpha"), &attr.text_alpha, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragInt(loc::TL("inspector.text.priority"), &attr.text_priority, 0.1f, 0, 100);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextDisabled(loc::T("inspector.text.hint"));
            ImGui::TextDisabled(loc::T("inspector.text.hint_2"));
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
            if (ImGui::Checkbox(loc::TL("inspector.video.stretch_to_screen"), &attr.video_stretch)) { edited = commit = true; }
            ImGui::BeginDisabled(attr.video_stretch);
            float pos[2] = { attr.video_x, attr.video_y };
            if (ImGui::DragFloat2(loc::TL("inspector.video.position"), pos, 1.0f, -4000.0f, 4000.0f, "%.0f"))
            {
                attr.video_x = pos[0]; attr.video_y = pos[1];
                edited = true;
            }
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (attr.video_lock_aspect)
            {
                edited |= ImGui::DragFloat(loc::TL("inspector.video.width"), &attr.video_w, 1.0f, 16.0f, 4000.0f, "%.0f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else
            {
                float size[2] = { attr.video_w, attr.video_h };
                if (ImGui::DragFloat2(loc::TL("inspector.video.size"), size, 1.0f, 16.0f, 4000.0f, "%.0f"))
                {
                    attr.video_w = size[0]; attr.video_h = size[1];
                    edited = true;
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (ImGui::Checkbox(loc::TL("inspector.video.lock_aspect_ratio"), &attr.video_lock_aspect)) { edited = commit = true; }
            ImGui::EndDisabled();
            ImGui::TextDisabled(loc::T("inspector.video.hint"));
            edited |= ImGui::ColorEdit3(loc::TL("inspector.video.tint"), attr.video_tint);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat(loc::TL("inspector.video.alpha"), &attr.video_alpha, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragInt(loc::TL("inspector.video.priority"), &attr.video_priority, 0.1f, 0, 100);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextDisabled(loc::T("inspector.video.hint_2"));
            const char* modes = loc::T("inspector.video.play_mode_items");
            if (ImGui::Combo(loc::TL("inspector.video.play_mode"), &attr.video_play_mode, modes)) { edited = commit = true; }
            edited |= ImGui::DragFloat(loc::TL("inspector.video.volume"), &attr.video_volume, 0.005f, 0.0f, 1.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::Checkbox(loc::TL("inspector.video.muted"), &attr.video_muted)) { edited = commit = true; }
            ImGui::TextDisabled(loc::T("inspector.video.hint_3"));
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
            const char* modes = loc::T("inspector.audio.play_items");
            if (ImGui::Combo(loc::TL("inspector.audio.play_mode"), &attr.audio_play, modes)) { edited = commit = true; }
            edited |= ImGui::DragFloat(loc::TL("inspector.audio.volume"), &attr.audio_volume, 0.05f, 0.0f, 20.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            edited |= ImGui::DragFloat(loc::TL("inspector.audio.pitch"), &attr.audio_pitch, 0.005f, 0.1f, 4.0f, "%.2f");
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::Checkbox(loc::TL("inspector.audio.loop"), &attr.audio_loop)) { edited = commit = true; }
            const char* audioClasses = loc::T("inspector.audio.class_items");
            if (ImGui::Combo(loc::TL("inspector.audio.class"), &attr.audio_class, audioClasses)) { edited = commit = true; }
            edited |= ImGui::DragInt(loc::TL("inspector.audio.priority"), &attr.audio_priority, 1.0f, -100, 100);
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            const char* loadModes = loc::T("inspector.audio.load_mode_items");
            if (ImGui::Combo(loc::TL("inspector.audio.load_mode"), &attr.audio_load_mode, loadModes)) { edited = commit = true; }
            if (ImGui::Checkbox(loc::TL("inspector.audio.3d_spatialize"), &attr.audio_spatial)) { edited = commit = true; }
            if (attr.audio_spatial)
            {
                edited |= ImGui::DragFloat(loc::TL("inspector.audio.min_distance"), &attr.audio_min_dist, 0.05f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat(loc::TL("inspector.audio.max_distance"), &attr.audio_max_dist, 0.1f, 0.02f, 1000.0f, "%.1f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat(loc::TL("inspector.audio.doppler_factor"), &attr.audio_doppler, 0.02f, 0.0f, 10.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            ImGui::TextDisabled(loc::T("inspector.audio.hint"));
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }
        else if (attr.type == "Trigger Volume")
        {
            ImGui::TextDisabled(loc::T("inspector.trigger.hint"));
            bool edited = false, commit = false;
            const char* shapes = loc::T("inspector.trigger.shape_items");
            if (ImGui::Combo(loc::TL("inspector.trigger.shape"), &attr.trig_shape, shapes)) { edited = commit = true; }
            if (attr.trig_shape == 0) // Box
            {
                edited |= ImGui::DragFloat3(loc::TL("inspector.trigger.half_extents"), attr.trig_size, 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else if (attr.trig_shape == 1) // Sphere
            {
                edited |= ImGui::DragFloat(loc::TL("inspector.trigger.radius"), &attr.trig_size[0], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            else // Capsule / Cylinder (Y-up): radius + half-height
            {
                edited |= ImGui::DragFloat(loc::TL("inspector.trigger.radius"), &attr.trig_size[0], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
                edited |= ImGui::DragFloat(loc::TL("inspector.trigger.half_height"), &attr.trig_size[1], 0.02f, 0.01f, 1000.0f, "%.2f");
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (edited && scene) scene->dirty = true;
            if (commit) save();
        }

        ImGui::PopID();
    }

    if (ImGui::Button(loc::TI(ICON_FA_PLUS, "inspector.add_attribute")))
        ImGui::OpenPopup("add_attribute");
    if (ImGui::BeginPopup("add_attribute"))
    {
        if (ImGui::MenuItem(loc::TL("inspector.attribute.3d_model")))
        {
            ObjectAttribute attr;
            attr.type = "3D Model";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.shader")))
        {
            ObjectAttribute attr;
            attr.type = "Shader";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.script")))
        {
            ObjectAttribute attr;
            attr.type = "Script";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.animator")))
        {
            ObjectAttribute attr;
            attr.type = "Animator";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.camera")))
        {
            ObjectAttribute attr;
            attr.type = "Camera";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.directional_light")))
        {
            ObjectAttribute attr;
            attr.type = "Directional Light";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.point_light")))
        {
            ObjectAttribute attr;
            attr.type = "Point Light";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.spot_light")))
        {
            ObjectAttribute attr;
            attr.type = "Spot Light";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.environment_light")))
        {
            ObjectAttribute attr;
            attr.type = "Environment Light";
            attr.light_intensity = 0.2f; // an ambient boost, not a blowout
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.skybox")))
        {
            ObjectAttribute attr;
            attr.type = "Skybox";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.rigid_body")))
        {
            ObjectAttribute attr;
            attr.type = "Rigid Body";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.trigger_volume")))
        {
            ObjectAttribute attr;
            attr.type = "Trigger Volume";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.image")))
        {
            ObjectAttribute attr;
            attr.type = "Image";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.color")))
        {
            ObjectAttribute attr;
            attr.type = "Color";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.text")))
        {
            ObjectAttribute attr;
            attr.type = "Text";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.video")))
        {
            ObjectAttribute attr;
            attr.type = "Video";
            obj->attributes.push_back(attr);
            save();
        }
        if (ImGui::MenuItem(loc::TL("inspector.attribute.audio")))
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
