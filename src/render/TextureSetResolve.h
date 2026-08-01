#pragma once

// Finds a material's texture maps among the files next to a model, for the
// slots the model file itself does not declare. Substance Painter names its output after the
// texture set material then channel, so MapIndex lists the
// nearby files once and resolve looks up likely names against it.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace texset
{
    // Preference order when one stem exists under several extensions.
    inline const std::vector<std::string>& Extensions()
    {
        static const std::vector<std::string> kExts =
            { ".png", ".tga", ".tif", ".tiff", ".jpg", ".jpeg", ".bmp", ".dds" };
        return kExts;
    }

    inline std::string Lower(std::string s)
    {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // -1 when the extension is not an image we can load.
    inline int ExtensionRank(const std::string& ext)
    {
        const std::string lower = Lower(ext);
        const std::vector<std::string>& exts = Extensions();
        for (std::size_t i = 0; i < exts.size(); ++i)
            if (exts[i] == lower)
                return (int)i;
        return -1;
    }

    // Stripped off a diffuse's name to recover the stem its siblings share.
    // Compound forms come first, or "Foo_Base_Color" reduces to "Foo_Base".
    inline const std::vector<std::string>& BaseColorSuffixes()
    {
        static const std::vector<std::string> kSuffixes =
            { "_BaseColor", "_Base_Color", "_BaseColour", "_Base_Colour",
              "_Albedo", "_Diffuse", "_DiffuseColor", "_Color", "_Colour" };
        return kSuffixes;
    }

    // The image files near a model, keyed by lowercased "<dir><stem>" so
    // lookups ignore case and extension. Values keep the real on-disk spelling,
    // since that is what goes into the .mesh blob.
    class MapIndex
    {
    public:
        // Scan one directory, non-recursive. `rel_dir` is relative to `root`,
        // empty or ending in '/'. Re-scanning a directory is a no-op.
        void AddDir(const std::filesystem::path& root, const std::string& rel_dir)
        {
            namespace fs = std::filesystem;
            if (!m_scanned.insert(Lower(rel_dir)).second)
                return; // already indexed

            std::error_code ec;
            const fs::path dir = rel_dir.empty() ? root : root / rel_dir;
            fs::directory_iterator it(dir, ec), end;
            if (ec) return;
            for (; it != end; it.increment(ec))
            {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                const int rank = ExtensionRank(it->path().extension().string());
                if (rank < 0) continue;

                const std::string key = Lower(rel_dir + it->path().stem().string());
                const std::string rel = rel_dir + it->path().filename().string();
                auto found = m_files.find(key);
                if (found == m_files.end())
                    m_files.emplace(key, Entry{ rel, rank });
                else if (rank < found->second.rank)
                    found->second = Entry{ rel, rank };
            }
        }

        // Relative path for "<rel_dir><stem>", or empty if there is no such file.
        std::string Lookup(const std::string& rel_dir, const std::string& stem) const
        {
            auto found = m_files.find(Lower(rel_dir + stem));
            return found == m_files.end() ? std::string() : found->second.rel;
        }

        bool Empty() const { return m_files.empty(); }

    private:
        struct Entry { std::string rel; int rank; };
        std::map<std::string, Entry> m_files;
        std::set<std::string>        m_scanned;
    };

    // "textures/HAIRa_Diffuse.png" -> dir "textures/", stem "HAIRa_Diffuse".
    // Takes either separator; an .mtl routinely writes backslashes.
    inline void SplitRel(const std::string& rel, std::string& dir, std::string& stem)
    {
        const std::size_t slash = rel.find_last_of("/\\");
        dir = (slash == std::string::npos) ? std::string() : rel.substr(0, slash + 1);
        for (char& c : dir) if (c == '\\') c = '/';
        const std::string name = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
        const std::size_t dot = name.find_last_of('.');
        stem = (dot == std::string::npos) ? name : name.substr(0, dot);
    }

    // The name as the importer gave it, plus a form with the characters an
    // exporter would have replaced when building a filename.
    inline std::vector<std::string> MaterialNameForms(const std::string& name)
    {
        std::vector<std::string> forms;
        if (name.empty()) return forms;
        forms.push_back(name);
        std::string sanitized = name;
        for (char& c : sanitized)
            if (c == ' ' || c == '.' || c == ':' || c == '/' || c == '\\') c = '_';
        if (sanitized != name) forms.push_back(sanitized);
        return forms;
    }

    // The map for one channel of one material, or empty if there is none.
    // `material` is the importer's material name, i.e. Substance's texture set;
    // `aliases` is a NULL-terminated list of channel spellings to accept, most
    // canonical first.
    //
    // Names are tried most-specific first:
    //   <diffuse's own path, channel swapped>   most reliable: the importer
    //                                           already proved that path right
    //   <mesh>_<material>_<channel>             Substance's stock template
    //   <material>_<channel>
    //   <mesh>_<channel>
    // each beside the model and under textures/.
    inline std::string Resolve(const MapIndex& index,
                               const std::string& diffuse_rel_in,
                               const std::string& mesh_stem,
                               const std::string& material,
                               const char* const* aliases)
    {
        // The comparison at the bottom has to see the same spelling the index
        // produces, and this can arrive with backslashes.
        std::string diffuse_rel = diffuse_rel_in;
        for (char& c : diffuse_rel) if (c == '\\') c = '/';

        struct Candidate { std::string dir, base; };
        std::vector<Candidate> candidates;

        // Keeps the diffuse's directory and texture-set name, both of which can
        // differ from the model's: brick_wall.obj ships brickwall_BaseColor.png.
        if (!diffuse_rel.empty())
        {
            std::string ddir, dstem;
            SplitRel(diffuse_rel, ddir, dstem);
            const std::string lower = Lower(dstem);
            for (const std::string& suffix : BaseColorSuffixes())
            {
                const std::string ls = Lower(suffix);
                if (lower.size() <= ls.size() ||
                    lower.compare(lower.size() - ls.size(), ls.size(), ls) != 0)
                    continue;
                candidates.push_back({ ddir, dstem.substr(0, dstem.size() - ls.size()) });
                break;
            }
        }

        std::vector<std::string> bases;
        for (const std::string& mat : MaterialNameForms(material))
        {
            if (!mesh_stem.empty()) bases.push_back(mesh_stem + "_" + mat);
            bases.push_back(mat);
        }
        if (!mesh_stem.empty()) bases.push_back(mesh_stem);

        static const char* const kDirs[] = { "", "textures/" };
        for (const std::string& base : bases)
            for (const char* dir : kDirs)
                candidates.push_back({ dir, base });

        for (const Candidate& c : candidates)
            for (const char* const* a = aliases; a && *a; ++a)
            {
                const std::string hit = index.Lookup(c.dir, c.base + "_" + *a);
                // A generic alias must not steal the base colour.
                if (!hit.empty() && Lower(hit) != Lower(diffuse_rel))
                    return hit;
            }
        return {};
    }
}
