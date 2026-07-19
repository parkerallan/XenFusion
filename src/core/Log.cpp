#include "core/Log.h"

#include <cstdio>

namespace applog
{
    static std::vector<LogEntry> g_entries;

    void Add(LogLevel level, const std::string& text)
    {
        g_entries.push_back({level, text});

        // Echo to the console (the app is a console-subsystem build, so this
        // shows up when running under a debugger / terminal).
        const char* tag = level == LogLevel::Error   ? "[ERROR]" :
                          level == LogLevel::Warning ? "[WARN] " :
                          level == LogLevel::Build   ? "[BUILD]" :
                          level == LogLevel::Script  ? "[LOG]  " : "[INFO] ";
        std::printf("%s %s\n", tag, text.c_str());
    }

    const std::vector<LogEntry>& Entries() { return g_entries; }

    void Clear() { g_entries.clear(); }
}
