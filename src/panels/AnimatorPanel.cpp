#include "panels/AnimatorPanel.h"

#include "anim/AnimationClip.h"
#include "imgui.h"
#include "state/EngineState.h"
#include "ui/Icons.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <unordered_set>

namespace
{
    bool InputString(const char* label, std::string& value,
                     ImGuiInputTextFlags flags = 0)
    {
        char buffer[512] = {};
        std::strncpy(buffer, value.c_str(), sizeof(buffer) - 1);
        if (!ImGui::InputText(label, buffer, sizeof(buffer), flags)) return false;
        value = buffer;
        return true;
    }

    std::string RelativeAssetPath(const EngineState& state, const char* payload,
                                  std::size_t size)
    {
        std::string value(payload, size);
        std::filesystem::path path(value);
        if (path.is_absolute())
        {
            std::error_code error;
            path = std::filesystem::relative(path, state.project_root, error);
            if (error) return value;
        }
        return path.generic_string();
    }

    bool IsModelPath(const std::string& value)
    {
        std::string extension = std::filesystem::path(value).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return (char)std::tolower(value); });
        return extension == ".fbx" || extension == ".gltf" ||
               extension == ".glb" || extension == ".obj";
    }

    bool ModelDropTarget(EngineState& state, std::string& output)
    {
        if (!ImGui::BeginDragDropTarget()) return false;
        bool changed = false;
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            const std::string path = RelativeAssetPath(state,
                (const char*)payload->Data, (std::size_t)payload->DataSize);
            if (IsModelPath(path)) { output = path; changed = true; }
            else state.AddLog("Animator expects an FBX, glTF, GLB, or OBJ model", LogLevel::Error);
        }
        ImGui::EndDragDropTarget();
        return changed;
    }

    int IndexOf(const std::vector<std::string>& values, const std::string& value)
    {
        for (std::size_t index = 0; index < values.size(); ++index)
            if (values[index] == value) return (int)index;
        return values.empty() ? -1 : 0;
    }

    bool StringCombo(const char* label, std::string& value,
                     const std::vector<std::string>& values)
    {
        if (values.empty())
        {
            ImGui::TextDisabled("%s: no options", label);
            return false;
        }
        std::vector<const char*> names;
        for (const std::string& option : values) names.push_back(option.c_str());
        int selected = IndexOf(values, value);
        if (!ImGui::Combo(label, &selected, names.data(), (int)names.size())) return false;
        value = values[(std::size_t)selected];
        return true;
    }

    std::string UniqueName(const std::string& requested,
                           const std::unordered_set<std::string>& used)
    {
        if (used.find(requested) == used.end()) return requested;
        for (unsigned suffix = 2; suffix < 10000; ++suffix)
        {
            const std::string candidate = requested + "_" + std::to_string(suffix);
            if (used.find(candidate) == used.end()) return candidate;
        }
        return requested;
    }
}

void AnimatorPanel::RefreshControllers(const EngineState& state)
{
    controller_paths_.clear();
    if (state.project_root.empty()) return;
    const std::filesystem::path directory = state.project_root / "assets" / "animators";
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error))
        if (entry.is_regular_file() && entry.path().extension() == ".anim")
            controller_paths_.push_back(entry.path());
    std::sort(controller_paths_.begin(), controller_paths_.end());
}

void AnimatorPanel::OpenController(const std::filesystem::path& path,
                                   EngineState& state)
{
    if (loaded_ && loaded_path_ == path) return;
    AnimatorController loaded;
    std::string error;
    if (!animator::LoadController(path, loaded, error))
    {
        state.AddLog("Failed to open animator controller: " + error, LogLevel::Error);
        return;
    }
    controller_ = std::move(loaded);
    loaded_path_ = path;
    loaded_ = true;
    dirty_ = false;
}

void AnimatorPanel::NewController()
{
    controller_ = AnimatorController();
    controller_.name = new_name_;
    loaded_path_.clear();
    loaded_ = true;
    dirty_ = true;
}

bool AnimatorPanel::ImportAnimationsFromModel(EngineState& state, const std::string& path)
{
    if (!IsModelPath(path))
    {
        state.AddLog("Animator expects an FBX, glTF, GLB, or OBJ model", LogLevel::Error);
        return false;
    }
    std::vector<std::string> animation_names;
    std::string error;
    if (!animation::DiscoverClips(state.project_root / path, animation_names, error))
    {
        state.AddLog("Failed to inspect model animations: " + error, LogLevel::Error);
        return false;
    }
    if (controller_.preview_model_path.empty()) controller_.preview_model_path = path;
    std::unordered_set<std::string> clip_ids, state_names;
    std::set<std::string> imported_pairs;
    for (const AnimatorClipReference& clip : controller_.clips)
    {
        clip_ids.insert(clip.id);
        imported_pairs.insert(clip.source_model_path + "#" + clip.clip_name);
    }
    for (const AnimatorStateDefinition& state_definition : controller_.states)
        state_names.insert(state_definition.name);

    unsigned imported = 0;
    for (const std::string& animation_name : animation_names)
    {
        if (imported_pairs.find(path + "#" + animation_name) != imported_pairs.end()) continue;
        const std::string clip_id = UniqueName(animation_name, clip_ids);
        clip_ids.insert(clip_id);
        controller_.clips.push_back({clip_id, path, animation_name});
        const std::string state_name = UniqueName(animation_name, state_names);
        state_names.insert(state_name);
        controller_.states.push_back({state_name, clip_id, 1.0f, true});
        if (controller_.default_state.empty()) controller_.default_state = state_name;
        ++imported;
    }
    dirty_ = dirty_ || imported > 0 || controller_.preview_model_path == path;
    state.AddLog(imported > 0
        ? "Imported " + std::to_string(imported) + " animation clips from " + path
        : "No new animation clips found in " + path);
    return imported > 0;
}

bool AnimatorPanel::SaveController(EngineState& state)
{
    if (!loaded_) return false;
    std::filesystem::path target = loaded_path_;
    if (target.empty())
    {
        if (controller_.name.empty())
        {
            state.AddLog("Animator controller name cannot be empty", LogLevel::Error);
            return false;
        }
        target = state.project_root / "assets" / "animators" /
                 (controller_.name + ".anim");
    }
    std::error_code fs_error;
    std::filesystem::create_directories(target.parent_path(), fs_error);
    std::string error;
    if (!animator::SaveController(target, controller_, error))
    {
        state.AddLog("Failed to save animator controller: " + error, LogLevel::Error);
        return false;
    }
    loaded_path_ = target;
    dirty_ = false;
    preview_.InvalidateClips();
    state.saved_animator_path = std::filesystem::relative(target, state.project_root, fs_error);
    if (fs_error) state.saved_animator_path = target;
    state.AddLog("Saved animator controller: " + target.filename().string());
    return true;
}

void AnimatorPanel::RenderClips(EngineState& state)
{
    ImGui::TextUnformatted("Imported Clips");
    ImGui::Separator();
    if (ImGui::Button("+ Add Clip"))
    {
        AnimatorClipReference clip;
        clip.id = "Clip_" + std::to_string(controller_.clips.size() + 1);
        clip.clip_name = clip.id;
        controller_.clips.push_back(clip);
        dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled();
    ImGui::Button("Import Selected Model");
    ImGui::EndDisabled();

    ImGui::Button("Drop Model (.fbx/.gltf/.glb) Here", ImVec2(-1.0f, 0.0f));
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            const std::string path = RelativeAssetPath(state, (const char*)payload->Data,
                                                       (std::size_t)payload->DataSize);
            if (IsModelPath(path))
            {
                ImportAnimationsFromModel(state, path);
            }
        }
        ImGui::EndDragDropTarget();
    }
    int remove = -1;
    for (std::size_t index = 0; index < controller_.clips.size(); ++index)
    {
        AnimatorClipReference& clip = controller_.clips[index];
        ImGui::PushID((int)index);
        const std::string title = clip.id.empty() ? "Unnamed clip" : clip.id;
        if (ImGui::TreeNodeEx("clip", 0, "%s", title.c_str()))
        {
            dirty_ |= InputString("Clip Id", clip.id);
            dirty_ |= InputString("Source Model", clip.source_model_path);
            dirty_ |= ModelDropTarget(state, clip.source_model_path);
            dirty_ |= InputString("Clip Name", clip.clip_name);
            if (ImGui::Button("Remove Clip")) remove = (int)index;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove >= 0)
    {
        controller_.clips.erase(controller_.clips.begin() + remove);
        dirty_ = true;
    }
}

void AnimatorPanel::RenderStates()
{
    if (!ImGui::CollapsingHeader("States", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::Button("+ Add State"))
    {
        AnimatorStateDefinition state;
        state.name = "State_" + std::to_string(controller_.states.size() + 1);
        if (!controller_.clips.empty()) state.clip_id = controller_.clips.front().id;
        controller_.states.push_back(state);
        if (controller_.default_state.empty()) controller_.default_state = state.name;
        dirty_ = true;
    }
    std::vector<std::string> state_names, clip_ids;
    for (const AnimatorStateDefinition& state : controller_.states) state_names.push_back(state.name);
    for (const AnimatorClipReference& clip : controller_.clips) clip_ids.push_back(clip.id);
    if (!state_names.empty()) dirty_ |= StringCombo("Default State", controller_.default_state, state_names);

    int remove = -1;
    for (std::size_t index = 0; index < controller_.states.size(); ++index)
    {
        AnimatorStateDefinition& state = controller_.states[index];
        ImGui::PushID((int)index);
        const std::string title = state.name.empty() ? "Unnamed state" : state.name;
        if (ImGui::TreeNodeEx("state", 0, "%s", title.c_str()))
        {
            const std::string old_name = state.name;
            if (InputString("Name", state.name))
            {
                if (controller_.default_state == old_name) controller_.default_state = state.name;
                for (AnimatorTransitionDefinition& transition : controller_.transitions)
                {
                    if (transition.from_state == old_name) transition.from_state = state.name;
                    if (transition.to_state == old_name) transition.to_state = state.name;
                }
                dirty_ = true;
            }
            dirty_ |= StringCombo("Clip", state.clip_id, clip_ids);
            if (ImGui::DragFloat("Playback Speed", &state.playback_speed, 0.01f, 0.01f, 8.0f, "%.2f"))
            { state.playback_speed = (std::max)(0.01f, state.playback_speed); dirty_ = true; }
            dirty_ |= ImGui::Checkbox("Loop", &state.loop);
            if (controller_.default_state == state.name) ImGui::TextDisabled("Default state");
            else if (ImGui::Button("Set As Default")) { controller_.default_state = state.name; dirty_ = true; }
            ImGui::SameLine();
            if (ImGui::Button("Remove")) remove = (int)index;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove >= 0)
    {
        const std::string name = controller_.states[(std::size_t)remove].name;
        controller_.states.erase(controller_.states.begin() + remove);
        controller_.transitions.erase(std::remove_if(controller_.transitions.begin(),
            controller_.transitions.end(), [&name](const AnimatorTransitionDefinition& transition)
            { return transition.from_state == name || transition.to_state == name; }),
            controller_.transitions.end());
        controller_.default_state = controller_.states.empty() ? std::string() : controller_.states.front().name;
        dirty_ = true;
    }
}

void AnimatorPanel::RenderTransitions()
{
    if (!ImGui::CollapsingHeader("Transitions", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::Button("+ Add Transition") && !controller_.states.empty())
    {
        AnimatorTransitionDefinition transition;
        transition.from_state = controller_.states.front().name;
        transition.to_state = controller_.states.size() > 1
            ? controller_.states[1].name : controller_.states.front().name;
        controller_.transitions.push_back(transition);
        dirty_ = true;
    }
    std::vector<std::string> state_names;
    for (const AnimatorStateDefinition& state : controller_.states) state_names.push_back(state.name);
    int remove = -1;
    for (std::size_t index = 0; index < controller_.transitions.size(); ++index)
    {
        AnimatorTransitionDefinition& transition = controller_.transitions[index];
        ImGui::PushID((int)index);
        const std::string title = transition.from_state + " -> " + transition.to_state;
        if (ImGui::TreeNodeEx("transition", 0, "%s", title.c_str()))
        {
            dirty_ |= StringCombo("From", transition.from_state, state_names);
            dirty_ |= StringCombo("To", transition.to_state, state_names);
            dirty_ |= InputString("Condition", transition.condition);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Examples: grounded, !grounded, speed > 0.5, moving == true");
            if (ImGui::DragFloat("Blend Duration", &transition.blend_duration, 0.01f, 0.0f, 10.0f, "%.2f"))
            { transition.blend_duration = (std::max)(0.0f, transition.blend_duration); dirty_ = true; }
            dirty_ |= ImGui::Checkbox("Has Exit Time", &transition.has_exit_time);
            if (transition.has_exit_time && ImGui::DragFloat("Exit Time", &transition.exit_time, 0.01f, 0.0f, 1000.0f, "%.2f s"))
            { transition.exit_time = (std::max)(0.0f, transition.exit_time); dirty_ = true; }
            if (ImGui::Button("Remove Transition")) remove = (int)index;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove >= 0)
    {
        controller_.transitions.erase(controller_.transitions.begin() + remove);
        dirty_ = true;
    }
}

void AnimatorPanel::RenderBoneModifiers(EngineState& state)
{
    ImGui::TextUnformatted("Bone Modifiers");
    ImGui::Separator();
    const std::string selected_bone = preview_.SelectedBoneName();
    bool already_added = false;
    for (const AnimatorBoneModifier& modifier : controller_.bone_modifiers)
        if (modifier.bone_name == selected_bone) already_added = true;
    const bool can_add = !selected_bone.empty() && !already_added;
    if (!can_add) ImGui::BeginDisabled();
    if (ImGui::Button("+ Add Selected Bone"))
    {
        AnimatorBoneModifier modifier;
        modifier.bone_name = selected_bone;
        controller_.bone_modifiers.push_back(modifier);
        dirty_ = true;
    }
    if (!can_add) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(selected_bone.empty() ? "(click a bone in the preview)" : "(%s)", selected_bone.c_str());
    int remove = -1;
    for (std::size_t index = 0; index < controller_.bone_modifiers.size(); ++index)
    {
        AnimatorBoneModifier& modifier = controller_.bone_modifiers[index];
        ImGui::PushID((int)index);
        const std::string title = modifier.bone_name.empty() ? "Modifier" : modifier.bone_name;
        if (ImGui::TreeNode(title.c_str()))
        {
            dirty_ |= InputString("Bone", modifier.bone_name);

            int type_index = (int)modifier.type;
            const char* type_names[] = {"Physics", "Collision"};
            if (ImGui::Combo("Type", &type_index, type_names, IM_ARRAYSIZE(type_names)))
            {
                const AnimatorBoneModifierType new_type = (AnimatorBoneModifierType)type_index;
                if (new_type == AnimatorBoneModifierType::Collision &&
                    modifier.type != AnimatorBoneModifierType::Collision)
                    preview_.ComputeBoneFitBox(modifier.bone_name, modifier.box_half_extents,
                                               modifier.box_center);
                modifier.type = new_type;
                dirty_ = true;
            }

            if (type_index == 0)
            {
                int preset = 0;
                std::size_t preset_count = 0;
                const BonePhysicsPreset* presets = bone_modifiers::PhysicsPresets(preset_count);
                std::vector<const char*> preset_names;
                for (std::size_t preset_index = 0; preset_index < preset_count; ++preset_index)
                    preset_names.push_back(presets[preset_index].name);
                if (ImGui::Combo("Preset", &preset, preset_names.data(), (int)preset_names.size()) && preset > 0)
                {
                    bone_modifiers::ApplyPhysicsPreset(modifier, (std::size_t)preset);
                    dirty_ = true;
                }

                struct Field { const char* label; float* value; float speed; float minimum; float maximum; const char* format; };
                Field fields[] = {
                    {"Strength", &modifier.strength, .01f, 0, 2, "%.2f"},
                    {"Stiffness", &modifier.stiffness, .01f, 0, 1, "%.2f"},
                    {"Damping", &modifier.damping, .01f, 0, 3, "%.2f"},
                    {"Mass", &modifier.mass, .01f, .001f, 100, "%.3f"},
                    {"Drag", &modifier.drag, .005f, 0, 1, "%.3f"},
                    {"Gravity Scale", &modifier.gravity_scale, .01f, 0, 5, "%.2f"},
                };
                for (Field& field : fields)
                    if (ImGui::DragFloat(field.label, field.value, field.speed, field.minimum, field.maximum, field.format))
                    { *field.value = (std::clamp)(*field.value, field.minimum, field.maximum); dirty_ = true; }

                if (ImGui::TreeNode("Advanced"))
                {
                    dirty_ |= ImGui::DragFloat3("Gravity Dir", modifier.gravity_dir.data(), .01f, -1, 1, "%.2f");
                    dirty_ |= ImGui::DragFloat("Angle Limit", &modifier.angle_limit_deg, .5f, 0, 180, "%.1f deg");
                    dirty_ |= ImGui::DragFloat("Radius", &modifier.radius, .001f, 0, 1, "%.3f");
                    dirty_ |= ImGui::Checkbox("Affects Children", &modifier.affects_children);
                    ImGui::TreePop();
                }
            }
            else
            {
                if (ImGui::Button("Fit to Bone"))
                    dirty_ |= preview_.ComputeBoneFitBox(modifier.bone_name,
                        modifier.box_half_extents, modifier.box_center);
                dirty_ |= ImGui::DragFloat3("Half Extents", modifier.box_half_extents.data(), .005f, 0, 100, "%.3f");
                dirty_ |= ImGui::DragFloat3("Center Offset", modifier.box_center.data(), .005f, -100, 100, "%.3f");
            }

            if (ImGui::Button("Remove Modifier")) remove = (int)index;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove >= 0)
    {
        controller_.bone_modifiers.erase(controller_.bone_modifiers.begin() + remove);
        dirty_ = true;
    }
}

void AnimatorPanel::RenderPreviewViewport(EngineState& state)
{
    const ImVec2 viewport_min = ImGui::GetWindowPos();
    const ImVec2 viewport_max(viewport_min.x + ImGui::GetWindowSize().x,
                              viewport_min.y + ImGui::GetWindowSize().y);
    const float toolbar_height = ImGui::GetFrameHeight() * 2.0f +
        ImGui::GetStyle().ItemSpacing.y * 3.0f;
    const ImVec2 canvas_max(viewport_max.x,
        (std::max)(viewport_min.y + 1.0f, viewport_max.y - toolbar_height));

    // Resolve the active clip (source model + name) from the toolbar selection.
    if (preview_clip_index_ >= (int)controller_.clips.size())
        preview_clip_index_ = 0;
    std::string clip_source, clip_name;
    if (preview_clip_index_ >= 0 && preview_clip_index_ < (int)controller_.clips.size())
    {
        clip_source = controller_.clips[preview_clip_index_].source_model_path;
        clip_name   = controller_.clips[preview_clip_index_].clip_name;
    }

    // Push display/playback state, then render the offscreen image + rig overlay.
    preview_.SetShowSkeleton(preview_skeleton_);
    preview_.SetShowMesh(preview_mesh_);
    preview_.SetShowTexture(preview_texture_);
    preview_.SetPlaying(preview_playing_);
    preview_.SetBoneModifiers(controller_.bone_modifiers);
    preview_.RenderUi(state, controller_.preview_model_path, clip_source, clip_name,
                      viewport_min, canvas_max);

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            const std::string path = RelativeAssetPath(state, (const char*)payload->Data,
                                                       (std::size_t)payload->DataSize);
            if (IsModelPath(path))
            {
                controller_.preview_model_path = path;
                dirty_ = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SetCursorScreenPos(ImVec2(viewport_min.x + 6.0f,
        canvas_max.y + ImGui::GetStyle().ItemSpacing.y));
    if (controller_.preview_model_path.empty()) ImGui::BeginDisabled();
    if (ImGui::Button(preview_playing_ ? ICON_FA_PAUSE : ICON_FA_PLAY))
        preview_playing_ = !preview_playing_;
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE)) { preview_time_ = 0.0f; preview_.SetTimeNormalized(0.0f); }
    ImGui::SameLine();

    std::vector<const char*> clip_names;
    for (const AnimatorClipReference& clip : controller_.clips)
        clip_names.push_back(clip.clip_name.empty() ? clip.id.c_str() : clip.clip_name.c_str());
    if (preview_clip_index_ >= (int)clip_names.size()) preview_clip_index_ = 0;
    ImGui::SetNextItemWidth(160.0f);
    if (!clip_names.empty())
        ImGui::Combo("##AnimatorPreviewClip", &preview_clip_index_, clip_names.data(), (int)clip_names.size());
    else
        ImGui::TextDisabled("(no clips)");

    ImGui::SameLine();
    ImGui::Checkbox("Skeleton", &preview_skeleton_);
    ImGui::SameLine();
    ImGui::Checkbox("Mesh", &preview_mesh_);
    ImGui::SameLine();
    ImGui::Checkbox("Texture", &preview_texture_);
    if (!preview_.SelectedBoneName().empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("Bone: %s", preview_.SelectedBoneName().c_str());
    }
    preview_time_ = preview_.TimeNormalized();
    ImGui::SetNextItemWidth((std::max)(80.0f, ImGui::GetContentRegionAvail().x - 8.0f));
    if (ImGui::SliderFloat("##AnimatorPreviewTime", &preview_time_, 0.0f, 1.0f, "%.2f"))
        preview_.SetTimeNormalized(preview_time_);
    if (controller_.preview_model_path.empty()) ImGui::EndDisabled();
}

void AnimatorPanel::RenderControllerEditor(EngineState& state)
{
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float total_width = ImGui::GetContentRegionAvail().x;
    const float available_height = ImGui::GetContentRegionAvail().y;
    const float library_height = 220.0f;
    const float top_height = available_height - library_height - spacing;
    const float half_width = (total_width - spacing) * 0.5f;

    ImGui::BeginChild("AnimatorViewport", ImVec2(half_width, top_height), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    RenderPreviewViewport(state);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, spacing);
    ImGui::BeginChild("NodeGraphCanvas", ImVec2(0.0f, top_height), true);
    dirty_ |= InputString("Controller Name", controller_.name);
    if (ImGui::BeginTabBar("AnimatorEditorTabs"))
    {
        if (ImGui::BeginTabItem("General"))
        {
            RenderStates();
            RenderTransitions();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Face"))
        {
            ImGui::TextDisabled("Face animation is outside the 360 runtime scope.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::BeginChild("BottomTablePanel", ImVec2(half_width, library_height), true);
    RenderBoneModifiers(state);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, spacing);
    ImGui::BeginChild("NodeLibraryPanel", ImVec2(0.0f, library_height), true);
    RenderClips(state);
    ImGui::EndChild();
}

void AnimatorPanel::Render(EngineState& state)
{
    if (!state.show_animator_panel) return;
    if (!ImGui::Begin("Animator", &state.show_animator_panel)) { ImGui::End(); return; }
    if (!state.HasProject())
    {
        ImGui::TextDisabled("Open a project to edit animator controllers.");
        ImGui::End();
        return;
    }

    RefreshControllers(state);
    if (!state.open_animator_path.empty())
    {
        OpenController(state.open_animator_path, state);
        state.open_animator_path.clear();
    }

    std::vector<std::string> names;
    names.push_back("New +");
    int selected = loaded_path_.empty() ? 0 : -1;
    for (std::size_t index = 0; index < controller_paths_.size(); ++index)
    {
        names.push_back(controller_paths_[index].stem().string());
        if (controller_paths_[index] == loaded_path_) selected = (int)index + 1;
    }
    std::vector<const char*> labels;
    for (const std::string& name : names) labels.push_back(name.c_str());
    if (selected < 0) selected = 0;

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float save_width = ImGui::CalcTextSize(ICON_FA_FLOPPY_DISK).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float build_width = ImGui::CalcTextSize(ICON_FA_HAMMER).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float play_width = ImGui::CalcTextSize(ICON_FA_PLAY).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float toolbar_width = save_width + build_width + play_width + spacing * 2.0f;
    const float row_start_x = ImGui::GetCursorPosX();
    const float row_width = ImGui::GetContentRegionAvail().x;
    const float status_width = dirty_ ? spacing + ImGui::CalcTextSize("Unsaved changes").x : 0.0f;
    const float fields_width = 180.0f + spacing + 200.0f + status_width;
    const bool toolbar_on_same_row = row_width >= fields_width + spacing + toolbar_width;

    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo("##AnimatorGraphSelector", &selected, labels.data(), (int)labels.size()))
    {
        if (selected == 0) NewController();
        else OpenController(controller_paths_[(std::size_t)selected - 1], state);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if (selected != 0) ImGui::BeginDisabled();
    if (ImGui::InputTextWithHint("##AnimatorGraphName", "", new_name_, sizeof(new_name_)))
    {
        controller_.name = new_name_;
        dirty_ = true;
    }
    if (selected != 0) ImGui::EndDisabled();

    if (dirty_) { ImGui::SameLine(); ImGui::TextDisabled("Unsaved changes"); }
    if (toolbar_on_same_row)
    {
        ImGui::SameLine();
        ImGui::SetCursorPosX(row_start_x + row_width - toolbar_width);
    }
    else
    {
        ImGui::SetCursorPosX(row_start_x + (std::max)(0.0f, row_width - toolbar_width));
    }
    if (ImGui::Button(ICON_FA_FLOPPY_DISK)) SaveController(state);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_HAMMER)) state.build_run_requested = true;
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLAY)) state.physics_playing = true;

    if (!loaded_) NewController();
    ImGui::Separator();
    RenderControllerEditor(state);
    ImGui::End();
}