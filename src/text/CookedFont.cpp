#include "text/CookedFont.h"
#include "../../runtime/src/SpakFormat.h"

#include <string.h>

namespace text
{
    CookedFont::CookedFont()
        : atlasHash(0), atlasWidth(0), atlasHeight(0), sdfSpread(0) {}

    static unsigned int ReadU32BE(const unsigned char* data)
    {
        return ((unsigned int)data[0] << 24) | ((unsigned int)data[1] << 16) |
               ((unsigned int)data[2] << 8) | (unsigned int)data[3];
    }

    static bool ReadFiniteF32BE(const unsigned char* data, float& value)
    {
        const unsigned int bits = ReadU32BE(data);
        if ((bits & 0x7F800000u) == 0x7F800000u)
            return false;
        memcpy(&value, &bits, sizeof(value));
        return true;
    }

    static bool HasExactGlyph(const FontMetrics& metrics, unsigned int codepoint)
    {
        for (size_t index = 0; index < metrics.glyphs.size(); ++index)
            if (metrics.glyphs[index].codepoint == codepoint)
                return true;
        return false;
    }

    bool ParseCookedFont(const unsigned char* data, unsigned int size, CookedFont& output)
    {
        output = CookedFont();
        if (!data || size < spak::kFontHeaderBytes ||
            ReadU32BE(data + 0) != spak::kFontMagic ||
            ReadU32BE(data + 4) != spak::kFontVersion)
            return false;

        const unsigned int glyphCount = ReadU32BE(data + 40);
        const unsigned int kerningCount = ReadU32BE(data + 44);
        if (glyphCount == 0 || glyphCount > spak::kMaxFontGlyphs ||
            kerningCount > spak::kMaxFontKerning)
            return false;
        const unsigned int glyphBytes = glyphCount * spak::kFontGlyphBytes;
        const unsigned int kernBytes = kerningCount * spak::kFontKernBytes;
        if (glyphBytes / spak::kFontGlyphBytes != glyphCount ||
            kernBytes / spak::kFontKernBytes != kerningCount ||
            glyphBytes > size - spak::kFontHeaderBytes ||
            kernBytes != size - spak::kFontHeaderBytes - glyphBytes)
            return false;

        output.atlasHash = ReadU32BE(data + 8);
        output.atlasWidth = ReadU32BE(data + 12);
        output.atlasHeight = ReadU32BE(data + 16);
        if (output.atlasHash == 0 || output.atlasWidth == 0 || output.atlasHeight == 0 ||
            output.atlasWidth > 4096 || output.atlasHeight > 4096 ||
            !ReadFiniteF32BE(data + 20, output.metrics.sourcePixelSize) ||
            !ReadFiniteF32BE(data + 24, output.metrics.ascent) ||
            !ReadFiniteF32BE(data + 28, output.metrics.descent) ||
            !ReadFiniteF32BE(data + 32, output.metrics.lineHeight) ||
            !ReadFiniteF32BE(data + 36, output.sdfSpread) ||
            output.metrics.sourcePixelSize <= 0.0f || output.metrics.lineHeight <= 0.0f ||
            output.sdfSpread <= 0.0f)
            return false;

        output.metrics.glyphs.reserve(glyphCount);
        const unsigned char* cursor = data + spak::kFontHeaderBytes;
        unsigned int previousCodepoint = 0;
        for (unsigned int index = 0; index < glyphCount; ++index, cursor += spak::kFontGlyphBytes)
        {
            Glyph glyph;
            glyph.codepoint = ReadU32BE(cursor + 0);
            if (glyph.codepoint <= previousCodepoint ||
                !ReadFiniteF32BE(cursor + 4, glyph.advance) ||
                !ReadFiniteF32BE(cursor + 8, glyph.bearingX) ||
                !ReadFiniteF32BE(cursor + 12, glyph.bearingY) ||
                !ReadFiniteF32BE(cursor + 16, glyph.width) ||
                !ReadFiniteF32BE(cursor + 20, glyph.height) ||
                !ReadFiniteF32BE(cursor + 24, glyph.u0) ||
                !ReadFiniteF32BE(cursor + 28, glyph.v0) ||
                !ReadFiniteF32BE(cursor + 32, glyph.u1) ||
                !ReadFiniteF32BE(cursor + 36, glyph.v1) ||
                glyph.advance < 0.0f || glyph.width < 0.0f || glyph.height < 0.0f ||
                glyph.width > output.atlasWidth || glyph.height > output.atlasHeight ||
                glyph.u0 < 0.0f || glyph.v0 < 0.0f || glyph.u1 > 1.0f || glyph.v1 > 1.0f ||
                glyph.u1 < glyph.u0 || glyph.v1 < glyph.v0)
                return false;
            previousCodepoint = glyph.codepoint;
            output.metrics.glyphs.push_back(glyph);
        }
        if (!HasExactGlyph(output.metrics, kFallbackCodepoint))
            return false;

        output.metrics.kerning.reserve(kerningCount);
        for (unsigned int index = 0; index < kerningCount; ++index, cursor += spak::kFontKernBytes)
        {
            KerningPair pair;
            pair.left = ReadU32BE(cursor + 0);
            pair.right = ReadU32BE(cursor + 4);
            if (!ReadFiniteF32BE(cursor + 8, pair.advance) ||
                !HasExactGlyph(output.metrics, pair.left) ||
                !HasExactGlyph(output.metrics, pair.right))
                return false;
            output.metrics.kerning.push_back(pair);
        }
        return true;
    }
}