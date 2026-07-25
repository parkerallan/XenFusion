#include "anim/AnimatorRuntime.h"

#include <iostream>

namespace
{
    int Fail(const char* message)
    {
        std::cerr << "AnimatorRuntimeTest: " << message << '\n';
        return 1;
    }
}

int main()
{
    AnimatorController controller;
    controller.default_state = "Idle";
    controller.states.push_back({"Idle", "idle", 1.0f, true});
    controller.states.push_back({"Walk", "walk", 2.0f, true});
    controller.states.push_back({"Run", "run", 1.0f, true});
    controller.transitions.push_back({"Idle", "Walk", "speed >= 1.0", 0.25f, true, 0.5f});
    controller.transitions.push_back({"Walk", "Run", "run", 0.1f, false, 0.0f});

    AnimatorRuntimeState state;
    animator::InitializeRuntime(controller, "", state);
    animator::SetFloat(state, "speed", 2.0f);
    if (animator::AdvanceRuntime(controller, 0.25f, 1.0f, true, state))
        return Fail("transition ignored exit time");
    if (!animator::AdvanceRuntime(controller, 0.25f, 1.0f, true, state) || state.active_state != "Walk")
        return Fail("float condition did not transition");
    if (state.previous_state != "Idle" || state.blend_remaining != 0.25f)
        return Fail("blend bookkeeping was not captured");

    animator::SetTrigger(state, "run");
    if (!animator::AdvanceRuntime(controller, 0.0f, 1.0f, true, state) || state.active_state != "Run")
        return Fail("trigger did not transition");
    if (state.triggers.find("run") != state.triggers.end())
        return Fail("trigger was not consumed");

    animator::SetBool(state, "grounded", false);
    if (!animator::EvaluateCondition(state, "!grounded") ||
        !animator::EvaluateCondition(state, "grounded == false"))
        return Fail("bool conditions failed");
    return 0;
}