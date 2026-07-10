#include "panels/LogPanel.h"

#include "state/EngineState.h"
#include "ui/Icons.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <string>

// Ported from the reference engine's LogPanel: an icon toolbar (clear /
// auto-scroll) beside a read-only multi-line text box so the log is
// selectable and copyable.
void LogPanel::Render(EngineState& state)
{
    if (!state.show_log_panel)
        return;

    if (!ImGui::Begin("Log", &state.show_log_panel))
    {
        ImGui::End();
        return;
    }

    // Flatten the messages into one buffer.
    std::string log_text;
    std::size_t total = 0;
    for (const std::string& m : state.log_messages)
        total += m.size() + 1;
    log_text.reserve(total);
    for (const std::string& m : state.log_messages)
    {
        log_text.append(m);
        log_text.push_back('\n');
    }

    const float button = ImGui::GetFrameHeight();
    const float toolbar_w = button; // tight column so the square buttons fill it

    if (ImGui::BeginTable("##LogLayout", 2,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_NoPadOuterX))
    {
        ImGui::TableSetupColumn("Toolbar", ImGuiTableColumnFlags_WidthFixed, toolbar_w);
        ImGui::TableSetupColumn("Log", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextColumn();
        // Zero horizontal frame padding so the icon isn't clipped/offset inside
        // the square button; center the glyph.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));

        if (ImGui::Button(ICON_FA_TRASH, ImVec2(button, button)))
            state.log_messages.clear();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Clear log");

        if (!state.auto_scroll_log)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button(ICON_FA_ARROW_DOWN, ImVec2(button, button)))
            state.auto_scroll_log = !state.auto_scroll_log;
        if (!state.auto_scroll_log)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Auto-scroll: %s", state.auto_scroll_log ? "On" : "Off");

        ImGui::PopStyleVar(2);

        ImGui::TableNextColumn();
        const ImGuiID text_id = ImGui::GetCurrentWindow()->GetID("##LogText");
        ImGui::InputTextMultiline("##LogText", log_text.data(), log_text.size() + 1,
                                  ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);

        if (state.auto_scroll_log)
        {
            char child_name[256];
            std::snprintf(child_name, sizeof(child_name), "Log/##LogText_%08X", text_id);
            if (ImGuiWindow* child = ImGui::FindWindowByName(child_name))
                ImGui::SetScrollY(child, child->ScrollMax.y);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
