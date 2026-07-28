#include "text/TextLayout.h"

#include <algorithm>

namespace text
{
    Glyph::Glyph()
        : codepoint(0), advance(0), bearingX(0), bearingY(0), width(0), height(0),
          u0(0), v0(0), u1(0), v1(0) {}

    KerningPair::KerningPair() : left(0), right(0), advance(0) {}

    FontMetrics::FontMetrics()
        : sourcePixelSize(1.0f), ascent(0), descent(0), lineHeight(1.0f) {}

    PositionedGlyph::PositionedGlyph() : glyphIndex(0), x(0), y(0) {}

    Layout::Layout() : width(0), height(0), truncated(false) {}

    static bool IsSupported(unsigned int codepoint)
    {
        return (codepoint >= 0x20u && codepoint <= 0x7Eu) ||
               (codepoint >= 0xA0u && codepoint <= 0xFFu) ||
               codepoint == '\n';
    }

    unsigned int DecodeLatin1(const std::string& value, unsigned int& offset)
    {
        if (offset >= value.size())
            return 0;

        const unsigned char first = (unsigned char)value[offset++];
        if (first < 0x80u)
            return IsSupported(first) ? first : kFallbackCodepoint;

        unsigned int codepoint = 0;
        unsigned int continuationCount = 0;
        if ((first & 0xE0u) == 0xC0u)
        {
            codepoint = first & 0x1Fu;
            continuationCount = 1;
            if (codepoint < 2u) return kFallbackCodepoint; // overlong
        }
        else if ((first & 0xF0u) == 0xE0u)
        {
            codepoint = first & 0x0Fu;
            continuationCount = 2;
        }
        else if ((first & 0xF8u) == 0xF0u)
        {
            codepoint = first & 0x07u;
            continuationCount = 3;
        }
        else
            return kFallbackCodepoint;

        for (unsigned int index = 0; index < continuationCount; ++index)
        {
            if (offset >= value.size())
                return kFallbackCodepoint;
            const unsigned char next = (unsigned char)value[offset];
            if ((next & 0xC0u) != 0x80u)
                return kFallbackCodepoint;
            ++offset;
            codepoint = (codepoint << 6) | (next & 0x3Fu);
        }

        if ((continuationCount == 2 && codepoint < 0x800u) ||
            (continuationCount == 3 && codepoint < 0x10000u) ||
            codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu))
            return kFallbackCodepoint;
        return IsSupported(codepoint) ? codepoint : kFallbackCodepoint;
    }

    int FindGlyph(const FontMetrics& font, unsigned int codepoint)
    {
        for (size_t index = 0; index < font.glyphs.size(); ++index)
            if (font.glyphs[index].codepoint == codepoint)
                return (int)index;
        if (codepoint != kFallbackCodepoint)
            return FindGlyph(font, kFallbackCodepoint);
        return -1;
    }

    float FindKerning(const FontMetrics& font, unsigned int left, unsigned int right)
    {
        for (size_t index = 0; index < font.kerning.size(); ++index)
            if (font.kerning[index].left == left && font.kerning[index].right == right)
                return font.kerning[index].advance;
        return 0.0f;
    }

    struct Token
    {
        std::vector<unsigned int> codepoints;
        bool newline;
        bool whitespace;
        Token() : newline(false), whitespace(false) {}
    };

    static void DecodeTokens(const std::string& value, std::vector<Token>& tokens)
    {
        unsigned int offset = 0;
        while (offset < value.size() && offset < kMaxTextBytes)
        {
            const unsigned int codepoint = DecodeLatin1(value, offset);
            Token token;
            if (codepoint == '\n')
            {
                token.newline = true;
                tokens.push_back(token);
                continue;
            }

            token.whitespace = codepoint == ' ' || codepoint == '\t';
            token.codepoints.push_back(token.whitespace ? (unsigned int)' ' : codepoint);
            while (offset < value.size() && offset < kMaxTextBytes)
            {
                const unsigned int savedOffset = offset;
                const unsigned int next = DecodeLatin1(value, offset);
                const bool nextWhitespace = next == ' ' || next == '\t';
                if (next == '\n' || nextWhitespace != token.whitespace)
                {
                    offset = savedOffset;
                    break;
                }
                token.codepoints.push_back(token.whitespace ? (unsigned int)' ' : next);
            }
            tokens.push_back(token);
        }
    }

    static float Measure(const FontMetrics& font, const std::vector<unsigned int>& codepoints,
                         float scale, unsigned int previous)
    {
        float width = 0.0f;
        for (size_t index = 0; index < codepoints.size(); ++index)
        {
            const int glyphIndex = FindGlyph(font, codepoints[index]);
            if (glyphIndex < 0) continue;
            width += FindKerning(font, previous, codepoints[index]) * scale;
            width += font.glyphs[glyphIndex].advance * scale;
            previous = codepoints[index];
        }
        return width;
    }

    bool BuildLayout(const FontMetrics& font, const std::string& value,
                     float pixelSize, float maxWidth, Layout& output)
    {
        output = Layout();
        if (font.sourcePixelSize <= 0.0f || font.lineHeight <= 0.0f || pixelSize <= 0.0f)
            return false;

        const float scale = pixelSize / font.sourcePixelSize;
        const float lineHeight = font.lineHeight * scale;
        std::vector<Token> tokens;
        DecodeTokens(value, tokens);
        float penX = 0.0f;
        float penY = font.ascent * scale;
        float maxLineWidth = 0.0f;
        unsigned int previous = 0;
        bool lineHasGlyph = false;
        unsigned int lineCount = 1;

        for (size_t tokenIndex = 0; tokenIndex < tokens.size(); ++tokenIndex)
        {
            const Token& token = tokens[tokenIndex];
            if (token.newline)
            {
                maxLineWidth = (std::max)(maxLineWidth, penX);
                penX = 0.0f; penY += lineHeight; previous = 0;
                lineHasGlyph = false; ++lineCount;
                continue;
            }

            const float tokenWidth = Measure(font, token.codepoints, scale, previous);
            if (maxWidth > 0.0f && lineHasGlyph && !token.whitespace && penX + tokenWidth > maxWidth)
            {
                maxLineWidth = (std::max)(maxLineWidth, penX);
                penX = 0.0f; penY += lineHeight; previous = 0;
                lineHasGlyph = false; ++lineCount;
            }
            if (token.whitespace && !lineHasGlyph)
                continue;

            for (size_t codepointIndex = 0; codepointIndex < token.codepoints.size(); ++codepointIndex)
            {
                const unsigned int codepoint = token.codepoints[codepointIndex];
                const int glyphIndex = FindGlyph(font, codepoint);
                if (glyphIndex < 0) continue;
                const Glyph& glyph = font.glyphs[glyphIndex];
                const float kern = FindKerning(font, previous, codepoint) * scale;
                if (maxWidth > 0.0f && lineHasGlyph && !token.whitespace &&
                    penX + kern + glyph.advance * scale > maxWidth)
                {
                    maxLineWidth = (std::max)(maxLineWidth, penX);
                    penX = 0.0f; penY += lineHeight; previous = 0;
                    lineHasGlyph = false; ++lineCount;
                }
                penX += FindKerning(font, previous, codepoint) * scale;
                if (codepoint != ' ')
                {
                    if (output.glyphs.size() >= kMaxGlyphs)
                    {
                        output.truncated = true;
                        output.width = (std::max)(maxLineWidth, penX);
                        output.height = lineCount * lineHeight;
                        return true;
                    }
                    PositionedGlyph positioned;
                    positioned.glyphIndex = (unsigned int)glyphIndex;
                    positioned.x = penX + glyph.bearingX * scale;
                    positioned.y = penY - glyph.bearingY * scale;
                    output.glyphs.push_back(positioned);
                    lineHasGlyph = true;
                }
                penX += glyph.advance * scale;
                previous = codepoint;
            }
        }

        output.width = (std::max)(maxLineWidth, penX);
        output.height = lineCount * lineHeight;
        return true;
    }
}