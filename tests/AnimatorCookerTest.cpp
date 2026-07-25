#include "anim/AnimationCodec.h"
#include "anim/AnimatorController.h"
#include "anim/AnimatorCooker.h"
#include "AnimatorCookFormat.h"

#include <iostream>

namespace
{
    unsigned int ReadU32(const unsigned char* data)
    {
        return ((unsigned int)data[0] << 24) | ((unsigned int)data[1] << 16) |
               ((unsigned int)data[2] << 8) | data[3];
    }
}

int main(int argc, char** argv)
{
    if (argc != 3) return 1;
    AnimatorController controller;
    std::string error;
    if (!animator::LoadController(argv[2], controller, error)) return 2;
    std::vector<unsigned char> payload;
    if (!animator::CookControllerBE(argv[1], controller, payload, error)) return 3;
    if (payload.size() < animcook::kHeaderBytes || ReadU32(payload.data()) != animcook::kMagic ||
        ReadU32(payload.data() + 20) != 3) return 4;
    const unsigned int clip_table = ReadU32(payload.data() + 32);
    for (unsigned int clip = 0; clip < 3; ++clip)
    {
        const unsigned char* record = payload.data() + clip_table + clip * animcook::kClipBytes;
        const unsigned int offset = ReadU32(record + 8);
        const unsigned int size = ReadU32(record + 12);
        AnimationClip decoded;
        if ((size_t)offset + size > payload.size() ||
            !animation::DecodeClipBE(payload.data() + offset, size, decoded, error)) return 5;
    }
    std::cout << "ANC1 controller: " << payload.size() << " bytes, 3 clips\n";
    return 0;
}