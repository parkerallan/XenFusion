#include "panels/AnimatorPanel.h"

#include "imgui.h"
#include "state/EngineState.h"
#include "ui/Icons.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

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
    bone_modifier_text_.clear();
    for (const nlohmann::json& modifier : controller_.bone_modifiers)
        bone_modifier_text_.push_back(modifier.dump(2));
}

void AnimatorPanel::NewController()
{
    controller_ = AnimatorController();
    controller_.name = new_name_;
    loaded_path_.clear();
    loaded_ = true;
    dirty_ = true;
    bone_modifier_text_.clear();
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
                AnimatorClipReference clip;
                clip.id = "Clip_" + std::to_string(controller_.clips.size() + 1);
                clip.source_model_path = path;
                clip.clip_name = clip.id;
                controller_.clips.push_back(clip);
                if (controller_.preview_model_path.empty()) controller_.preview_model_path = path;
                dirty_ = true;
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
    (void)state;
    ImGui::TextUnformatted("Bone Modifiers");
    ImGui::Separator();
    ImGui::BeginDisabled();
    ImGui::Button("+ Add Selected Bone");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(click a bone in the preview)");
    int remove = -1;
    for (std::size_t index = 0; index < controller_.bone_modifiers.size(); ++index)
    {
        nlohmann::json& modifier = controller_.bone_modifiers[index];
        ImGui::PushID((int)index);
        std::string bone_name = modifier.value("bone_name", std::string());
        const std::string title = bone_name.empty() ? "Modifier" : bone_name;
        if (ImGui::TreeNode(title.c_str()))
        {
            if (InputString("Bone", bone_name)) { modifier["bone_name"] = bone_name; dirty_ = true; }

            std::string type = modifier.value("type", std::string("Physics"));
            int type_index = type == "Collision" ? 1 : 0;
            const char* type_names[] = {"Physics", "Collision"};
            if (ImGui::Combo("Type", &type_index, type_names, IM_ARRAYSIZE(type_names)))
            { modifier["type"] = type_names[type_index]; dirty_ = true; }

            if (type_index == 0)
            {
                int preset = 0;
                const char* presets[] = {"Custom", "Hair (short)", "Hair (long)",
                    "Breast", "Cloth (light)", "Cloth (heavy)", "Tail / Antenna"};
                ImGui::Combo("Preset", &preset, presets, IM_ARRAYSIZE(presets));

                struct Field { const char* label; const char* key; float value; float minimum; float maximum; };
                Field fields[] = {
                    {"Strength", "strength", modifier.value("strength", 1.0f), 0.0f, 2.0f},
                    {"Stiffness", "stiffness", modifier.value("stiffness", 0.3f), 0.0f, 1.0f},
                    {"Damping", "damping", modifier.value("damping", 1.0f), 0.0f, 3.0f},
                    {"Mass", "mass", modifier.value("mass", 1.0f), 0.001f, 100.0f},
                    {"Drag", "drag", modifier.value("drag", 0.05f), 0.0f, 1.0f},
                    {"Gravity Scale", "gravity_scale", modifier.value("gravity_scale", 0.0f), 0.0f, 5.0f},
                };
                for (Field& field : fields)
                    if (ImGui::DragFloat(field.label, &field.value, 0.01f, field.minimum, field.maximum, "%.2f"))
                    { modifier[field.key] = (std::clamp)(field.value, field.minimum, field.maximum); dirty_ = true; }

                if (ImGui::TreeNode("Advanced"))
                {
                    float gravity[3] = {0.0f, -1.0f, 0.0f};
                    if (modifier.contains("gravity_dir") && modifier["gravity_dir"].is_array() && modifier["gravity_dir"].size() == 3)
                        for (int axis = 0; axis < 3; ++axis) gravity[axis] = modifier["gravity_dir"][axis].get<float>();
                    if (ImGui::DragFloat3("Gravity Dir", gravity, 0.01f, -1.0f, 1.0f, "%.2f"))
                    { modifier["gravity_dir"] = {gravity[0], gravity[1], gravity[2]}; dirty_ = true; }
                    float angle = modifier.value("angle_limit_deg", 60.0f);
                    if (ImGui::DragFloat("Angle Limit", &angle, 0.5f, 0.0f, 180.0f, "%.1f deg"))
                    { modifier["angle_limit_deg"] = (std::clamp)(angle, 0.0f, 180.0f); dirty_ = true; }
                    float radius = modifier.value("radius", 0.05f);
                    if (ImGui::DragFloat("Radius", &radius, 0.001f, 0.0f, 1.0f, "%.3f"))
                    { modifier["radius"] = (std::max)(0.0f, radius); dirty_ = true; }
                    bool affects_children = modifier.value("affects_children", true);
                    if (ImGui::Checkbox("Affects Children", &affects_children))
                    { modifier["affects_children"] = affects_children; dirty_ = true; }
                    ImGui::TreePop();
                }
            }
            else
            {
                const char* modes[] = {"Trigger", "Rigidbody"};
                int mode = modifier.value("collision_mode", std::string("Trigger")) == "Rigidbody" ? 1 : 0;
                if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes)))
                { modifier["collision_mode"] = modes[mode]; dirty_ = true; }
                ImGui::BeginDisabled();
                ImGui::Button("Fit to Bone");
                ImGui::EndDisabled();
                float half_extents[3] = {0.05f, 0.05f, 0.05f};
                float center[3] = {0.0f, 0.0f, 0.0f};
                if (modifier.contains("box_half_extents") && modifier["box_half_extents"].is_array() && modifier["box_half_extents"].size() == 3)
                    for (int axis = 0; axis < 3; ++axis) half_extents[axis] = modifier["box_half_extents"][axis].get<float>();
                if (modifier.contains("box_center") && modifier["box_center"].is_array() && modifier["box_center"].size() == 3)
                    for (int axis = 0; axis < 3; ++axis) center[axis] = modifier["box_center"][axis].get<float>();
                if (ImGui::DragFloat3("Half Extents", half_extents, 0.005f, 0.0f, 100.0f, "%.3f"))
                { modifier["box_half_extents"] = {half_extents[0], half_extents[1], half_extents[2]}; dirty_ = true; }
                if (ImGui::DragFloat3("Center Offset", center, 0.005f, -100.0f, 100.0f, "%.3f"))
                { modifier["box_center"] = {center[0], center[1], center[2]}; dirty_ = true; }
            }

            if (ImGui::Button("Remove Modifier")) remove = (int)index;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove >= 0)
    {
        controller_.bone_modifiers.erase(controller_.bone_modifiers.begin() + remove);
        if ((std::size_t)remove < bone_modifier_text_.size())
            bone_modifier_text_.erase(bone_modifier_text_.begin() + remove);
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

    ImGui::SetCursorScreenPos(viewport_min);
    ImGui::InvisibleButton("##AnimatorPreviewCanvas",
        ImVec2((std::max)(1.0f, canvas_max.x - viewport_min.x),
               (std::max)(1.0f, canvas_max.y - viewport_min.y)));
    ImGui::GetWindowDrawList()->AddRectFilled(viewport_min, canvas_max,
                                               IM_COL32(30, 30, 33, 255));
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
    if (ImGui::Button(ICON_FA_ROTATE)) preview_time_ = 0.0f;
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
    ImGui::SetNextItemWidth((std::max)(80.0f, ImGui::GetContentRegionAvail().x - 8.0f));
    ImGui::SliderFloat("##AnimatorPreviewTime", &preview_time_, 0.0f, 1.0f, "%.2fs");
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