#pragma once

#include <memory>

struct EngineState;

// Right panel — properties of the object selected in the Hierarchy.
class InspectorPanel
{
public:
    InspectorPanel();
    ~InspectorPanel();

    void Render(EngineState& state);

private:
    struct VideoImportJob;
    std::unique_ptr<VideoImportJob> video_import_;

    void PollVideoImport(EngineState& state);
};
