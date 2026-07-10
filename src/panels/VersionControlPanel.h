#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct EngineState;

// Git version-control panel, docked next to Files. Drives the `git` CLI in the
// project root (mirrors the reference engine's VersionControlPanel): detect /
// init repo, list changes, stage + commit, pull / push / fetch.
class VersionControlPanel
{
public:
    void Render(EngineState& state);

private:
    struct Change
    {
        std::string status; // two-char porcelain code
        std::string path;
    };

    void Refresh(EngineState& state);
    bool RunGit(const std::string& args, std::string& output) const;

    std::filesystem::path git_root_;
    bool                  has_repo_ = false;
    bool                  refresh_requested_ = true;
    std::string           current_branch_;
    std::string           status_message_;
    std::vector<Change>   changes_;
    std::unordered_map<std::string, bool> selection_;
    std::array<char, 512> commit_message_{};
};
