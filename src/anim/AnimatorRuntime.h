#pragma once

#include "anim/AnimatorController.h"

#include <map>
#include <set>
#include <string>

struct AnimatorRuntimeState
{
    std::string active_state;
    std::string previous_state;
    float state_time = 0.0f;
    float previous_state_time = 0.0f;
    float previous_state_speed = 1.0f;
    float blend_duration = 0.0f;
    float blend_remaining = 0.0f;
    std::map<std::string, float> float_parameters;
    std::map<std::string, bool> bool_parameters;
    std::set<std::string> triggers;
};

namespace animator
{
    bool EvaluateCondition(AnimatorRuntimeState& state, const std::string& condition);
    void InitializeRuntime(const AnimatorController& controller, const std::string& initial_state,
                           AnimatorRuntimeState& state);
    bool AdvanceRuntime(const AnimatorController& controller, float delta_time,
                        float object_speed, bool auto_play, AnimatorRuntimeState& state);
    void SetFloat(AnimatorRuntimeState& state, const std::string& name, float value);
    void SetBool(AnimatorRuntimeState& state, const std::string& name, bool value);
    void SetTrigger(AnimatorRuntimeState& state, const std::string& name);
}