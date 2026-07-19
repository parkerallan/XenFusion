#pragma once

#include <filesystem>

struct EngineState;

// Editor-only "Mapping" panel: binds PC keyboard/mouse inputs to Xbox controls so
// scripts (which poll Xbox controls) can be driven from the keyboard/mouse in the
// Play preview. Two columns — left: Xbox control, right: the bound PC input.
// Persisted per project as input_mappings.ini. The console runtime ignores this.
class MapperPanel
{
public:
    void Render(EngineState& state);

private:
    void Load(EngineState& state);
    void Save(EngineState& state) const;

    int  listening_ = -1;    // row currently capturing a binding, or -1
    bool armed_     = false; // skip the frame capture started (the initiating click)
    std::filesystem::path loaded_project_;
};
