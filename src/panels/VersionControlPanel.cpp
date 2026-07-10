#include "panels/VersionControlPanel.h"

#include "state/EngineState.h"
#include "ui/Icons.h"

#include "imgui.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    std::string Trim(std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        std::size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        return s.substr(i);
    }
}

bool VersionControlPanel::RunGit(const std::string& args, std::string& output) const
{
    // -C runs git as if started in the project root (no chdir needed).
    const std::string cmd = "git -C \"" + git_root_.string() + "\" " + args + " 2>&1";
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe)
    {
        output = "Failed to launch git.";
        return false;
    }
    output.clear();
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe))
        output += buf;
    return _pclose(pipe) == 0;
}

void VersionControlPanel::Refresh(EngineState& /*state*/)
{
    changes_.clear();

    std::string out;
    if (RunGit("status --porcelain", out))
    {
        std::istringstream ss(out);
        std::string line;
        while (std::getline(ss, line))
        {
            if (line.size() < 4)
                continue;
            const std::string status = line.substr(0, 2);
            const std::string path   = line.substr(3);
            changes_.push_back({status, path});
            if (!selection_.count(path))
                selection_[path] = true; // stage everything by default
        }
    }

    std::string branch;
    if (RunGit("rev-parse --abbrev-ref HEAD", branch))
        current_branch_ = Trim(branch);
    else
        current_branch_.clear();
}

void VersionControlPanel::Render(EngineState& state)
{
    if (!state.show_version_control_panel)
        return;

    if (!ImGui::Begin("Version Control", &state.show_version_control_panel))
    {
        ImGui::End();
        return;
    }

    if (!state.HasProject())
    {
        ImGui::TextDisabled("No project open.");
        ImGui::End();
        return;
    }

    if (git_root_ != state.project_root)
    {
        git_root_ = state.project_root;
        selection_.clear();
        refresh_requested_ = true;
    }

    std::error_code ec;
    has_repo_ = fs::exists(git_root_ / ".git", ec);

    if (!has_repo_)
    {
        ImGui::TextWrapped("This project folder is not a git repository.");
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_CODE_BRANCH " Initialize Repository"))
        {
            std::string out;
            if (RunGit("init", out))
                state.AddLog("Initialized git repository");
            else
                state.AddLog("git init failed: " + out);
            refresh_requested_ = true;
        }
        ImGui::End();
        return;
    }

    if (refresh_requested_)
    {
        Refresh(state);
        refresh_requested_ = false;
    }

    // Toolbar: branch + sync actions.
    ImGui::Text(ICON_FA_CODE_BRANCH " %s", current_branch_.empty() ? "(detached)" : current_branch_.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
        refresh_requested_ = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Pull"))
    {
        RunGit("pull --rebase", status_message_);
        state.AddLog("git pull:\n" + status_message_);
        refresh_requested_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Push"))
    {
        RunGit("push", status_message_);
        state.AddLog("git push:\n" + status_message_);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Fetch"))
    {
        RunGit("fetch --all --prune", status_message_);
        state.AddLog("git fetch:\n" + status_message_);
        refresh_requested_ = true;
    }

    ImGui::Separator();

    // Changed files.
    ImGui::BeginChild("##changes", ImVec2(0.0f, -96.0f), ImGuiChildFlags_Borders);
    if (changes_.empty())
    {
        ImGui::TextDisabled("No changes.");
    }
    else
    {
        for (const Change& c : changes_)
        {
            ImGui::PushID(c.path.c_str());
            bool& sel = selection_[c.path];
            ImGui::Checkbox("##sel", &sel);
            ImGui::SameLine();
            ImGui::TextUnformatted(c.status.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(c.path.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // Commit box.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##commitmsg", "Commit message", commit_message_.data(), commit_message_.size());

    bool any_selected = false;
    for (const Change& c : changes_)
        if (selection_[c.path]) { any_selected = true; break; }

    const bool can_commit = any_selected && commit_message_[0] != '\0';
    ImGui::BeginDisabled(!can_commit);
    if (ImGui::Button(ICON_FA_CODE_BRANCH " Commit"))
    {
        for (const Change& c : changes_)
            if (selection_[c.path])
            {
                std::string out;
                RunGit("add -- \"" + c.path + "\"", out);
            }

        std::string msg = commit_message_.data();
        for (std::size_t i = 0; i < msg.size(); ++i)
            if (msg[i] == '"') msg[i] = '\''; // avoid breaking the quoted arg

        std::string out;
        if (RunGit("commit -m \"" + msg + "\"", out))
            state.AddLog("Committed: " + msg);
        else
            state.AddLog("git commit failed:\n" + out);

        commit_message_ = {};
        refresh_requested_ = true;
    }
    ImGui::EndDisabled();

    ImGui::End();
}
