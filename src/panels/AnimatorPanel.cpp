#include "panels/AnimatorPanel.h"
#include "loc/Loc.h"

#include "anim/AnimationClip.h"
#include "anim/FaceShapes.h"
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
            ImGui::TextDisabled(loc::T("animator.no_options"), label);
            return false;
        }
        std::vector<const char*> names;
        for (const std::string& option : values) names.push_back(option.c_str());
        int selected = IndexOf(values, value);
        if (!ImGui::Combo(label, &selected, names.data(), (int)names.size())) return false;
        value = values[(std::size_t)selected];
        return true;
    }

    // Enumerated once; adapters do not come and go inside a session often
    // enough to pay for this every frame.
    const std::vector<std::string>& LocalAddressList()
    {
        static std::vector<std::string> addresses = livelink::LocalAddresses();
        return addresses;
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
    ImGui::TextUnformatted(loc::T("animator.clips.imported_clips"));
    ImGui::Separator();
    if (ImGui::Button(loc::TL("animator.clips.add_clip")))
    {
        AnimatorClipReference clip;
        clip.id = "Clip_" + std::to_string(controller_.clips.size() + 1);
        clip.clip_name = clip.id;
        controller_.clips.push_back(clip);
        dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled();
    ImGui::Button(loc::TL("animator.clips.import_selected_model"));
    ImGui::EndDisabled();

    ImGui::Button(loc::TL("animator.clips.drop_model_fbx_gltf_glb_here"), ImVec2(-1.0f, 0.0f));
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
        if (ImGui::TreeNodeEx(loc::TL("animator.clips.clip"), 0, "%s", title.c_str()))
        {
            dirty_ |= InputString("Clip Id", clip.id);
            dirty_ |= InputString("Source Model", clip.source_model_path);
            dirty_ |= ModelDropTarget(state, clip.source_model_path);
            dirty_ |= InputString("Clip Name", clip.clip_name);
            if (ImGui::Button(loc::TL("animator.clips.remove_clip"))) remove = (int)index;
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
    if (!ImGui::CollapsingHeader(loc::TL("animator.states.states"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::Button(loc::TL("animator.states.add_state")))
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
        if (ImGui::TreeNodeEx(loc::TL("animator.states.state"), 0, "%s", title.c_str()))
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
            if (ImGui::DragFloat(loc::TL("animator.states.playback_speed"), &state.playback_speed, 0.01f, 0.01f, 8.0f, "%.2f"))
            { state.playback_speed = (std::max)(0.01f, state.playback_speed); dirty_ = true; }
            dirty_ |= ImGui::Checkbox(loc::TL("animator.states.loop"), &state.loop);
            if (controller_.default_state == state.name) ImGui::TextDisabled(loc::T("animator.states.default_state"));
            else if (ImGui::Button(loc::TL("animator.states.set_as_default"))) { controller_.default_state = state.name; dirty_ = true; }
            ImGui::SameLine();
            if (ImGui::Button(loc::TL("animator.states.remove"))) remove = (int)index;
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
    if (!ImGui::CollapsingHeader(loc::TL("animator.transitions.transitions"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::Button(loc::TL("animator.transitions.add_transition")) && !controller_.states.empty())
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
        if (ImGui::TreeNodeEx(loc::TL("animator.transitions.transition"), 0, "%s", title.c_str()))
        {
            dirty_ |= StringCombo("From", transition.from_state, state_names);
            dirty_ |= StringCombo("To", transition.to_state, state_names);
            dirty_ |= InputString("Condition", transition.condition);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(loc::T("animator.transitions.hint"));
            if (ImGui::DragFloat(loc::TL("animator.transitions.blend_duration"), &transition.blend_duration, 0.01f, 0.0f, 10.0f, "%.2f"))
            { transition.blend_duration = (std::max)(0.0f, transition.blend_duration); dirty_ = true; }
            dirty_ |= ImGui::Checkbox(loc::TL("animator.transitions.has_exit_time"), &transition.has_exit_time);
            if (transition.has_exit_time && ImGui::DragFloat(loc::TL("animator.transitions.exit_time"), &transition.exit_time, 0.01f, 0.0f, 1000.0f, "%.2f s"))
            { transition.exit_time = (std::max)(0.0f, transition.exit_time); dirty_ = true; }
            if (ImGui::Button(loc::TL("animator.transitions.remove_transition"))) remove = (int)index;
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
    ImGui::TextUnformatted(loc::T("animator.bonemod.bone_modifiers"));
    ImGui::Separator();
    const std::string selected_bone = preview_.SelectedBoneName();
    bool already_added = false;
    for (const AnimatorBoneModifier& modifier : controller_.bone_modifiers)
        if (modifier.bone_name == selected_bone) already_added = true;
    const bool can_add = !selected_bone.empty() && !already_added;
    if (!can_add) ImGui::BeginDisabled();
    if (ImGui::Button(loc::TL("animator.bonemod.add_selected_bone")))
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
            const char* type_names[] = { loc::T("animator.bonemod.type_physics"),
                                         loc::T("animator.bonemod.type_collision") };
            if (ImGui::Combo(loc::TL("animator.bonemod.type"), &type_index, type_names, IM_ARRAYSIZE(type_names)))
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
                if (ImGui::Combo(loc::TL("animator.bonemod.preset"), &preset, preset_names.data(), (int)preset_names.size()) && preset > 0)
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

                if (ImGui::TreeNode(loc::TL("animator.bonemod.advanced")))
                {
                    dirty_ |= ImGui::DragFloat3(loc::TL("animator.bonemod.gravity_dir"), modifier.gravity_dir.data(), .01f, -1, 1, "%.2f");
                    dirty_ |= ImGui::DragFloat(loc::TL("animator.bonemod.angle_limit"), &modifier.angle_limit_deg, .5f, 0, 180, "%.1f deg");
                    dirty_ |= ImGui::DragFloat(loc::TL("animator.bonemod.radius"), &modifier.radius, .001f, 0, 1, "%.3f");
                    dirty_ |= ImGui::Checkbox(loc::TL("animator.bonemod.affects_children"), &modifier.affects_children);
                    ImGui::TreePop();
                }
            }
            else
            {
                if (ImGui::Button(loc::TL("animator.bonemod.fit_to_bone")))
                    dirty_ |= preview_.ComputeBoneFitBox(modifier.bone_name,
                        modifier.box_half_extents, modifier.box_center);
                dirty_ |= ImGui::DragFloat3(loc::TL("animator.bonemod.half_extents"), modifier.box_half_extents.data(), .005f, 0, 100, "%.3f");
                dirty_ |= ImGui::DragFloat3(loc::TL("animator.bonemod.center_offset"), modifier.box_center.data(), .005f, -100, 100, "%.3f");
            }

            if (ImGui::Button(loc::TL("animator.bonemod.remove_modifier"))) remove = (int)index;
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
    // The toolbar lists body clips first, then recorded face clips; selecting a
    // face clip leaves the body clip unset, so the skeleton holds its pose.
    const int body_clip_count = (int)controller_.clips.size();
    if (preview_clip_index_ >= body_clip_count + (int)controller_.face.clips.size())
        preview_clip_index_ = 0;
    std::string clip_source, clip_name;
    if (preview_clip_index_ >= 0 && preview_clip_index_ < body_clip_count)
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

    // Built in full before any c_str() is taken: the labels must not move.
    std::vector<std::string> face_labels;
    for (const std::string& clip : controller_.face.clips)
        face_labels.push_back(std::filesystem::path(clip).stem().string() + "  (face)");

    std::vector<const char*> clip_names;
    for (const AnimatorClipReference& clip : controller_.clips)
        clip_names.push_back(clip.clip_name.empty() ? clip.id.c_str() : clip.clip_name.c_str());
    for (const std::string& label : face_labels)
        clip_names.push_back(label.c_str());

    if (preview_clip_index_ >= (int)clip_names.size()) preview_clip_index_ = 0;
    ImGui::SetNextItemWidth(160.0f);
    if (!clip_names.empty())
    {
        if (ImGui::Combo("##AnimatorPreviewClip", &preview_clip_index_,
                         clip_names.data(), (int)clip_names.size()))
        {
            if (preview_clip_index_ >= body_clip_count)
                PlayFaceClipPreview(state,
                    controller_.face.clips[preview_clip_index_ - body_clip_count]);
            else
                StopFaceClipPreview();
        }
    }
    else
        ImGui::TextDisabled(loc::T("animator.preview.no_clips"));
    if (face_clip_playing_)
    {
        ImGui::SameLine();
        ImGui::Text(loc::T("animator.preview.s"), face_clip_time_, face_clip_view_.Duration());
    }

    ImGui::SameLine();
    ImGui::Checkbox(loc::TL("animator.preview.skeleton"), &preview_skeleton_);
    ImGui::SameLine();
    ImGui::Checkbox(loc::TL("animator.preview.mesh"), &preview_mesh_);
    ImGui::SameLine();
    ImGui::Checkbox(loc::TL("animator.preview.texture"), &preview_texture_);
    if (!preview_.SelectedBoneName().empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled(loc::T("animator.preview.bone"), preview_.SelectedBoneName().c_str());
    }
    preview_time_ = preview_.TimeNormalized();
    ImGui::SetNextItemWidth((std::max)(80.0f, ImGui::GetContentRegionAvail().x - 8.0f));
    if (ImGui::SliderFloat("##AnimatorPreviewTime", &preview_time_, 0.0f, 1.0f, "%.2f"))
        preview_.SetTimeNormalized(preview_time_);
    if (controller_.preview_model_path.empty()) ImGui::EndDisabled();
}

// --- Face tab: expression poses, previewed live on the previewed character.

const float* AnimatorPanel::LiveWeights()
{
    return (live_link_preview_ && have_live_weights_) ? live_weights_ : nullptr;
}

void AnimatorPanel::TickTracking(EngineState& state)
{
    const double now = ImGui::GetTime();
    if (live_link_.IsOpen())
    {
        live_link_.Poll(now);
        // Only puppet the face while packets are actually arriving. The last
        // received frame otherwise sticks forever and silently outranks a
        // playing clip, which reads as the clip not animating at all.
        have_live_weights_ = live_link_.Weights(live_weights_, face::kShapeCount) &&
                             live_link_.Receiving(now);

        if (calibrating_ && now >= calibrate_until_)
        {
            calibrating_ = false;
            const unsigned int samples = live_link_.Calibration().SampleCount();
            if (live_link_.Calibration().End())
                state.AddLog("Neutral calibrated on " + std::to_string(samples) + " frames");
            else
                state.AddLog("Calibration got no frames - is anything streaming?",
                             LogLevel::Warning);
        }
    }
    else
    {
        have_live_weights_ = false;
    }

    // Sampling lives here too, so a take survives switching tabs mid-record.
    if (recorder_.Recording() && have_live_weights_)
        recorder_.Sample(now, live_weights_, face::kShapeCount);

    TickFaceClipPreview(ImGui::GetIO().DeltaTime);
}

void AnimatorPanel::RenderLiveLink(EngineState& state)
{
    ImGui::SeparatorText(loc::T("animator.livelink.live_link_face"));

    const double now = ImGui::GetTime();
    const bool receiving = live_link_.IsOpen() && live_link_.Receiving(now);
    if (!live_link_.IsOpen())
    {
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt(loc::TL("animator.livelink.port"), &live_link_port_, 0);
        ImGui::SameLine();
        if (ImGui::Button(loc::TL("animator.livelink.connect")))
        {
            if (!live_link_.Open((unsigned short)live_link_port_))
                state.AddLog("Live Link: " + live_link_.Error(), LogLevel::Error);
        }
        if (live_link_.Error().empty())
            ImGui::TextDisabled(loc::T("animator.livelink.not_listening"));
        else
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                               live_link_.Error().c_str());
    }
    else
    {
        if (ImGui::Button(loc::TL("animator.livelink.disconnect"))) { live_link_.Close(); have_live_weights_ = false; }
        ImGui::SameLine();
        if (receiving)
            ImGui::Text(loc::T("animator.livelink.s_from"), live_link_.Subject().c_str(),
                        live_link_.PacketsPerSecond(), live_link_.LastSender().c_str());
        else if (live_link_.RejectedCount() > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                "%u packets from %s rejected (%u bytes, not Live Link format)",
                live_link_.RejectedCount(), live_link_.LastSender().c_str(),
                live_link_.LastRejectSize());
        else if (live_link_.DatagramCount() > 0)
            ImGui::TextDisabled(loc::T("animator.livelink.hint"),
                                live_link_.LastSender().c_str());
        else
            ImGui::TextDisabled(loc::T("animator.livelink.hint_2"),
                                live_link_port_);

        // Nothing at all means the phone is aimed somewhere else, so say where
        // it should be aimed instead of leaving it to be guessed.
        if (live_link_.DatagramCount() == 0)
        {
            const std::vector<std::string>& addresses = LocalAddressList();
            if (addresses.empty())
                ImGui::TextDisabled(loc::T("animator.livelink.hint_3"));
            else
                for (const std::string& address : addresses)
                    ImGui::TextDisabled(loc::T("animator.livelink.point_the_phone_at"),
                                        address.c_str(), live_link_port_);
        }
    }

    if (!receiving) ImGui::BeginDisabled();
    if (calibrating_)
    {
        ImGui::Text(loc::T("animator.livelink.hold_still_s"), calibrate_until_ - now);
    }
    else if (ImGui::Button(loc::TL("animator.livelink.calibrate_neutral")))
    {
        live_link_.Calibration().Begin();
        calibrating_ = true;
        calibrate_until_ = now + 1.5;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(live_link_.Calibration().Valid() ? "(calibrated)" : "(not calibrated)");

    ImGui::Checkbox(loc::TL("animator.livelink.preview"), &live_link_preview_);
    ImGui::SameLine();
    ImGui::Checkbox(loc::TL("animator.livelink.on_selected_object"), &live_link_on_character_);
    if (!receiving) ImGui::EndDisabled();
}

bool AnimatorPanel::PlayFaceClipPreview(EngineState& state, const std::string& relative_path)
{
    StopFaceClipPreview();

    FaceClipAsset asset;
    std::string error;
    if (!faceclip::Load(state.project_root / relative_path, asset, error) ||
        !faceclip::CookBE(asset, face_clip_bytes_, error))
    {
        state.AddLog("Face clip '" + relative_path + "': " + error, LogLevel::Error);
        face_clip_bytes_.clear();
        return false;
    }
    if (!face::ParseClip(face_clip_bytes_.data(), (unsigned)face_clip_bytes_.size(),
                         face_clip_view_))
    {
        state.AddLog("Face clip '" + relative_path + "' did not parse", LogLevel::Error);
        face_clip_bytes_.clear();
        return false;
    }

    face_clip_path_ = relative_path;
    face_clip_audio_abs_.clear();
    if (face_clip_view_.audioPathBytes > 0)
        face_clip_audio_abs_ = (state.project_root /
            std::string(face_clip_view_.audioPath,
                        face_clip_view_.audioPathBytes)).string();
    face_clip_playing_ = true;
    face_clip_time_ = 0.0f;
    return true;
}

void AnimatorPanel::StopFaceClipPreview()
{
    face_clip_playing_ = false;
    face_clip_time_ = 0.0f;
    face_clip_path_.clear();
    face_clip_audio_abs_.clear();
    face_clip_bytes_.clear();
    // A want list of nothing is a stop, so this releases the voice.
    face_clip_audio_.Update(nullptr, 0, 0.0f, nullptr);
}

void AnimatorPanel::TickFaceClipPreview(float dt)
{
    if (!face_clip_playing_)
    {
        face_clip_audio_.Update(nullptr, 0, dt, nullptr);
        return;
    }

    // The clip carries its own audio path; without one the clock is just dt.
    const std::string key = "facepreview";
    float audio_seconds = -1.0f;
    if (!face_clip_audio_abs_.empty())
    {
        aud::Want want;
        want.key  = key;
        want.path = face_clip_audio_abs_;
        face_clip_audio_.Update(&want, 1, dt, nullptr);
        audio_seconds = face_clip_audio_.PlaybackSeconds(key);
    }
    else
    {
        face_clip_audio_.Update(nullptr, 0, dt, nullptr);
    }

    if (audio_seconds >= 0.0f)
        face_clip_time_ = audio_seconds;
    else
        face_clip_time_ += dt;

    unsigned char touched[face::kShapeCount] = {};
    memset(face_clip_weights_, 0, sizeof(face_clip_weights_));
    face::SampleClip(face_clip_view_, face_clip_time_, false,
                     face_clip_weights_, face::kShapeCount, touched);

    if (face_clip_time_ >= face_clip_view_.Duration())
        StopFaceClipPreview();
}

void AnimatorPanel::UpdateFacePreview()
{
    // Tracking, when it is running, outranks everything; a playing clip then
    // outranks a pose being scrubbed.
    if (const float* live = LiveWeights())
    {
        preview_.SetFaceWeights(live);
        return;
    }
    if (face_clip_playing_)
    {
        preview_.SetFaceWeights(face_clip_weights_);
        return;
    }
    if (face_pose_preview_ < 0 ||
        face_pose_preview_ >= (int)controller_.face.poses.size())
    {
        preview_.SetFaceWeights(nullptr);
        return;
    }
    float weights[face::kShapeCount] = {};
    for (const auto& [target, weight] : controller_.face.poses[face_pose_preview_].weights)
    {
        const int shape = face::ShapeIndex(target.c_str());
        if (shape != face::kShapeNone)
            weights[shape] = std::clamp(weight, 0.0f, 1.0f);
    }
    preview_.SetFaceWeights(weights);
}


void AnimatorPanel::RenderFacePoses()
{
    AnimatorFaceConfig& face = controller_.face;
    ImGui::SeparatorText(loc::T("animator.poses.expression_poses"));
    if (ImGui::SmallButton(loc::TL("animator.poses.add_pose")))
    {
        FaceExpressionPose pose;
        pose.name = "Pose " + std::to_string(face.poses.size() + 1);
        face.poses.push_back(pose);
        face_pose_preview_ = (int)face.poses.size() - 1;
        dirty_ = true;
    }

    if (face_pose_preview_ >= (int)face.poses.size())
        face_pose_preview_ = -1;

    for (std::size_t index = 0; index < face.poses.size(); ++index)
    {
        FaceExpressionPose& pose = face.poses[index];
        ImGui::PushID((int)index);
        // Per-index id, so renaming does not steal focus on every keystroke.
        if (ImGui::TreeNode((void*)(uintptr_t)index, "%s",
                            pose.name.empty() ? "Pose" : pose.name.c_str()))
        {
            dirty_ |= InputString("Name", pose.name);

            bool previewing = (face_pose_preview_ == (int)index);
            if (ImGui::Checkbox(loc::TL("animator.poses.preview"), &previewing))
                face_pose_preview_ = previewing ? (int)index : -1;

            ImGui::SameLine();
            if (!pose.name.empty() && face.default_pose == pose.name)
                ImGui::TextDisabled(loc::T("animator.poses.default"));
            else if (ImGui::SmallButton(loc::TL("animator.poses.set_as_default")))
            {
                face.default_pose = pose.name;
                dirty_ = true;
            }

            // Offer only shapes this pose does not already drive.
            std::vector<const char*> available;
            std::vector<int> shape_of;
            available.push_back("+ Add blendshape...");
            shape_of.push_back(-1);
            for (int shape = 0; shape < face::kShapeCount; ++shape)
            {
                const char* name = face::ShapeName(shape);
                bool present = false;
                for (const auto& [target, _] : pose.weights)
                    if (target == name) { present = true; break; }
                if (!present) { available.push_back(name); shape_of.push_back(shape); }
            }
            int add = 0;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("##addshape", &add, available.data(), (int)available.size()) && add > 0)
            {
                pose.weights.push_back({face::ShapeName(shape_of[(std::size_t)add]), 1.0f});
                dirty_ = true;
            }

            for (std::size_t w = 0; w < pose.weights.size(); ++w)
            {
                ImGui::PushID((int)w);
                if (ImGui::SmallButton("X"))
                {
                    pose.weights.erase(pose.weights.begin() + (std::ptrdiff_t)w);
                    dirty_ = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(220.0f);
                dirty_ |= ImGui::SliderFloat(pose.weights[w].first.c_str(),
                                             &pose.weights[w].second, 0.0f, 1.0f, "%.2f");
                ImGui::PopID();
            }

            if (pose.weights.empty())
                ImGui::TextDisabled(loc::T("animator.poses.none"));

            if (ImGui::SmallButton(loc::TL("animator.poses.remove_pose")))
            {
                if (face.default_pose == pose.name) face.default_pose.clear();
                face.poses.erase(face.poses.begin() + (std::ptrdiff_t)index);
                if (face_pose_preview_ == (int)index) face_pose_preview_ = -1;
                dirty_ = true;
                ImGui::TreePop();
                ImGui::PopID();
                break;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (face.poses.empty())
        ImGui::TextDisabled(loc::T("animator.poses.none"));
}

void AnimatorPanel::RenderRecord(EngineState& state)
{
    ImGui::SeparatorText(loc::T("animator.record.record"));

    if (!audio_devices_scanned_)
    {
        audio_devices_ = facerec::AudioDevices(audio_device_error_);
        audio_devices_scanned_ = true;
        // Default to a real microphone. Leaving this on "(no audio)" produces a
        // silent take that only reveals itself on playback.
        if (!audio_devices_.empty() && audio_device_index_ == 0)
            audio_device_index_ = 1;
    }

    std::vector<const char*> names;
    names.push_back("(no audio)");
    for (const std::string& device : audio_devices_) names.push_back(device.c_str());
    ImGui::SetNextItemWidth(280.0f);
    ImGui::Combo(loc::TL("animator.record.microphone"), &audio_device_index_, names.data(), (int)names.size());
    if (!audio_device_error_.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled(loc::T("animator.record.text"), audio_device_error_.c_str());
    }

    if (audio_device_index_ == 0)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                           "No microphone selected - this take will be silent");

    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputText(loc::TL("animator.record.take_name"), take_name_, sizeof(take_name_));

    const double now = ImGui::GetTime();
    if (recorder_.Recording())
    {
        if (ImGui::Button(loc::TL("animator.record.stop")))
        {
            std::string error;
            if (recorder_.Stop(now, error))
            {
                const std::string relative =
                    std::string("assets/face/") + take_name_ + ".faceclip";
                auto& clips = controller_.face.clips;
                if (std::find(clips.begin(), clips.end(), relative) == clips.end())
                {
                    clips.push_back(relative);
                    dirty_ = true;
                }
                state.AddLog("Recorded " + relative);
            }
            else
            {
                state.AddLog("Recording failed: " + error, LogLevel::Error);
            }
        }
        ImGui::SameLine();
        if (recorder_.WaitingForAudio())
            ImGui::TextDisabled(loc::T("animator.record.hint"));
        else
            ImGui::Text(loc::T("animator.record.s_frames"), recorder_.Elapsed(now), recorder_.FrameCount());
    }
    else
    {
        const bool ready = live_link_.IsOpen() && live_link_.Receiving(now) && take_name_[0];
        if (!ready) ImGui::BeginDisabled();
        if (ImGui::Button(loc::TL("animator.record.record")))
        {
            const std::string relative = std::string("assets/face/") + take_name_;
            const std::string device = audio_device_index_ > 0
                ? audio_devices_[(std::size_t)audio_device_index_ - 1] : std::string();
            std::string error;
            if (!recorder_.Start(state.project_root / (relative + ".faceclip"),
                                 state.project_root / (relative + ".mp2"),
                                 relative + ".mp2", device, 30.0f, error))
                state.AddLog("Could not start recording: " + error, LogLevel::Error);
        }
        if (!ready) ImGui::EndDisabled();
        if (!live_link_.Receiving(now))
        {
            ImGui::SameLine();
            ImGui::TextDisabled(loc::T("animator.record.needs_tracking"));
        }
    }

}

void AnimatorPanel::RenderFaceClips(EngineState& state)
{
    ImGui::SeparatorText(loc::T("animator.faceclips.clips"));
    auto& clips = controller_.face.clips;
    for (std::size_t index = 0; index < clips.size(); ++index)
    {
        ImGui::PushID((int)(2000 + index));
        if (ImGui::SmallButton("X"))
        {
            clips.erase(clips.begin() + (std::ptrdiff_t)index);
            dirty_ = true;
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        const std::filesystem::path path(clips[index]);
        const bool exists = std::filesystem::exists(state.project_root / path);
        const bool playing = face_clip_playing_ && face_clip_path_ == clips[index];
        if (!exists) ImGui::BeginDisabled();
        if (ImGui::SmallButton(playing ? "Stop" : "Play"))
        {
            if (playing) StopFaceClipPreview();
            else         PlayFaceClipPreview(state, clips[index]);
        }
        if (!exists) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!exists) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
        ImGui::TextUnformatted(path.stem().string().c_str());
        if (!exists)
        {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled(loc::T("animator.faceclips.missing"));
        }
        ImGui::PopID();
    }
    if (clips.empty())
        ImGui::TextDisabled(loc::T("animator.faceclips.none"));
}

void AnimatorPanel::RenderFace(EngineState& state)
{
    RenderLiveLink(state);
    RenderRecord(state);
    RenderFaceClips(state);
    RenderFacePoses();
    UpdateFacePreview();
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
        if (ImGui::BeginTabItem(loc::TL("animator.controller.general")))
        {
            RenderStates();
            RenderTransitions();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(loc::TL("animator.controller.face")))
        {
            RenderFace(state);
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
    if (!ImGui::Begin(loc::TWin("panel.animator.title", "Animator"), &state.show_animator_panel)) { ImGui::End(); return; }
    if (!state.HasProject())
    {
        ImGui::TextDisabled(loc::T("animator.toolbar.hint"));
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

    if (dirty_) { ImGui::SameLine(); ImGui::TextDisabled(loc::T("animator.toolbar.unsaved_changes")); }
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
    TickTracking(state);
    ImGui::Separator();
    RenderControllerEditor(state);
    ImGui::End();
}