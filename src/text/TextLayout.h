#pragma once

#include <string>
#include <vector>

namespace text
{
    const unsigned int kFallbackCodepoint = 0x3Fu;
    const unsigned int kMaxTextBytes = 4096u;
    const unsigned int kMaxGlyphs = 2048u;

    struct Glyph
    {
        unsigned int codepoint;
        float advance;
        float bearingX;
        float bearingY;
        float width;
        float height;
        float u0, v0, u1, v1;

        Glyph();
    };

    struct KerningPair
    {
        unsigned int left;
        unsigned int right;
        float advance;

        KerningPair();
    };

    struct FontMetrics
    {
        float sourcePixelSize;
        float ascent;
        float descent;
        float lineHeight;
        std::vector<Glyph> glyphs;
        std::vector<KerningPair> kerning;

        FontMetrics();
    };

    struct PositionedGlyph
    {
        unsigned int glyphIndex;
        float x;
        float y;

        PositionedGlyph();
    };

    struct Layout
    {
        float width;
        float height;
        bool truncated;
        std::vector<PositionedGlyph> glyphs;

        Layout();
    };

    // Decodes one codepoint and advances offset. Invalid UTF-8 and codepoints
    // outside Basic Latin / Latin-1 Supplement map to '?'.
    unsigned int DecodeLatin1(const std::string& value, unsigned int& offset);

    int FindGlyph(const FontMetrics& font, unsigned int codepoint);
    float FindKerning(const FontMetrics& font, unsigned int left, unsigned int right);

    // maxWidth <= 0 disables wrapping. Coordinates and bounds are in pixels.
    // Whitespace starts a new line only when a following word would overflow;
    // words wider than maxWidth wrap at glyph boundaries.
    bool BuildLayout(const FontMetrics& font, const std::string& value,
                     float pixelSize, float maxWidth, Layout& output);
}