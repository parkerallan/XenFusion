#include "build/BuildRun.h"

#include "core/Log.h"
#include "state/EngineState.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    HANDLE      g_proc   = NULL;      // the running build_run.ps1 process
    HANDLE      g_read   = NULL;      // read handle on its output log
    long long   g_offset = 0;        // bytes of the log already consumed
    std::string g_partial;           // a not-yet-newline-terminated tail line

    // Emit one build-output line to the editor log (errors highlighted).
    void EmitLine(std::string line)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return;
        const bool err = line.find("ERROR")   != std::string::npos ||
                         line.find("FAILED")  != std::string::npos ||
                         line.find(": error") != std::string::npos;
        applog::Add(err ? LogLevel::Error : LogLevel::Build, "  " + line);
    }

    // Read whatever the build process has written since last time and log any
    // complete lines. PowerShell may emit UTF-16 (interleaved NULs) when its
    // stdout is redirected, so NULs are stripped.
    void DrainLog()
    {
        if (!g_read || g_read == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER pos; pos.QuadPart = g_offset;
        SetFilePointerEx(g_read, pos, NULL, FILE_BEGIN);

        char buf[4096];
        DWORD n = 0;
        while (ReadFile(g_read, buf, sizeof(buf), &n, NULL) && n > 0)
        {
            g_offset += n;
            for (DWORD i = 0; i < n; ++i)
                if (buf[i] != '\0') g_partial.push_back(buf[i]);

            std::size_t nl;
            while ((nl = g_partial.find('\n')) != std::string::npos)
            {
                EmitLine(g_partial.substr(0, nl));
                g_partial.erase(0, nl + 1);
            }
        }
    }
}

namespace
{
    // Launch one of the runtime PowerShell scripts detached, tailing its output.
    // `scriptName` is a file in RUNTIME_SOURCE_DIR; `extraArgs` are the wide args
    // appended after the script path; `logDir` is where buildrun.log is written
    // (the build output base). Returns false (and logs) on any failure.
    bool LaunchScript(const wchar_t* scriptName, const std::wstring& extraArgs,
                      const fs::path& logDir, const char* label)
    {
#ifndef RUNTIME_SOURCE_DIR
        applog::Error(std::string(label) + ": runtime source dir not configured (rebuild the editor with CMake).");
        return false;
#else
        const fs::path runtimeDir(RUNTIME_SOURCE_DIR);
        const fs::path script  = runtimeDir / scriptName;
        if (!fs::exists(script))
        { applog::Error(std::string(label) + ": " + script.filename().string() + " not found at " + script.string()); return false; }

        std::error_code mkec; fs::create_directories(logDir, mkec); // build output base may not exist yet
        const fs::path logPath = logDir / "buildrun.log";

        const std::wstring cmd =
            L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() +
            L"\" " + extraArgs;

        // The child writes stdout+stderr into logPath; we tail it every frame.
        SECURITY_ATTRIBUTES sa; sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
        HANDLE hLog = CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hLog == INVALID_HANDLE_VALUE)
        { applog::Error(std::string(label) + ": cannot create " + logPath.string()); return false; }

        STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hLog;
        si.hStdError  = hLog;
        PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

        std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back(0);
        const BOOL ok = CreateProcessW(NULL, cmdbuf.data(), NULL, NULL, TRUE,
                                       CREATE_NO_WINDOW, NULL, runtimeDir.wstring().c_str(), &si, &pi);
        CloseHandle(hLog);
        if (!ok) { applog::Error(std::string(label) + ": failed to launch the build process"); return false; }
        CloseHandle(pi.hThread);

        g_proc   = pi.hProcess;
        g_offset = 0;
        g_partial.clear();
        g_read = CreateFileW(logPath.wstring().c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        return true;
#endif
    }

    // The XDK-root arg shared by all runtime scripts (empty when unset -> the
    // scripts fall back to the machine XEDK).
    std::wstring XedkArg(const EngineState& state)
    {
        if (state.toolchain_xdk.empty()) return L"";
        return L" -Xedk \"" + fs::path(state.toolchain_xdk).wstring() + L"\"";
    }

    // The build output base: the configured folder, or runtime/ when unset.
    fs::path OutBase(const EngineState& state)
    {
#ifdef RUNTIME_SOURCE_DIR
        return state.build_output_dir.empty() ? fs::path(RUNTIME_SOURCE_DIR)
                                              : fs::path(state.build_output_dir);
#else
        return fs::path(state.build_output_dir);
#endif
    }

    // -Config (always) and -OutDir (only when set) args shared by all scripts.
    std::wstring BuildArgs(const EngineState& state)
    {
        const std::string cfg = state.build_config.empty() ? "Release" : state.build_config;
        std::wstring a = L" -Config " + std::wstring(cfg.begin(), cfg.end());
        if (!state.build_output_dir.empty())
            a += L" -OutDir \"" + fs::path(state.build_output_dir).wstring() + L"\"";
        return a;
    }
}

namespace buildrun
{
    bool Running() { return g_proc != NULL; }

    void Start(EngineState& state)
    {
        if (g_proc) { applog::Warn("Build & Run: already in progress"); return; }
        if (!state.HasProject()) { applog::Error("Build & Run: open a project first"); return; }
        if (state.toolchain_emulator.empty())
        { applog::Error("Build & Run: set the Xenia path in Settings > Toolchain"); return; }

        const std::wstring args =
            L"-Project \"" + state.project_root.wstring() +
            L"\" -Xenia \"" + fs::path(state.toolchain_emulator).wstring() + L"\"" +
            XedkArg(state) + BuildArgs(state);
        if (LaunchScript(L"build_run.ps1", args, OutBase(state), "Build & Run"))
            applog::Build("Build & Run: building '" + state.project_name + "' -> .xex -> Xenia...");
    }

    void StartIso(EngineState& state)
    {
        if (g_proc) { applog::Warn("Build .iso: a build is already in progress"); return; }
        if (!state.HasProject()) { applog::Error("Build .iso: open a project first"); return; }
        if (state.toolchain_xdk.empty())
        { applog::Error("Build .iso: set the XDK path in Settings > Toolchain (XDiscBld lives there)"); return; }

        std::wstring args = L"-Project \"" + state.project_root.wstring() + L"\"" +
                            XedkArg(state) + BuildArgs(state);
        if (!state.build_iso_name.empty())
            args += L" -IsoName \"" + std::wstring(state.build_iso_name.begin(), state.build_iso_name.end()) + L"\"";
        if (LaunchScript(L"build_iso.ps1", args, OutBase(state), "Build .iso"))
            applog::Build("Build .iso: building '" + state.project_name + "' -> .xex -> XDVDFS .iso...");
    }

    void Poll(EngineState&)
    {
        if (!g_proc) return;
        DrainLog();
        if (WaitForSingleObject(g_proc, 0) != WAIT_OBJECT_0)
            return; // still running

        DrainLog(); // catch anything written between the last read and exit
        if (!g_partial.empty()) { EmitLine(g_partial); g_partial.clear(); }

        DWORD code = 0;
        GetExitCodeProcess(g_proc, &code);
        if (code == 0) applog::Build("Build & Run: done.");
        else           applog::Error("Build & Run: FAILED (see output above).");

        CloseHandle(g_proc); g_proc = NULL;
        if (g_read && g_read != INVALID_HANDLE_VALUE) { CloseHandle(g_read); g_read = NULL; }
    }

    void Clean(EngineState& state)
    {
        if (g_proc) { applog::Warn("Clean Build: a build is in progress"); return; }
#ifndef RUNTIME_SOURCE_DIR
        applog::Error("Clean Build: runtime source dir not configured.");
#else
        // Clean the configured build output base (or runtime/ when unset). The base
        // folder itself is left in place - only build artifacts inside it go.
        const fs::path base = OutBase(state);
        const char* targets[] = { "Release", "Debug", "deploy", "buildrun.log", "game.xgd" };
        const int nTargets = (int)(sizeof(targets) / sizeof(targets[0]));
        int removed = 0;
        // Remove any produced disc images (<name>.iso) too.
        {
            std::error_code ec;
            for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().extension() == ".iso")
                {
                    std::error_code rec;
                    fs::remove(it->path(), rec);
                    if (!rec) { applog::Build("Clean Build: removed " + it->path().filename().string()); ++removed; }
                }
        }
        for (int i = 0; i < nTargets; ++i)
        {
            std::error_code ec;
            const fs::path p = base / targets[i];
            if (!fs::exists(p, ec)) continue;
            fs::remove_all(p, ec);
            if (ec) applog::Error("Clean Build: could not remove " + p.string() + " (" + ec.message() + ")");
            else  { applog::Build("Clean Build: removed " + std::string(targets[i])); ++removed; }
        }
        applog::Build(removed ? "Clean Build: done." : "Clean Build: nothing to remove (already clean).");
#endif
    }
}
