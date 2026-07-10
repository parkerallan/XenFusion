#include "panels/FilesPanel.h"

#include "project/ProjectIO.h"
#include "state/EngineState.h"
#include "ui/Icons.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace
{
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
                    state.open_file_path = p;
                    state.show_editor_panel = true;
                    ImGui::SetWindowFocus("Editor");
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

    ImGui::End();
}
