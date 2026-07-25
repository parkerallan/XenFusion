#include "anim/AnimatorRuntime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
    std::string Trim(const std::string& value)
    {
        size_t first = 0;
        while (first < value.size() && std::isspace((unsigned char)value[first])) ++first;
        size_t last = value.size();
        while (last > first && std::isspace((unsigned char)value[last - 1])) --last;
        return value.substr(first, last - first);
    }

    const AnimatorStateDefinition* FindState(const AnimatorController& controller,
                                              const std::string& name)
    {
        for (const AnimatorStateDefinition& state : controller.states)
            if (state.name == name) return &state;
        return nullptr;
    }

    bool ParseBool(const std::string& token, bool& value)
    {
        if (token == "true" || token == "True" || token == "TRUE") { value = true; return true; }
        if (token == "false" || token == "False" || token == "FALSE") { value = false; return true; }
        return false;
    }
}

namespace animator
{
    bool EvaluateCondition(AnimatorRuntimeState& state, const std::string& condition)
    {
        const std::string expression = Trim(condition);
        if (expression.empty()) return true;

        const char* operators[] = {"==", "!=", ">=", "<=", ">", "<"};
        for (const char* op : operators)
        {
            const size_t position = expression.find(op);
            if (position == std::string::npos) continue;
            const std::string name = Trim(expression.substr(0, position));
            const std::string right = Trim(expression.substr(position + std::strlen(op)));
            if (name.empty() || right.empty()) return false;

            const auto bool_value = state.bool_parameters.find(name);
            if (bool_value != state.bool_parameters.end())
            {
                bool rhs = false;
                if (!ParseBool(right, rhs)) return false;
                return std::strcmp(op, "==") == 0 ? bool_value->second == rhs
                     : std::strcmp(op, "!=") == 0 ? bool_value->second != rhs : false;
            }
            const auto float_value = state.float_parameters.find(name);
            if (float_value == state.float_parameters.end()) return false;
            char* end = nullptr;
            const float rhs = std::strtof(right.c_str(), &end);
            if (end == right.c_str() || *end != '\0') return false;
            const float lhs = float_value->second;
            if (std::strcmp(op, "==") == 0) return std::fabs(lhs - rhs) <= 0.0001f;
            if (std::strcmp(op, "!=") == 0) return std::fabs(lhs - rhs) > 0.0001f;
            if (std::strcmp(op, ">=") == 0) return lhs >= rhs;
            if (std::strcmp(op, "<=") == 0) return lhs <= rhs;
            if (std::strcmp(op, ">") == 0) return lhs > rhs;
            return lhs < rhs;
        }

        if (expression[0] == '!')
        {
            const std::string name = Trim(expression.substr(1));
            const auto value = state.bool_parameters.find(name);
            if (value != state.bool_parameters.end()) return !value->second;
            return state.triggers.find(name) == state.triggers.end();
        }
        const auto value = state.bool_parameters.find(expression);
        if (value != state.bool_parameters.end()) return value->second;
        const auto trigger = state.triggers.find(expression);
        if (trigger != state.triggers.end())
        {
            state.triggers.erase(trigger);
            return true;
        }
        return false;
    }

    void InitializeRuntime(const AnimatorController& controller, const std::string& initial_state,
                           AnimatorRuntimeState& state)
    {
        state = AnimatorRuntimeState();
        state.active_state = FindState(controller, initial_state) ? initial_state : controller.default_state;
        if (!FindState(controller, state.active_state) && !controller.states.empty())
            state.active_state = controller.states.front().name;
    }

    bool AdvanceRuntime(const AnimatorController& controller, float delta_time,
                        float object_speed, bool auto_play, AnimatorRuntimeState& state)
    {
        const AnimatorStateDefinition* active = FindState(controller, state.active_state);
        if (!active) return false;
        if (auto_play)
        {
            state.state_time += delta_time * (std::max)(0.0f, object_speed) *
                               (std::max)(0.0f, active->playback_speed);
            if (!state.previous_state.empty())
                state.previous_state_time += delta_time * (std::max)(0.0f, object_speed) *
                                             state.previous_state_speed;
        }
        if (state.blend_remaining > 0.0f)
        {
            state.blend_remaining = (std::max)(0.0f, state.blend_remaining - delta_time);
            if (state.blend_remaining == 0.0f)
            {
                state.previous_state.clear();
                state.previous_state_time = 0.0f;
                state.blend_duration = 0.0f;
            }
        }

        for (const AnimatorTransitionDefinition& transition : controller.transitions)
        {
            if (transition.from_state != state.active_state) continue;
            if (transition.has_exit_time && state.state_time < transition.exit_time) continue;
            if (!EvaluateCondition(state, transition.condition)) continue;
            if (!FindState(controller, transition.to_state)) continue;

            state.previous_state = state.active_state;
            state.previous_state_time = state.state_time;
            state.previous_state_speed = (std::max)(0.0f, active->playback_speed);
            state.blend_duration = (std::max)(0.0f, transition.blend_duration);
            state.blend_remaining = state.blend_duration;
            if (state.blend_duration == 0.0f) state.previous_state.clear();
            state.active_state = transition.to_state;
            state.state_time = 0.0f;
            return true;
        }
        return false;
    }

    void SetFloat(AnimatorRuntimeState& state, const std::string& name, float value)
    {
        state.float_parameters[name] = value;
        state.bool_parameters.erase(name);
    }
    void SetBool(AnimatorRuntimeState& state, const std::string& name, bool value)
    {
        state.bool_parameters[name] = value;
        state.float_parameters.erase(name);
    }
    void SetTrigger(AnimatorRuntimeState& state, const std::string& name)
    {
        state.triggers.insert(name);
    }
}