#include "panels/LogPanel.h"
#include "loc/Loc.h"

#include "core/Log.h"
#include "state/EngineState.h"
#include "ui/Icons.h"

#include "imgui.h"

namespace
{
    // Not localized: fixed-width markers the log text aligns against.
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

    if (!ImGui::Begin(loc::TWin("panel.log.title", "Log"), &state.show_log_panel))
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
        ImGui::SetTooltip(loc::T("log.tooltip.clear"));
    ImGui::SameLine();
    if (!state.auto_scroll_log)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (ImGui::Button(ICON_FA_ARROW_DOWN, ImVec2(button, button)))
        state.auto_scroll_log = !state.auto_scroll_log;
    if (!state.auto_scroll_log)
        ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip(loc::T("log.tooltip.auto_scroll"),
                          state.auto_scroll_log ? loc::T("common.on") : loc::T("common.off"));

    ImGui::Separator();

    if (ImGui::BeginChild("##logscroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        const std::vector<LogEntry>& entries = applog::Entries();

        // Entries only ever append, so a stored index stays valid — except
        // across a Clear(), where the list shrinks and every index goes stale.
        if ((int)entries.size() < m_last_count)
            m_selection.Clear();
        m_last_count = (int)entries.size();

        m_visible.clear();
        for (int i = 0; i < (int)entries.size(); ++i)
            if (Enabled(state, entries[i].level))
                m_visible.push_back(i);

        // The drawn rows are a filtered subset, so the adapter maps a row
        // position to the entry index the selection is actually keyed by.
        // ApplyRequests runs every range/select-all through it.
        m_selection.UserData = &m_visible;
        m_selection.AdapterIndexToStorageId =
            [](ImGuiSelectionBasicStorage* self, int row) -> ImGuiID
            { return (ImGuiID)(*(const std::vector<int>*)self->UserData)[row]; };

        ImGuiMultiSelectIO* ms = ImGui::BeginMultiSelect(
            ImGuiMultiSelectFlags_BoxSelect1d | ImGuiMultiSelectFlags_ClearOnEscape |
            ImGuiMultiSelectFlags_ClearOnClickVoid,
            m_selection.Size, (int)m_visible.size());
        m_selection.ApplyRequests(ms);

        for (int row = 0; row < (int)m_visible.size(); ++row)
        {
            const int index = m_visible[row];
            const LogEntry& e = entries[index];
            ImGui::SetNextItemSelectionUserData(row);
            ImGui::PushID(index);
            // The Selectable carries an empty label: log text can contain '##',
            // which a label would read as an ID separator and hide everything
            // after it. The real text is drawn back over the row instead.
            const ImVec2 row_pos = ImGui::GetCursorPos();
            ImGui::Selectable("##row", m_selection.Contains((ImGuiID)index));
            ImGui::SetCursorPos(row_pos);
            ImGui::PushStyleColor(ImGuiCol_Text, Color(e.level));
            ImGui::TextUnformatted(Tag(e.level));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(e.text.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();
        }

        ms = ImGui::EndMultiSelect();
        m_selection.ApplyRequests(ms);

        // Ctrl+C is the expected gesture but invisible, so the right-click menu
        // offers the same thing.
        bool copy = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteFocused);
        if (ImGui::BeginPopupContextWindow("##logmenu"))
        {
            if (ImGui::MenuItem(loc::TL("common.copy"), "Ctrl+C", false, m_selection.Size > 0))
                copy = true;
            if (ImGui::MenuItem(loc::TL("common.select_all"), "Ctrl+A", false, !m_visible.empty()))
                for (int i = 0; i < (int)m_visible.size(); ++i)
                    m_selection.SetItemSelected((ImGuiID)m_visible[i], true);
            ImGui::EndPopup();
        }

        if (copy && m_selection.Size > 0)
        {
            // Walk rows, not the selection storage, so the clipboard comes out
            // in the order the lines are shown.
            std::string text;
            for (int row = 0; row < (int)m_visible.size(); ++row)
            {
                const int index = m_visible[row];
                if (!m_selection.Contains((ImGuiID)index))
                    continue;
                text += Tag(entries[index].level);
                text += entries[index].text;
                text += '\n';
            }
            ImGui::SetClipboardText(text.c_str());
        }

        if (state.auto_scroll_log && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();
}
