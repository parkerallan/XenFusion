#include "render/ShaderCompiler.h"

#include "core/Log.h"
#include "render/Shader.h" // BakeToCso

#include <cctype>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace shadercompiler
{
    void CompileAll(const fs::path& standard_hlsl, const fs::path& project_root)
    {
        if (project_root.empty())
        {
            applog::Warn("Open a project to compile shaders");
            return;
        }
        const fs::path out = project_root / "shaders";
        int ok = 0, fail = 0;

        // 1) the engine's built-in standard shader.
        std::string err;
        if (shader::BakeToCso(standard_hlsl, out, err)) ++ok;
        else { ++fail; applog::Error("standard.hlsl: " + (err.empty() ? "compile failed" : err)); }

        // 2) every project shader (.hlsl anywhere under the project, except output).
        std::error_code ec;
        for (const fs::directory_entry& de : fs::recursive_directory_iterator(project_root, ec))
        {
            if (!de.is_regular_file(ec)) continue;
            const fs::path p = de.path();
            std::string ext = p.extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext != ".hlsl") continue;
            if (p.parent_path() == out) continue; // skip anything already in shaders/

            std::string e;
            if (shader::BakeToCso(p, out, e)) ++ok;
            else { ++fail; applog::Error(p.filename().string() + ": " + (e.empty() ? "compile failed" : e)); }
        }

        applog::Build("Compiled " + std::to_string(ok) + " shader(s)" +
                      (fail ? ", " + std::to_string(fail) + " failed" : "") + " -> " + out.string());
    }
}
