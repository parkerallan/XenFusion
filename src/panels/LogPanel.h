#pragma once

#include "imgui.h"

#include <vector>

struct EngineState;

// Bottom panel — engine log output.
class LogPanel
{
public:
    void Render(EngineState& state);

private:
    // Line selection: drag box-select, shift-range, ctrl-toggle and Ctrl+A, all
    // from ImGui's multi-select. Selection is keyed by index into
    // applog::Entries() rather than by row, so toggling a level filter doesn't
    // reshuffle what's highlighted; m_visible maps a drawn row back to its entry.
    ImGuiSelectionBasicStorage m_selection;
    std::vector<int>           m_visible;
    int                        m_last_count = 0;
};
