#pragma once

#include <filesystem>
#include <memory>
#include <vector>

struct EngineState;
class TextEditor;

// Code/text editor panel backed by ImGuiColorTextEdit, mirroring the reference
// engine's EditorComponent. Holds one tab per open file; a file is opened (or
// re-focused) by setting state.open_file_path, which this panel drains.
class EditorPanel
{
public:
    EditorPanel();
    ~EditorPanel();

    void Render(EngineState& state);

private:
    struct Doc
    {
        std::filesystem::path       path;
        std::unique_ptr<TextEditor> editor;
        bool                        want_focus = false; // select this tab this frame
    };

    void OpenFile(const std::filesystem::path& path); // add a tab or focus existing
    void SaveDoc(Doc& doc, EngineState& state);

    std::vector<Doc> docs_;
};
