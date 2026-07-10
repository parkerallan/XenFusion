#pragma once

#include <filesystem>

struct EngineState;

// Bottom panel — a tile/thumbnail browser for the project's assets/ folder.
// Folders are tiles you click into (with a breadcrumb to go back); files show a
// large type-specific visual (image / audio / model / shader-sphere / file).
class AssetsPanel
{
public:
    void Render(EngineState& state);

private:
    std::filesystem::path selected_; // highlighted asset
};
