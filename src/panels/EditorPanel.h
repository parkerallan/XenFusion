#pragma once

#include <filesystem>
#include <memory>

struct EngineState;
class TextEditor;

// Code/text editor panel backed by ImGuiColorTextEdit, mirroring the reference
// engine's EditorComponent. Edits the file at state.open_file_path.
class EditorPanel
{
public:
    EditorPanel();
    ~EditorPanel();

    void Render(EngineState& state);

private:
    void LoadFile(const std::filesystem::path& path);
    void Save(EngineState& state);

    std::unique_ptr<TextEditor> editor_;
    std::filesystem::path       loaded_path_;
};
