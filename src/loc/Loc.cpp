#include "loc/Loc.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <windows.h> // GetModuleFileNameW

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;
using nlohmann::json;

namespace
{
    typedef std::map<std::string, std::string> Table;

    Table g_base;    // en.json — the fallback for every lookup
    Table g_overlay; // selected language; empty while English is active
    Table g_composed; // "label###id" strings handed out by TL and TWin

    std::vector<loc::LanguageInfo> g_available;
    std::string g_current = "en";
    std::string g_pending;
    bool        g_has_pending = false;

    std::string               g_font;      // "_font" of the active language
    std::string               g_glyphset;  // "_glyphs" of the active language
    std::vector<unsigned int> g_extended;  // codepoints > U+00FF it uses

    const char* const kBaseCode = "en";

    fs::path LangDir()
    {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return fs::path(std::wstring(buf, n)).parent_path() / "lang";
    }

    // Depth-first walk: nested objects extend the dotted path.
    void Flatten(const json& node, const std::string& prefix,
                 const std::string& file, Table& out)
    {
        for (json::const_iterator it = node.begin(); it != node.end(); ++it)
        {
            const std::string& name = it.key();
            if (!name.empty() && name[0] == '_') // reserved metadata
                continue;

            const std::string path = prefix.empty() ? name : prefix + "." + name;

            if (it->is_object())
            {
                Flatten(*it, path, file, out);
            }
            else if (it->is_string())
            {
                if (!out.insert(std::make_pair(path, it->get<std::string>())).second)
                    applog::Warn("Localization: duplicate key \"" + path + "\" in " + file);
            }
            else if (it->is_array())
            {
                // Stored the way ImGui::Combo wants its items: NUL-separated.
                std::string joined;
                bool ok = true;
                for (json::const_iterator e = it->begin(); e != it->end(); ++e)
                {
                    if (!e->is_string()) { ok = false; break; }
                    joined += e->get<std::string>();
                    joined += '\0';
                }
                if (!ok)
                    applog::Warn("Localization: \"" + path + "\" in " + file +
                                 " must be a list of strings");
                else if (!out.insert(std::make_pair(path, joined)).second)
                    applog::Warn("Localization: duplicate key \"" + path + "\" in " + file);
            }
            else
            {
                applog::Warn("Localization: \"" + path + "\" in " + file +
                             " is neither a section, a string nor a list");
            }
        }
    }

    // Conversion characters of a printf string: "%.2f ms (%d)" -> "fd".
    std::string Specifiers(const std::string& value)
    {
        std::string out;
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] != '%')
                continue;
            if (i + 1 < value.size() && value[i + 1] == '%') { ++i; continue; }

            size_t j = i + 1;
            while (j < value.size() && std::strchr("-+ #0123456789.*", value[j]))
                ++j; // flags, width, precision
            while (j < value.size() && std::strchr("hlLjzt", value[j]))
                ++j; // length modifiers
            if (j < value.size())
                out += value[j];
            i = j;
        }
        return out;
    }

    // Mismatched specifiers are a varargs crash; drop the entry so English wins.
    void DropSpecifierMismatches(Table& overlay, const std::string& file)
    {
        for (Table::iterator it = overlay.begin(); it != overlay.end(); )
        {
            const Table::const_iterator base = g_base.find(it->first);
            if (base != g_base.end() && Specifiers(base->second) != Specifiers(it->second))
            {
                applog::Warn("Localization: \"" + it->first + "\" in " + file +
                             " has mismatched format specifiers; using English");
                it = overlay.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Codepoints above U+00FF used by a table. The JSON is already validated,
    // so this decode assumes well-formed UTF-8.
    void CollectExtended(const Table& table, std::vector<unsigned int>& out)
    {
        for (Table::const_iterator it = table.begin(); it != table.end(); ++it)
        {
            const std::string& s = it->second;
            for (size_t i = 0; i < s.size(); )
            {
                const unsigned char c = (unsigned char)s[i];
                unsigned int cp = c;
                size_t len = 1;
                if      ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu; len = 2; }
                else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; len = 3; }
                else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; len = 4; }

                if (i + len > s.size()) break;
                for (size_t k = 1; k < len; ++k)
                    cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3Fu);
                i += len;

                if (cp > 0xFFu)
                    out.push_back(cp);
            }
        }
    }

    // display_name, font and glyphset receive the reserved "_" keys if given.
    bool LoadFile(const fs::path& path, Table* out, std::string* display_name,
                  std::string* font = 0, std::string* glyphset = 0)
    {
        const std::string name = path.filename().string();

        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;

        json j;
        try { in >> j; }
        catch (const std::exception& e)
        {
            applog::Error("Localization: " + name + " failed to parse: " + e.what());
            return false;
        }
        if (!j.is_object())
        {
            applog::Error("Localization: " + name + " is not a JSON object");
            return false;
        }

        if (display_name)
            *display_name = j.value("_name", std::string());
        if (font)
            *font = j.value("_font", std::string());
        if (glyphset)
            *glyphset = j.value("_glyphs", std::string());
        if (out)
            Flatten(j, std::string(), name, *out);
        return true;
    }

    void ScanAvailable()
    {
        g_available.clear();

        std::error_code ec;
        const fs::path dir = LangDir();
        if (!fs::is_directory(dir, ec))
        {
            applog::Error("Localization: no lang folder at " + dir.string());
            return;
        }

        for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".json")
                continue;

            std::string display, font;
            if (!LoadFile(entry.path(), nullptr, &display, &font))
                continue;

            loc::LanguageInfo info;
            info.code = entry.path().stem().string();
            info.name = display.empty() ? info.code : display;
            info.font = font;
            g_available.push_back(info);
        }

        // English first, then alphabetical by the name shown in the dropdown.
        std::sort(g_available.begin(), g_available.end(),
                  [](const loc::LanguageInfo& a, const loc::LanguageInfo& b)
                  {
                      if ((a.code == kBaseCode) != (b.code == kBaseCode))
                          return a.code == kBaseCode;
                      return a.name < b.name;
                  });
    }

    bool IsAvailable(const std::string& code)
    {
        for (size_t i = 0; i < g_available.size(); ++i)
            if (g_available[i].code == code)
                return true;
        return false;
    }

    // Includes the base table so English fallbacks stay renderable.
    void RefreshGlyphNeeds()
    {
        g_extended.clear();
        CollectExtended(g_base, g_extended);
        CollectExtended(g_overlay, g_extended);
        std::sort(g_extended.begin(), g_extended.end());
        g_extended.erase(std::unique(g_extended.begin(), g_extended.end()),
                         g_extended.end());
    }

    void LoadSelection(const std::string& code)
    {
        g_current = code;
        g_overlay.clear();
        g_composed.clear();
        g_font.clear();
        g_glyphset.clear();

        if (code == kBaseCode)
        {
            RefreshGlyphNeeds();
            return;
        }

        const fs::path path = LangDir() / (code + ".json");
        if (!LoadFile(path, &g_overlay, nullptr, &g_font, &g_glyphset))
        {
            applog::Error("Localization: could not load " + path.string() + "; using English");
            g_overlay.clear();
            g_font.clear();
            g_glyphset.clear();
            g_current = kBaseCode;
            RefreshGlyphNeeds();
            return;
        }
        DropSpecifierMismatches(g_overlay, path.filename().string());
        RefreshGlyphNeeds();
    }
}

namespace loc
{
    void Init(const std::string& code)
    {
        ScanAvailable();

        g_base.clear();
        const fs::path base = LangDir() / (std::string(kBaseCode) + ".json");
        if (!LoadFile(base, &g_base, nullptr))
        {
            // Every lookup would fall through to its own key, so the UI would
            // read as dotted paths.
            applog::Error("Localization: " + base.string() +
                          " is missing; the UI will show key names");
        }

        LoadSelection(IsAvailable(code) ? code : std::string(kBaseCode));
    }

    const std::vector<LanguageInfo>& Available() { return g_available; }
    const std::string&               Current()   { return g_current; }
    const std::string&               FontFile()  { return g_font; }
    const std::string&               GlyphSet()  { return g_glyphset; }

    const std::vector<unsigned int>& ExtendedCodepoints() { return g_extended; }

    void RequestLanguage(const std::string& code)
    {
        if (code == g_current)
            return;
        g_pending     = code;
        g_has_pending = true;
    }

    void Apply()
    {
        if (!g_has_pending)
            return;
        const std::string code = g_pending;
        g_has_pending = false;
        g_pending.clear();
        LoadSelection(code);
    }

    const char* T(const char* key)
    {
        if (!key)
            return "";

        Table::const_iterator it = g_overlay.find(key);
        if (it != g_overlay.end())
            return it->second.c_str();

        it = g_base.find(key);
        if (it != g_base.end())
            return it->second.c_str();

        return key; // caller's string literal: static lifetime
    }

    const char* TL(const char* key)
    {
        return key ? TWin(key, key) : "";
    }

    const char* TI(const char* icon, const char* key)
    {
        if (!key)
            return "";
        if (!icon)
            return TL(key);

        // '\x01' keeps this from colliding with a TWin entry sharing the key.
        const std::string cache_key = std::string("\x01") + icon + '\n' + key;

        const Table::const_iterator it = g_composed.find(cache_key);
        if (it != g_composed.end())
            return it->second.c_str();

        const std::string composed = std::string(icon) + " " + T(key) + "###" + key;
        return g_composed.insert(std::make_pair(cache_key, composed)).first->second.c_str();
    }

    const char* TWin(const char* key, const char* stable_id)
    {
        const std::string cache_key = std::string(stable_id) + '\n' + key;

        const Table::const_iterator it = g_composed.find(cache_key);
        if (it != g_composed.end())
            return it->second.c_str();

        const std::string composed = std::string(T(key)) + "###" + stable_id;
        return g_composed.insert(std::make_pair(cache_key, composed)).first->second.c_str();
    }
}
