#pragma once

struct EngineState;

// Bottom panel — frame timing.
class PerformancePanel
{
public:
    void Render(EngineState& state);

private:
    static constexpr int kHistory = 120;
    float frame_times_[kHistory] = {};
    int   offset_ = 0;
};
