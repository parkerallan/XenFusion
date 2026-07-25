#pragma once

#include "anim/AnimatorController.h"

#include <filesystem>
#include <string>
#include <vector>

namespace animator
{
    bool CookControllerBE(const std::filesystem::path& project_root,
                          const AnimatorController& controller,
                          std::vector<unsigned char>& output, std::string& error);
}