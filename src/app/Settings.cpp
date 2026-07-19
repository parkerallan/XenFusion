#include "app/Settings.h"

#include "state/EngineState.h"

#include <nlohmann/json.hpp>

#include <windows.h> // GetModuleFileNameW

#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace
{
    fs::path SettingsPath()
    {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return fs::path(std::wstring(buf, n)).parent_path() / "settings.json";
    }
}

namespace settings
{
    void Save(const EngineState& s)
    {
        json j;
        j["theme"]           = (int)s.theme;
        j["clear_color"]     = { s.clear_color[0], s.clear_color[1], s.clear_color[2], s.clear_color[3] };
        j["auto_scroll_log"] = s.auto_scroll_log;
        j["log"] = {
            {"info",    s.log_show_info},
            {"warning", s.log_show_warning},
            {"error",   s.log_show_error},
            {"build",   s.log_show_build},
            {"script",  s.log_show_script},
        };
        j["toolchain"] = {
            {"xdk",      s.toolchain_xdk},
            {"emulator", s.toolchain_emulator},
        };
        j["build"] = {
            {"output_dir", s.build_output_dir},
            {"config",     s.build_config},
            {"iso_name",   s.build_iso_name},
        };
        j["panels"] = {
            {"viewport",        s.show_viewport_panel},
            {"inspector",       s.show_inspector_panel},
            {"files",           s.show_files_panel},
            {"version_control", s.show_version_control_panel},
            {"assets",          s.show_assets_panel},
            {"settings",        s.show_settings_panel},
            {"log",             s.show_log_panel},
            {"performance",     s.show_performance_panel},
            {"editor",          s.show_editor_panel},
        };

        std::ofstream out(SettingsPath(), std::ios::binary | std::ios::trunc);
        if (out)
            out << j.dump(2) << '\n';
    }

    void Load(EngineState& s)
    {
        std::ifstream in(SettingsPath(), std::ios::binary);
        if (!in)
            return;
        json j;
        try { in >> j; }
        catch (const std::exception&) { return; }

        s.theme = (Theme)j.value("theme", (int)s.theme);
        if (j.contains("clear_color") && j["clear_color"].is_array() && j["clear_color"].size() == 4)
            for (int i = 0; i < 4; ++i) s.clear_color[i] = j["clear_color"][i].get<float>();
        s.auto_scroll_log = j.value("auto_scroll_log", s.auto_scroll_log);

        if (j.contains("log"))
        {
            const json& l = j["log"];
            s.log_show_info    = l.value("info",    s.log_show_info);
            s.log_show_warning = l.value("warning", s.log_show_warning);
            s.log_show_error   = l.value("error",   s.log_show_error);
            s.log_show_build   = l.value("build",   s.log_show_build);
            s.log_show_script  = l.value("script",  s.log_show_script);
        }
        if (j.contains("toolchain"))
        {
            const json& t = j["toolchain"];
            s.toolchain_xdk      = t.value("xdk",      std::string());
            s.toolchain_emulator = t.value("emulator", std::string());
        }
        if (j.contains("build"))
        {
            const json& b = j["build"];
            s.build_output_dir = b.value("output_dir", s.build_output_dir);
            s.build_config     = b.value("config",     s.build_config);
            s.build_iso_name   = b.value("iso_name",   s.build_iso_name);
        }
        if (j.contains("panels"))
        {
            const json& p = j["panels"];
            s.show_viewport_panel        = p.value("viewport",        s.show_viewport_panel);
            s.show_inspector_panel       = p.value("inspector",       s.show_inspector_panel);
            s.show_files_panel           = p.value("files",           s.show_files_panel);
            s.show_version_control_panel = p.value("version_control", s.show_version_control_panel);
            s.show_assets_panel          = p.value("assets",          s.show_assets_panel);
            s.show_settings_panel        = p.value("settings",        s.show_settings_panel);
            s.show_log_panel             = p.value("log",             s.show_log_panel);
            s.show_performance_panel     = p.value("performance",     s.show_performance_panel);
            s.show_editor_panel          = p.value("editor",          s.show_editor_panel);
        }
    }
}
