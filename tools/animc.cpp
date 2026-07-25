#include "anim/AnimatorController.h"
#include "anim/AnimatorCooker.h"

#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: animc <project-root> <controller.anim> <output.anc>\n";
        return 2;
    }
    AnimatorController controller;
    std::string error;
    if (!animator::LoadController(argv[2], controller, error))
    { std::cerr << "animc: " << error << '\n'; return 1; }
    std::vector<unsigned char> payload;
    if (!animator::CookControllerBE(argv[1], controller, payload, error))
    { std::cerr << "animc: " << error << '\n'; return 1; }
    std::ofstream output(argv[3], std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(payload.data()), (std::streamsize)payload.size());
    if (!output) { std::cerr << "animc: cannot write output\n"; return 1; }
    std::cout << "animc: wrote " << payload.size() << " bytes\n";
    return 0;
}