#pragma once

#include "anim/AnimationClip.h"

#include <string>
#include <vector>

namespace animation
{
    bool EncodeClipBE(const AnimationClip& clip, std::vector<unsigned char>& output,
                      std::string& error);
    bool DecodeClipBE(const unsigned char* data, size_t size, AnimationClip& output,
                      std::string& error);
}