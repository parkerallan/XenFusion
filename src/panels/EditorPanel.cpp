#include "panels/EditorPanel.h"

#include "state/EngineState.h"
#include "ui/Icons.h"

#include "TextEditor.h"
#include "imgui.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

EditorPanel::EditorPanel()  = default;
EditorPanel::~EditorPanel() = default;

namespace
{
    void ApplyLanguage(TextEditor& ed, const std::filesystem::path& path)
    {
        std::string ext = path.extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);

        if (ext == ".lua")
            ed.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
        else if (ext == ".hlsl" || ext == ".glsl" || ext == ".fx" || ext == ".vsh" || ext == ".fsh")
            ed.SetLanguageDefinition(TextEditor::LanguageDefinition::GLSL());
        else if (ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".c" || ext == ".cs")
            ed.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
        else
            ed.SetLanguageDefinition(TextEditor::LanguageDefinition());
    }
}

void EditorPanel::OpenFile(const std::filesystem::path& path)
{
    // Already open? Just focus that tab.
    for (Doc& d : docs_)
    {
        if (d.path == path)
        {
            d.want_focus = true;
            return;
        }
    }

    Doc doc;
    doc.path   = path;
    doc.editor = std::make_unique<TextEditor>();
    doc.editor->SetShowWhitespaces(false);
    doc.editor->SetPalette(TextEditor::GetDarkPalette());

    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    doc.editor->SetText(ss.str());
    ApplyLanguage(*doc.editor, path);

    doc.want_focus = true;
    docs_.push_back(std::move(doc));
}

void EditorPanel::SaveDoc(Doc& doc, EngineState& state)
{
    std::ofstream out(doc.path, std::ios::binary | std::ios::trunc);
    out << doc.editor->GetText();
    state.AddLog("Saved " + doc.path.filename().string());
}

void EditorPanel::Render(EngineState& state)
{
    if (!state.show_editor_panel)
        return;

    if (!ImGui::Begin("Editor", &state.show_editor_panel))
    {
        ImGui::End();
        return;
    }

    // A panel (Assets/Files) requested a file be opened.
    if (!state.open_file_path.empty())
    {
        OpenFile(state.open_file_path);
        state.open_file_path.clear();
    }

    if (docs_.empty())
    {
        ImGui::TextDisabled("Open a text or code file from the Files or Assets panel.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##editor_tabs",
                           ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs |
                           ImGuiTabBarFlags_FittingPolicyScroll))
    {
        for (std::size_t i = 0; i < docs_.size(); /* advance below */)
        {
            Doc& doc = docs_[i];
            ImGui::PushID((int)i);

            bool open = true;
            const ImGuiTabItemFlags flags = doc.want_focus ? ImGuiTabItemFlags_SetSelected : 0;
            doc.want_focus = false;

            if (ImGui::BeginTabItem(doc.path.filename().string().c_str(), &open, flags))
            {
                if (ImGui::SmallButton(ICON_FA_FLOPPY_DISK " Save"))
                    SaveDoc(doc, state);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", doc.path.string().c_str());

                // Ctrl+S saves the active tab.
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
                    SaveDoc(doc, state);

                doc.editor->Render("##texteditor");
                ImGui::EndTabItem();
            }

            ImGui::PopID();

            if (!open) docs_.erase(docs_.begin() + (long)i);
            else       ++i;
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
