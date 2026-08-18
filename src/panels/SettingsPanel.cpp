#include "panels/SettingsPanel.h"

#include "app/Settings.h"
#include "loc/Loc.h"
#include "state/EngineState.h"

#include "imgui.h"

#include <cstdio>
#include <string>
#include <vector>

void SettingsPanel::Render(EngineState& state)
{
    if (!state.show_settings_panel)
        return;

    if (!ImGui::Begin(loc::TWin("panel.settings.title", "Settings"), &state.show_settings_panel))
    {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText(loc::T("settings.toolchain.header"));

    // A read-only path field (can't be typed into) + a Browse button that asks
    // EngineApplication to open the Windows folder picker for that entry.
    auto path_row = [&](const char* label, const std::string& value, int pick_id)
    {
        ImGui::PushID(pick_id);
        ImGui::TextUnformatted(label);

        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", value.empty() ? loc::T("common.not_set") : value.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::SetNextItemWidth(-90.0f); // leave room for the button
        ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::Button(loc::TL("common.browse")))
            state.toolchain_pick = pick_id;
        ImGui::PopID();
    };

    path_row(loc::T("settings.toolchain.xdk"),      state.toolchain_xdk,      1);
    path_row(loc::T("settings.toolchain.emulator"), state.toolchain_emulator, 2);

    ImGui::SeparatorText(loc::T("settings.build.header"));

    // Base folder for all console build artifacts (.xex, deploy\, .iso, obj).
    // Keeps builds out of the engine's source tree. Empty = the runtime\ folder.
    path_row(loc::T("settings.build.output_dir"), state.build_output_dir, 3);
    if (state.build_output_dir.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(loc::T("settings.build.output_dir_default"));
        ImGui::PopStyleColor();
    }

    // Release / Debug for the .xex build. Persisted immediately on change.
    // MSBuild configuration identifiers, not prose — untranslated.
    ImGui::TextUnformatted(loc::T("settings.build.config"));
    ImGui::SetNextItemWidth(160.0f);
    const bool isDebug = (state.build_config == "Debug");
    if (ImGui::BeginCombo("##buildconfig", isDebug ? "Debug" : "Release"))
    {
        if (ImGui::Selectable("Release", !isDebug)) { state.build_config = "Release"; settings::Save(state); }
        if (ImGui::Selectable("Debug",    isDebug)) { state.build_config = "Debug";   settings::Save(state); }
        ImGui::EndCombo();
    }

    // Disc image filename (editable). Empty = "<project>.iso". Saved when the
    // field loses focus after an edit.
    ImGui::TextUnformatted(loc::T("settings.build.iso_name"));
    char isobuf[256];
    std::snprintf(isobuf, sizeof(isobuf), "%s", state.build_iso_name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##isoname", loc::T("settings.build.iso_name_hint"), isobuf, sizeof(isobuf)))
        state.build_iso_name = isobuf;
    if (ImGui::IsItemDeactivatedAfterEdit())
        settings::Save(state);

    // The list is whatever <exe>/lang holds. Applied next frame.
    ImGui::SeparatorText(loc::T("settings.language.header"));
    const std::vector<loc::LanguageInfo>& languages = loc::Available();
    if (languages.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(loc::T("settings.language.none"));
        ImGui::PopStyleColor();
    }
    else
    {
        const std::string& current = loc::Current();
        const char* preview = current.c_str();
        for (size_t i = 0; i < languages.size(); ++i)
            if (languages[i].code == current)
                preview = languages[i].name.c_str();

        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("##language", preview))
        {
            for (size_t i = 0; i < languages.size(); ++i)
            {
                const bool selected = (languages[i].code == current);
                if (ImGui::Selectable(languages[i].name.c_str(), selected))
                {
                    state.language = languages[i].code;
                    settings::Save(state);
                    loc::RequestLanguage(state.language);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SeparatorText(loc::T("settings.log_filters.header"));
    ImGui::Checkbox(loc::TL("settings.log_filters.info"),    &state.log_show_info);
    ImGui::SameLine(); ImGui::Checkbox(loc::TL("settings.log_filters.warning"), &state.log_show_warning);
    ImGui::Checkbox(loc::TL("settings.log_filters.error"),   &state.log_show_error);
    ImGui::SameLine(); ImGui::Checkbox(loc::TL("settings.log_filters.build"),   &state.log_show_build);
    ImGui::SameLine(); ImGui::Checkbox(loc::TL("settings.log_filters.script"),  &state.log_show_script);

    ImGui::End();
}
