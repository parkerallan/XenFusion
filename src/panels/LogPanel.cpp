#include "panels/LogPanel.h"

#include "core/Log.h"
#include "state/EngineState.h"
#include "ui/Icons.h"

#include "imgui.h"

namespace
{
    const char* Tag(LogLevel l)
    {
        switch (l)
        {
        case LogLevel::Error:   return "[ERROR] ";
        case LogLevel::Warning: return "[WARN]  ";
        case LogLevel::Build:   return "[BUILD] ";
        case LogLevel::Script:  return "[LOG]   ";
        default:                return "[INFO]  ";
        }
    }

    ImVec4 Color(LogLevel l)
    {
        switch (l)
        {
        case LogLevel::Error:   return ImVec4(0.95f, 0.42f, 0.38f, 1.0f);
        case LogLevel::Warning: return ImVec4(0.95f, 0.75f, 0.25f, 1.0f);
        case LogLevel::Build:   return ImVec4(0.45f, 0.72f, 1.00f, 1.0f);
        case LogLevel::Script:  return ImVec4(0.55f, 0.85f, 0.65f, 1.0f);
        default:                return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        }
    }

    bool Enabled(const EngineState& s, LogLevel l)
    {
        switch (l)
        {
        case LogLevel::Error:   return s.log_show_error;
        case LogLevel::Warning: return s.log_show_warning;
        case LogLevel::Build:   return s.log_show_build;
        case LogLevel::Script:  return s.log_show_script;
        default:                return s.log_show_info;
        }
    }
}

void LogPanel::Render(EngineState& state)
{
    if (!state.show_log_panel)
        return;

    if (!ImGui::Begin("Log", &state.show_log_panel))
    {
        ImGui::End();
        return;
    }

    // Toolbar: clear + auto-scroll toggle.
    const float button = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    if (ImGui::Button(ICON_FA_TRASH, ImVec2(button, button)))
        applog::Clear();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Clear log");
    ImGui::SameLine();
    if (!state.auto_scroll_log)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (ImGui::Button(ICON_FA_ARROW_DOWN, ImVec2(button, button)))
        state.auto_scroll_log = !state.auto_scroll_log;
    if (!state.auto_scroll_log)
        ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Auto-scroll: %s", state.auto_scroll_log ? "On" : "Off");

    ImGui::Separator();

    if (ImGui::BeginChild("##logscroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        for (const LogEntry& e : applog::Entries())
        {
            if (!Enabled(state, e.level))
                continue;
            ImGui::PushStyleColor(ImGuiCol_Text, Color(e.level));
            ImGui::TextUnformatted(Tag(e.level));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(e.text.c_str());
            ImGui::PopStyleColor();
        }

        if (state.auto_scroll_log && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();
}
