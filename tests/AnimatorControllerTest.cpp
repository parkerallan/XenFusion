#include "anim/AnimatorController.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;
    AnimatorController controller;
    std::string error;
    if (!animator::LoadController(argv[1], controller, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    if (controller.default_state != "Survey" || controller.clips.size() != 3 ||
        controller.states.size() != 3)
        return 1;

    const fs::path round_trip = fs::temp_directory_path() / "xenfusion_animator_test.anim";
    if (!animator::SaveController(round_trip, controller, error))
        return 1;
    AnimatorController reloaded;
    if (!animator::LoadController(round_trip, reloaded, error))
        return 1;
    std::error_code remove_error;
    fs::remove(round_trip, remove_error);
    return reloaded.default_state == controller.default_state &&
           reloaded.clips.size() == controller.clips.size() &&
           reloaded.states.size() == controller.states.size() ? 0 : 1;
}