#include "text/TextLayout.h"
#include "text/CookedFont.h"
#include "SpakFormat.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
bool Near(float left, float right) { return std::fabs(left - right) < 0.001f; }

text::FontMetrics MakeFont()
{
    text::FontMetrics font;
    font.sourcePixelSize = 10.0f;
    font.ascent = 8.0f;
    font.descent = -2.0f;
    font.lineHeight = 12.0f;
    for (unsigned int codepoint = 0x20; codepoint <= 0xFF; ++codepoint)
    {
        text::Glyph glyph;
        glyph.codepoint = codepoint;
        glyph.advance = codepoint == ' ' ? 3.0f : 6.0f;
        glyph.bearingY = 8.0f;
        glyph.width = 5.0f;
        glyph.height = 8.0f;
        font.glyphs.push_back(glyph);
    }
    text::KerningPair pair;
    pair.left = 'A'; pair.right = 'V'; pair.advance = -1.0f;
    font.kerning.push_back(pair);
    return font;
}

bool Check(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "TextLayoutTest: %s\n", message);
    return condition;
}

void PushU32BE(std::vector<unsigned char>& bytes, unsigned int value)
{
    bytes.push_back((unsigned char)(value >> 24));
    bytes.push_back((unsigned char)(value >> 16));
    bytes.push_back((unsigned char)(value >> 8));
    bytes.push_back((unsigned char)value);
}

void PushF32BE(std::vector<unsigned char>& bytes, float value)
{
    unsigned int bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    PushU32BE(bytes, bits);
}

std::vector<unsigned char> MakeCookedFont()
{
    std::vector<unsigned char> bytes;
    PushU32BE(bytes, spak::kFontMagic); PushU32BE(bytes, spak::kFontVersion);
    PushU32BE(bytes, 123u); PushU32BE(bytes, 64u); PushU32BE(bytes, 64u);
    PushF32BE(bytes, 10.0f); PushF32BE(bytes, 8.0f); PushF32BE(bytes, -2.0f);
    PushF32BE(bytes, 12.0f); PushF32BE(bytes, 5.0f);
    PushU32BE(bytes, 1u); PushU32BE(bytes, 0u);
    PushU32BE(bytes, '?'); PushF32BE(bytes, 6.0f); PushF32BE(bytes, 0.0f);
    PushF32BE(bytes, 8.0f); PushF32BE(bytes, 5.0f); PushF32BE(bytes, 8.0f);
    PushF32BE(bytes, 0.0f); PushF32BE(bytes, 0.0f); PushF32BE(bytes, 0.5f); PushF32BE(bytes, 0.5f);
    return bytes;
}
}

int main()
{
    text::FontMetrics font = MakeFont();
    unsigned int offset = 0;
    bool ok = true;
    ok &= Check(text::DecodeLatin1(std::string("\xC3\xA9"), offset) == 0xE9u, "Latin-1 UTF-8 decode");
    offset = 0;
    ok &= Check(text::DecodeLatin1(std::string("\xE2\x82\xAC"), offset) == '?', "unsupported Unicode fallback");
    offset = 0;
    ok &= Check(text::DecodeLatin1(std::string("\xC0\xAF"), offset) == '?', "overlong UTF-8 fallback");

    text::Layout layout;
    ok &= Check(text::BuildLayout(font, "AV", 10.0f, 0.0f, layout), "layout succeeds");
    ok &= Check(layout.glyphs.size() == 2 && Near(layout.width, 11.0f), "kerning affects width");

    ok &= Check(text::BuildLayout(font, "one two", 10.0f, 24.0f, layout), "wrapped layout succeeds");
    ok &= Check(layout.height == 24.0f && layout.glyphs.size() == 6, "word wrap and spaces");

    ok &= Check(text::BuildLayout(font, "ABCDE", 10.0f, 13.0f, layout), "long word layout succeeds");
    ok &= Check(layout.height == 36.0f, "long word wraps at glyph boundaries");

    ok &= Check(text::BuildLayout(font, "A\nB", 20.0f, 0.0f, layout), "newline layout succeeds");
    ok &= Check(layout.height == 48.0f && Near(layout.glyphs[1].y - layout.glyphs[0].y, 24.0f), "scaled line height");

    std::vector<unsigned char> cooked = MakeCookedFont();
    text::CookedFont parsed;
    ok &= Check(text::ParseCookedFont(&cooked[0], (unsigned int)cooked.size(), parsed), "cooked font parses");
    ok &= Check(parsed.atlasHash == 123u && parsed.metrics.glyphs.size() == 1, "cooked font fields");
    ok &= Check(!text::ParseCookedFont(&cooked[0], (unsigned int)cooked.size() - 1, parsed), "truncated font rejected");
    cooked[20] = 0x7F; cooked[21] = 0x80; cooked[22] = 0; cooked[23] = 0;
    ok &= Check(!text::ParseCookedFont(&cooked[0], (unsigned int)cooked.size(), parsed), "infinite metric rejected");

    return ok ? 0 : 1;
}