#pragma once

struct EngineState;

// Left panel — project file tree. Placeholder static layout for now; will be
// backed by a real project directory later.
class FilesPanel
{
public:
    void Render(EngineState& state);
};
