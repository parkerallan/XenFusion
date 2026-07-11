#pragma once

struct EngineState;

// Persist the user's settings (theme, toolchain paths, log filters, panel
// visibility, ...) to a JSON config file next to the exe.
namespace settings
{
    void Load(EngineState& state); // read <exe>/settings.json (no-op if absent)
    void Save(const EngineState& state);
}
