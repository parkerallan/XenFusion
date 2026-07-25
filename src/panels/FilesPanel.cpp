#include "panels/FilesPanel.h"

#include "project/ProjectIO.h"
#include "state/EngineState.h"
#include "ui/Icons.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // Deferred context-menu action (applied after the tree is drawn so we don't
    // mutate state.scenes mid-iteration).
    enum class PendingOp { None, RenameScene, CopyScene, DeleteScene, RenameObject, CopyObject, DeleteObject };
    struct PendingAction { PendingOp op = PendingOp::None; int scene = -1; int object = -1; };

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

    void DrawScene(EngineState& state, int sceneIndex)
    {
        SceneFile& scene = state.scenes[sceneIndex];
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

        if (open)
        {
            for (int i = 0; i < (int)scene.objects.size(); ++i)
            {
                ImGui::PushID(i);
                ImGuiTreeNodeFlags of = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanAvailWidth |
                                        ImGuiTreeNodeFlags_NoTreePushOnOpen;
                if (state.selected_scene == sceneIndex && state.selected_object == i)
                    of |= ImGuiTreeNodeFlags_Selected;
                ImGui::TreeNodeEx("##obj", of, ICON_FA_CUBE "  %s", scene.objects[i].name.c_str());
                if (ImGui::IsItemClicked())
                {
                    state.selected_scene = sceneIndex;
                    state.selected_object = i;
                }
                if (ImGui::BeginPopupContextItem("##obj_ctx"))
                {
                    if (ImGui::MenuItem("Rename")) RequestRename(PendingOp::RenameObject, sceneIndex, i, scene.objects[i].name);
                    if (ImGui::MenuItem("Copy"))   g_action = {PendingOp::CopyObject, sceneIndex, i};
                    if (ImGui::MenuItem("Delete")) g_action = {PendingOp::DeleteObject, sceneIndex, i};
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
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
                dup.name += " copy";
                s.objects.insert(s.objects.begin() + g_action.object + 1, dup);
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
                s.objects.erase(s.objects.begin() + g_action.object);
                project::SaveScene(s);
                if (state.selected_scene == g_action.scene && state.selected_object == g_action.object)
                    state.selected_object = -1;
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
                    s.objects[g_action.object].name = g_rename_buf;
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
