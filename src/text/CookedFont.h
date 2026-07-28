#pragma once

#include "text/TextLayout.h"

namespace text
{
    struct CookedFont
    {
        unsigned int atlasHash;
        unsigned int atlasWidth;
        unsigned int atlasHeight;
        float sdfSpread;
        FontMetrics metrics;

        CookedFont();
    };

    bool ParseCookedFont(const unsigned char* data, unsigned int size, CookedFont& output);
}