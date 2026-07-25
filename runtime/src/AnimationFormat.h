#pragma once

namespace animfmt
{
    const unsigned int kMagicV1 = 0x414E4D31; // 'ANM1'
    const unsigned int kMagicV2 = 0x414E4D32; // 'ANM2'
    const unsigned int kVersionV1 = 1;
    const unsigned int kVersionV2 = 2;
    const unsigned int kHeaderBytes = 32;
    const unsigned int kTrackBytes = 56;
    const unsigned int kSampleBytes = 20;
}