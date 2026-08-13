#include "panels/FilesPanel.h"

#include "project/ProjectIO.h"
#include "scene/Hierarchy.h"
#include "state/EngineState.h"
#include "ui/Icons.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // Deferred context-menu action (applied after the tree is drawn so we don't
    // mutate state.scenes mid-iteration).
    enum class PendingOp { None, RenameScene, CopyScene, DeleteScene, RenameObject, CopyObject, DeleteObject, ReparentObject };
    struct PendingAction { PendingOp op = PendingOp::None; int scene = -1; int object = -1; int new_parent = -1; };
    struct ObjectPayload { int scene; int object; };

    PendingAction g_action;
    bool          g_open_rename = false;
    char          g_rename_buf[128] = {};

    void RequestRename(PendingOp op, int scene, int object, const std::string& current)
    {
        g_action = {op, scene, object};
        std::snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", current.c_str());
        g_open_rename = true;
    }

    int FindSceneIndex(EngineState& state, const fs::path& path)
    {
        for (int i = 0; i < (int)state.scenes.size(); ++i)
            if (state.scenes[i].path == path)
                return i;
        return -1;
    }

    void ResolveHierarchy(const SceneFile& scene, std::vector<int>& parents,
                          std::vector<hier::Resolved>& resolved)
    {
        std::vector<std::string> names(scene.objects.size()), parentNames(scene.objects.size());
        std::vector<hier::Node> nodes(scene.objects.size());
        for (int i = 0; i < (int)scene.objects.size(); ++i)
        {
            names[i] = scene.objects[i].name;
            parentNames[i] = scene.objects[i].parent;
        }
        hier::MapParents(names, parentNames, parents);
        for (int i = 0; i < (int)scene.objects.size(); ++i)
        {
            const SceneObject& object = scene.objects[i];
            for (int axis = 0; axis < 3; ++axis)
            {
                nodes[i].pos[axis] = object.position[axis];
                nodes[i].rot[axis] = object.rotation[axis];
                nodes[i].scale[axis] = object.scale[axis];
            }
            nodes[i].visible = object.visible;
            nodes[i].parent = parents[i];
        }
        hier::Resolve(nodes, resolved);
    }

    void ResolveImageOrigins(const SceneFile& scene, const std::vector<int>& parents,
                             std::vector<std::array<float, 2>>& origins)
    {
        origins.assign(scene.objects.size(), {0.0f, 0.0f});
        std::vector<bool> done(scene.objects.size(), false);
        for (int pass = 0; pass < (int)scene.objects.size(); ++pass)
            for (int i = 0; i < (int)scene.objects.size(); ++i)
            {
                if (done[i] || (parents[i] >= 0 && !done[parents[i]])) continue;
                if (parents[i] >= 0) origins[i] = origins[parents[i]];
                for (const ObjectAttribute& attribute : scene.objects[i].attributes)
                    if (attribute.type == "Image")
                    {
                        origins[i][0] += attribute.image_x;
                        origins[i][1] += attribute.image_y;
                        break;
                    }
                done[i] = true;
            }
    }

    void DrawObject(EngineState& state, int sceneIndex, int objectIndex,
                    const std::vector<int>& parents,
                    const std::vector<hier::Resolved>& resolved)
    {
        SceneFile& scene = state.scenes[sceneIndex];
        bool hasChildren = false;
        for (int i = 0; i < (int)parents.size(); ++i)
            if (parents[i] == objectIndex) { hasChildren = true; break; }

        ImGui::PushID(objectIndex);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (hasChildren) flags |= ImGuiTreeNodeFlags_OpenOnArrow;
        else flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (state.selected_scene == sceneIndex && state.selected_object == objectIndex)
            flags |= ImGuiTreeNodeFlags_Selected;

        const ImVec2 rowPos = ImGui::GetCursorScreenPos();
        if (!resolved[objectIndex].visible)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const bool open = ImGui::TreeNodeEx("##obj", flags, ICON_FA_CUBE "  %s",
                                            scene.objects[objectIndex].name.c_str());
        if (!resolved[objectIndex].visible)
            ImGui::PopStyleColor();

        const bool released = ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        const bool toggled = ImGui::IsItemToggledOpen();
        bool releasedOnArrow = false;
        if (hasChildren && released)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            const float arrowEnd = rowPos.x + ImGui::GetFontSize() + style.FramePadding.x * 2.0f + style.TouchExtraPadding.x;
            releasedOnArrow = ImGui::GetIO().MousePos.x < arrowEnd;
        }

        bool dragging = false;
        if (ImGui::BeginDragDropSource())
        {
            const ObjectPayload payload = {sceneIndex, objectIndex};
            ImGui::SetDragDropPayload("SCENE_OBJECT", &payload, sizeof(payload));
            ImGui::TextUnformatted(scene.objects[objectIndex].name.c_str());
            ImGui::EndDragDropSource();
            dragging = true;
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT"))
            {
                const ObjectPayload source = *static_cast<const ObjectPayload*>(payload->Data);
                if (source.scene == sceneIndex && source.object != objectIndex &&
                    !hier::IsDescendant(parents, objectIndex, source.object))
                    g_action = {PendingOp::ReparentObject, sceneIndex, source.object, objectIndex};
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("##obj_ctx"))
        {
            if (ImGui::MenuItem("Rename")) RequestRename(PendingOp::RenameObject, sceneIndex, objectIndex, scene.objects[objectIndex].name);
            if (ImGui::MenuItem("Copy"))   g_action = {PendingOp::CopyObject, sceneIndex, objectIndex};
            if (ImGui::MenuItem("Delete")) g_action = {PendingOp::DeleteObject, sceneIndex, objectIndex};
            ImGui::EndPopup();
        }
        if (released && !toggled && !releasedOnArrow && !dragging)
        {
            state.selected_scene = sceneIndex;
            state.selected_object = objectIndex;
        }

        if (hasChildren && open)
        {
            for (int i = 0; i < (int)parents.size(); ++i)
                if (parents[i] == objectIndex)
                    DrawObject(state, sceneIndex, i, parents, resolved);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void DrawScene(EngineState& state, int sceneIndex)
    {
        SceneFile& scene = state.scenes[sceneIndex];
        std::vector<int> parents;
        std::vector<hier::Resolved> resolved;
        ResolveHierarchy(scene, parents, resolved);
        ImGui::PushID(sceneIndex);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (state.selected_scene == sceneIndex && state.selected_object == -1)
            flags |= ImGuiTreeNodeFlags_Selected;
        if (scene.objects.empty())
            flags |= ImGuiTreeNodeFlags_Leaf;

        const std::string label = scene.name + (scene.dirty ? " *" : "");
        const bool open = ImGui::TreeNodeEx("##scene", flags, ICON_FA_FILM "  %s", label.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            state.selected_scene = sceneIndex;
            state.selected_object = -1;
        }
        if (ImGui::BeginPopupContextItem("##scene_ctx"))
        {
            if (ImGui::MenuItem("Rename")) RequestRename(PendingOp::RenameScene, sceneIndex, -1, scene.name);
            if (ImGui::MenuItem("Copy"))   g_action = {PendingOp::CopyScene, sceneIndex, -1};
            if (ImGui::MenuItem("Delete")) g_action = {PendingOp::DeleteScene, sceneIndex, -1};
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT"))
            {
                const ObjectPayload source = *static_cast<const ObjectPayload*>(payload->Data);
                if (source.scene == sceneIndex)
                    g_action = {PendingOp::ReparentObject, sceneIndex, source.object, -1};
            }
            ImGui::EndDragDropTarget();
        }

        if (open)
        {
            for (int i = 0; i < (int)scene.objects.size(); ++i)
                if (parents[i] < 0)
                    DrawObject(state, sceneIndex, i, parents, resolved);
            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    void DrawDirectory(EngineState& state, const fs::path& dir)
    {
        std::error_code ec;
        std::vector<fs::directory_entry> entries;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
            entries.push_back(e);

        std::sort(entries.begin(), entries.end(),
                  [](const fs::directory_entry& a, const fs::directory_entry& b)
                  {
                      const bool da = a.is_directory(), db = b.is_directory();
                      if (da != db) return da; // directories first
                      return a.path().filename().string() < b.path().filename().string();
                  });

        for (const fs::directory_entry& e : entries)
        {
            const fs::path p = e.path();
            if (e.is_directory(ec))
            {
                if (p == state.AssetsDir())
                    continue; // assets/ is the Assets tab's responsibility

                ImGui::PushID(p.string().c_str());
                const std::string label = std::string(ICON_FA_FOLDER "  ") + p.filename().string();
                if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    DrawDirectory(state, p);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            else if (p.extension() == ".scene")
            {
                const int idx = FindSceneIndex(state, p);
                if (idx >= 0)
                    DrawScene(state, idx);
                else
                    ImGui::TreeNodeEx((std::string(ICON_FA_FILM "  ") + p.filename().string()).c_str(),
                                      ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
            }
            else
            {
                ImGui::TreeNodeEx((std::string(ICON_FA_FILE "  ") + p.filename().string()).c_str(),
                                  ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    if (p.extension() == ".anim")
                    {
                        state.open_animator_path = p;
                        state.show_animator_panel = true;
                        ImGui::SetWindowFocus("Animator");
                    }
                    else
                    {
                        state.open_file_path = p;
                        state.show_editor_panel = true;
                        ImGui::SetWindowFocus("Editor");
                    }
                }
            }
        }
    }
}

void FilesPanel::Render(EngineState& state)
{
    if (!state.show_files_panel)
        return;

    if (!ImGui::Begin("Files", &state.show_files_panel))
    {
        ImGui::End();
        return;
    }

    if (!state.HasProject())
    {
        ImGui::TextDisabled("No project open.");
        ImGui::TextDisabled("File > New Project / Open Project.");
        ImGui::End();
        return;
    }

    // Toolbar: project name + add menu.
    ImGui::Text(ICON_FA_FOLDER_OPEN "  %s", state.project_name.c_str());
    ImGui::SameLine();
    {
        const char* lbl = ICON_FA_PLUS " Add";
        const float w = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        if (ImGui::Button(lbl))
            ImGui::OpenPopup("files_add");
    }
    if (ImGui::BeginPopup("files_add"))
    {
        if (ImGui::MenuItem("New Scene"))
            project::NewScene(state, "Scene");

        SceneFile* scene = state.SelectedScene();
        if (ImGui::MenuItem("New Object", nullptr, false, scene != nullptr))
            state.selected_object = project::NewObject(state, *scene, "Object");

        ImGui::EndPopup();
    }

    ImGui::Separator();

    DrawDirectory(state, state.project_root);

    // --- Apply deferred copy/delete actions ---
    switch (g_action.op)
    {
    case PendingOp::CopyScene:
        project::CopyScene(state, g_action.scene);
        g_action.op = PendingOp::None;
        break;
    case PendingOp::DeleteScene:
        project::DeleteScene(state, g_action.scene);
        g_action.op = PendingOp::None;
        break;
    case PendingOp::CopyObject:
        if (g_action.scene >= 0 && g_action.scene < (int)state.scenes.size())
        {
            SceneFile& s = state.scenes[g_action.scene];
            if (g_action.object >= 0 && g_action.object < (int)s.objects.size())
            {
                SceneObject dup = s.objects[g_action.object];
                dup.name = project::MakeUniqueObjectName(s, dup.name + " copy");
                s.objects.insert(s.objects.begin() + g_action.object + 1, dup);
                if (state.selected_scene == g_action.scene && state.selected_object > g_action.object)
                    ++state.selected_object;
                project::SaveScene(s);
            }
        }
        g_action.op = PendingOp::None;
        break;
    case PendingOp::DeleteObject:
        if (g_action.scene >= 0 && g_action.scene < (int)state.scenes.size())
        {
            SceneFile& s = state.scenes[g_action.scene];
            if (g_action.object >= 0 && g_action.object < (int)s.objects.size())
            {
                std::vector<int> parents;
                std::vector<hier::Resolved> resolved;
                ResolveHierarchy(s, parents, resolved);
                std::vector<bool> remove(s.objects.size(), false);
                for (int i = 0; i < (int)s.objects.size(); ++i)
                    remove[i] = i == g_action.object || hier::IsDescendant(parents, i, g_action.object);

                if (state.selected_scene == g_action.scene && state.selected_object >= 0 &&
                    state.selected_object < (int)remove.size())
                {
                    if (remove[state.selected_object]) state.selected_object = -1;
                    else
                    {
                        int removedBefore = 0;
                        for (int i = 0; i < state.selected_object; ++i)
                            if (remove[i]) ++removedBefore;
                        state.selected_object -= removedBefore;
                    }
                }
                for (int i = (int)s.objects.size() - 1; i >= 0; --i)
                    if (remove[i]) s.objects.erase(s.objects.begin() + i);
                project::SaveScene(s);
            }
        }
        g_action.op = PendingOp::None;
        break;
    case PendingOp::ReparentObject:
        if (g_action.scene >= 0 && g_action.scene < (int)state.scenes.size())
        {
            SceneFile& s = state.scenes[g_action.scene];
            if (g_action.object >= 0 && g_action.object < (int)s.objects.size() &&
                g_action.new_parent < (int)s.objects.size())
            {
                std::vector<int> parents;
                std::vector<hier::Resolved> resolved;
                ResolveHierarchy(s, parents, resolved);
                std::vector<std::array<float, 2>> imageOrigins;
                ResolveImageOrigins(s, parents, imageOrigins);
                if (g_action.new_parent < 0 ||
                    (g_action.new_parent != g_action.object &&
                     !hier::IsDescendant(parents, g_action.new_parent, g_action.object)))
                {
                    float local[16];
                    for (int element = 0; element < 16; ++element)
                        local[element] = resolved[g_action.object].world[element];
                    if (g_action.new_parent >= 0)
                    {
                        float inverseParent[16];
                        hier::AffineInverse(resolved[g_action.new_parent].world, inverseParent);
                        hier::Multiply(local, inverseParent, local);
                    }
                    SceneObject& object = s.objects[g_action.object];
                    hier::Decompose(local, object.position, object.rotation, object.scale);
                    const float oldParentX = parents[g_action.object] >= 0 ? imageOrigins[parents[g_action.object]][0] : 0.0f;
                    const float oldParentY = parents[g_action.object] >= 0 ? imageOrigins[parents[g_action.object]][1] : 0.0f;
                    const float newParentX = g_action.new_parent >= 0 ? imageOrigins[g_action.new_parent][0] : 0.0f;
                    const float newParentY = g_action.new_parent >= 0 ? imageOrigins[g_action.new_parent][1] : 0.0f;
                    for (ObjectAttribute& attribute : object.attributes)
                        if (attribute.type == "Image")
                        {
                            attribute.image_x += oldParentX - newParentX;
                            attribute.image_y += oldParentY - newParentY;
                        }
                    object.parent = g_action.new_parent >= 0 ? s.objects[g_action.new_parent].name : std::string();
                    project::SaveScene(s);
                }
            }
        }
        g_action.op = PendingOp::None;
        break;
    default:
        break; // rename handled via popup below
    }

    // --- Rename popup ---
    if (g_open_rename)
    {
        ImGui::OpenPopup("Rename##files");
        g_open_rename = false;
    }
    if (ImGui::BeginPopup("Rename##files"))
    {
        ImGui::TextUnformatted("New name:");
        ImGui::SetNextItemWidth(240.0f);
        const bool enter = ImGui::InputText("##rn", g_rename_buf, sizeof(g_rename_buf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Rename") || enter) && g_rename_buf[0] != '\0')
        {
            if (g_action.op == PendingOp::RenameScene)
            {
                project::RenameScene(state, g_action.scene, g_rename_buf);
            }
            else if (g_action.op == PendingOp::RenameObject &&
                     g_action.scene >= 0 && g_action.scene < (int)state.scenes.size())
            {
                SceneFile& s = state.scenes[g_action.scene];
                if (g_action.object >= 0 && g_action.object < (int)s.objects.size())
                {
                    const std::string oldName = s.objects[g_action.object].name;
                    const std::string newName = project::MakeUniqueObjectName(s, g_rename_buf, g_action.object);
                    s.objects[g_action.object].name = newName;
                    for (SceneObject& object : s.objects)
                        if (object.parent == oldName) object.parent = newName;
                    project::SaveScene(s);
                }
            }
            g_action.op = PendingOp::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            g_action.op = PendingOp::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}
