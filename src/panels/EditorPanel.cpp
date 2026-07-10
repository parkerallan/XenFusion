#include "panels/EditorPanel.h"

#include "state/EngineState.h"
#include "ui/Icons.h"

#include "TextEditor.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

EditorPanel::EditorPanel()
    : editor_(std::make_unique<TextEditor>())
{
    editor_->SetShowWhitespaces(false);
    editor_->SetPalette(TextEditor::GetDarkPalette());
}

EditorPanel::~EditorPanel() = default;

void EditorPanel::LoadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    editor_->SetText(ss.str());

    std::string ext = path.extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);

    if (ext == ".lua")
        editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    else if (ext == ".hlsl" || ext == ".glsl" || ext == ".fx" || ext == ".vsh" || ext == ".fsh")
        editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::GLSL());
    else if (ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".c" || ext == ".cs")
        editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
    else
        editor_->SetLanguageDefinition(TextEditor::LanguageDefinition());

    loaded_path_ = path;
}

void EditorPanel::Save(EngineState& state)
{
    if (loaded_path_.empty())
        return;
    std::ofstream out(loaded_path_, std::ios::binary | std::ios::trunc);
    out << editor_->GetText();
    state.AddLog("Saved " + loaded_path_.filename().string());
}

void EditorPanel::Render(EngineState& state)
{
    if (!state.show_editor_panel)
        return;

    if (!ImGui::Begin("Editor", &state.show_editor_panel, ImGuiWindowFlags_MenuBar))
    {
        ImGui::End();
        return;
    }

    // Load a newly-requested file.
    if (!state.open_file_path.empty() && state.open_file_path != loaded_path_)
        LoadFile(state.open_file_path);

    if (loaded_path_.empty())
    {
        ImGui::TextDisabled("Open a text or code file from the Files or Assets panel.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginMenuBar())
    {
        ImGui::TextUnformatted(ICON_FA_FILE " ");
        ImGui::TextUnformatted(loaded_path_.filename().string().c_str());
        if (ImGui::SmallButton(ICON_FA_FLOPPY_DISK " Save"))
            Save(state);
        // Save when the editor is focused and Ctrl+S is pressed.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
            Save(state);
        ImGui::EndMenuBar();
    }

    editor_->Render("##texteditor");

    ImGui::End();
}
