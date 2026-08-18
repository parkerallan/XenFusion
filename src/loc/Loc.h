#pragma once

#include <string>
#include <vector>

// Engine UI localization. Language files live in <exe>/lang/*.json, organised
// as nested sections; code addresses a string by the dotted path through that
// tree (T("menu.file.exit")) and the loader flattens each file once at load.
// Keys beginning with '_' are reserved metadata.
//
// en.json is the base table and is REQUIRED: with symbolic keys there is no
// English left in the source, so a key missing from it renders as its own
// dotted path.
namespace loc
{
    struct LanguageInfo
    {
        std::string code; // filename stem, e.g. "es"
        std::string name; // "_name", e.g. "Espanol"
        std::string font; // "_font"; empty when Latin-1 suffices
    };

    // "_font" of the active language — the face shipped in <exe>/fonts that
    // covers its script. Empty for Latin-1 languages.
    const std::string& FontFile();

    // "_glyphs" of the active language ("japanese", "korean", ...). A whole
    // script, not just the glyphs the translations use: the user can type in
    // their own language and those characters must rasterize too.
    const std::string& GlyphSet();

    // Codepoints above U+00FF the translations use, merged on top of the range
    // above so a string reaching outside the standard script still renders.
    const std::vector<unsigned int>& ExtendedCodepoints();

    // Call once, after settings::Load and before any UI is built.
    void Init(const std::string& code);

    const std::vector<LanguageInfo>& Available();
    const std::string&               Current();

    // Queues a switch; the tables are untouched until Apply(), so a const char*
    // handed out earlier in the frame cannot dangle. Apply() runs at the top of
    // the frame loop, before ImGui's NewFrame.
    void RequestLanguage(const std::string& code);
    void Apply();

    // Active translation, else the English base, else the key itself.
    // Valid until the next Apply().
    //
    // T   — plain text (Text, SeparatorText, tooltips); carries no ImGui ID.
    // TL  — interactive widgets; appends "###key" so the ID survives a language
    //       change and two labels translating alike cannot collide.
    // TI  — TL with a Font Awesome glyph in front, for ICON_FA_X " Words".
    // TWin— panel windows; the ID is given explicitly and must stay the
    //       window's original English title, since docking, SetWindowFocus and
    //       imgui.ini all key off it.
    const char* T(const char* key);
    const char* TL(const char* key);
    const char* TI(const char* icon, const char* key);
    const char* TWin(const char* key, const char* stable_id);
}
